#include "TimelineView.logic.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace gocue::recorder
{
namespace
{
juce::String k(const char* s) { return juce::String::fromUTF8(s); }
template<class T> bool contains(const std::vector<T>& v, const T& id) { return std::find(v.begin(), v.end(), id) != v.end(); }
bool audio(const Track& t) { return t.kind == TrackKind::mic || t.kind == TrackKind::importAudio; }
juce::Result blocked() { return juce::Result::fail(k("녹화 중에는 구조 편집과 스크럽을 사용할 수 없습니다. 프리뷰와 마커 추가는 가능합니다.")); }
ClipEditResult failure(const RecorderProject& p, const juce::String& reason)
{ ClipEditResult r(p); r.status = juce::Result::fail(reason); return r; }
}
juce::String TimelineEditController::text(TimelineAction a)
{
    switch (a)
    {
        case TimelineAction::split: return k("스플릿");
        case TimelineAction::trimIn: return k("앞 트림");
        case TimelineAction::trimOut: return k("뒤 트림");
        case TimelineAction::remove: return k("삭제");
        case TimelineAction::rippleAll: case TimelineAction::rippleAudio: return k("구간 삭제하고 당기기");
        case TimelineAction::move: return k("이동");
        case TimelineAction::earlier: return k("앞으로");
        case TimelineAction::later: return k("뒤로");
        case TimelineAction::unlink: return k("링크 해제");
        case TimelineAction::link: return k("선택 클립 링크");
        case TimelineAction::undo: return k("실행취소");
        case TimelineAction::redo: return k("다시실행");
        case TimelineAction::addMarker: return k("마커 추가");
        case TimelineAction::editMarker: return k("마커 변경");
        case TimelineAction::deleteMarker: return k("마커 삭제");
        case TimelineAction::mute: return k("음소거");
        case TimelineAction::solo: return k("솔로");
        case TimelineAction::seek: return k("이동");
        case TimelineAction::hideTrack: return juce::String::fromUTF8("트랙 숨기기");
        case TimelineAction::showTrack: return juce::String::fromUTF8("트랙 보이기");
    }
    return {};
}
bool TimelineEditController::enabled(TimelineAction a) const
{
    if (a == TimelineAction::addMarker) return true;
    if (isLocked()) return false;
    switch (a)
    {
        case TimelineAction::undo: return document.getHistory().undoEntry() != nullptr;
        case TimelineAction::redo: return document.getHistory().redoEntry() != nullptr;
        case TimelineAction::rippleAll: return range.has_value();
        case TimelineAction::rippleAudio: return range.has_value() && !trackIds.empty();
        case TimelineAction::remove: return range.has_value() || !targets().empty();
        case TimelineAction::link: return targets().size() > 1;
        case TimelineAction::seek: case TimelineAction::editMarker: case TimelineAction::deleteMarker:
        case TimelineAction::mute: case TimelineAction::solo:
        case TimelineAction::hideTrack: case TimelineAction::showTrack: return true;
        default: return !targets().empty();
    }
}
juce::String TimelineEditController::historyText(bool redo) const
{
    const auto* entry = redo ? document.getHistory().redoEntry() : document.getHistory().undoEntry();
    return text(redo ? TimelineAction::redo : TimelineAction::undo) + (entry ? ": " + entry->name : juce::String());
}
juce::String TimelineEditController::editName(TimelineAction a) const
{
    const auto ids = targets(); juce::String name;
    if (!ids.empty()) if (const auto* c = document.getProject().findClip(ids.front()))
        for (const auto& t : document.getProject().tracks) if (t.trackId == c->trackId) name = t.name;
    if (a == TimelineAction::trimIn || a == TimelineAction::trimOut) return name + k(" 트림");
    if (a == TimelineAction::move || a == TimelineAction::split || a == TimelineAction::unlink) return name + " " + text(a);
    return text(a);
}
std::vector<Id> TimelineEditController::expandLinks(const RecorderProject& p, const std::vector<Id>& ids)
{
    std::vector<Id> result;
    const auto add = [&](const Id& id) { if (const auto* c = p.findClip(id); c && p.isActive(*c) && !contains(result, id)) result.push_back(id); };
    for (const auto& id : ids) add(id);
    for (const auto& group : p.linkGroups)
        if (std::any_of(group.clipIds.begin(), group.clipIds.end(), [&](const Id& id) { return contains(result, id); }))
            for (const auto& id : group.clipIds) add(id);
    return result;
}
void TimelineEditController::reconcileSelection()
{
    const auto& p = document.getProject();
    if (publishedSelection != document.getSelection())
    {
        std::vector<Id> seeds;
        for (const auto& id : explicitClips) if (contains(document.getSelection(), id)) seeds.push_back(id);
        // Split/delete/undo may replace IDs. Keep the directly clicked lane as the
        // unlink intent; a model result's expanded selection is not a new click.
        if (seeds.empty()) for (const auto& id : document.getSelection())
            if (const auto* c = p.findClip(id); c && c->trackId == primaryTrack) seeds.push_back(id);
        if (seeds.empty()) for (const auto& id : document.getSelection())
            if (!contains(expandLinks(p, seeds), id)) seeds.push_back(id);
        explicitClips = std::move(seeds);
        if (!contains(explicitClips, focus)) focus = explicitClips.empty() ? Id() : explicitClips.front();
        publishedSelection = document.getSelection();
    }
    trackIds.erase(std::remove_if(trackIds.begin(), trackIds.end(), [&](const Id& id)
    { return std::none_of(p.tracks.begin(), p.tracks.end(), [&](const Track& t) { return t.trackId == id; }); }), trackIds.end());
}
void TimelineEditController::clickClip(const Id& id, bool shift, bool control)
{
    cancelDrag(); reconcileSelection(); const auto& p = document.getProject(); const auto* c = p.findClip(id);
    if (!c || !p.isActive(*c)) return;
    if (shift && p.findClip(anchor) && p.findClip(anchor)->trackId == c->trackId)
    {
        const auto* first = p.findClip(anchor); const auto lo = std::min(first->timelineStartSample, c->timelineStartSample);
        const auto hi = std::max(first->timelineStartSample, c->timelineStartSample);
        if (!control) explicitClips.clear();
        for (const auto& t : p.tracks) if (t.trackId == c->trackId) for (const auto& item : t.clips.items())
            if (p.isActive(item) && item.timelineStartSample >= lo && item.timelineStartSample <= hi && !contains(explicitClips, item.clipId)) explicitClips.push_back(item.clipId);
    }
    else if (control)
    {
        const auto it = std::find(explicitClips.begin(), explicitClips.end(), id);
        if (it == explicitClips.end()) explicitClips.push_back(id); else explicitClips.erase(it);
    }
    else explicitClips = {id};
    focus = contains(explicitClips, id) ? id : explicitClips.empty() ? Id() : explicitClips.front();
    if (!shift) anchor = id;
    primaryTrack = focus.isEmpty() ? Id() : p.findClip(focus)->trackId;
    if (focus.isNotEmpty()) { explicitClips.erase(std::remove(explicitClips.begin(), explicitClips.end(), focus), explicitClips.end()); explicitClips.insert(explicitClips.begin(), focus); }
    publishedSelection = expandLinks(p, explicitClips); document.setSelection(publishedSelection); range.reset();
}
void TimelineEditController::clearSelection()
{ cancelDrag(); explicitClips.clear(); focus.clear(); anchor.clear(); primaryTrack.clear(); publishedSelection.clear(); document.setSelection({}); }
std::vector<Id> TimelineEditController::targets() const
{
    auto ids = expandLinks(document.getProject(), document.getSelection());
    if (contains(ids, focus)) { ids.erase(std::remove(ids.begin(), ids.end(), focus), ids.end()); ids.insert(ids.begin(), focus); }
    return ids;
}
void TimelineEditController::selectTrack(const Id& id, bool additive)
{
    const auto& ts = document.getProject().tracks;
    if (std::none_of(ts.begin(), ts.end(), [&](const Track& t) { return t.trackId == id && audio(t); })) return;
    if (!additive) trackIds = {id};
    else if (contains(trackIds, id)) trackIds.erase(std::remove(trackIds.begin(), trackIds.end(), id), trackIds.end());
    else trackIds.push_back(id);
}
void TimelineEditController::setRange(Sample a, Sample b)
{ if (a < 0 || b < 0 || a == b) range.reset(); else range = SampleRange{std::min(a, b), std::max(a, b) - std::min(a, b)}; }
juce::Result TimelineEditController::seek(Sample at)
{
    if (isLocked()) return blocked();
    if (at < 0) return juce::Result::fail(k("위치는 0 이상의 샘플로 입력하세요."));
    cursor = at; if (onSeek) onSeek(at, true); return juce::Result::ok();
}
std::vector<Id> TimelineEditController::neighbour(const RecorderProject& p, const std::vector<Id>& ids, bool later) const
{
    if (ids.empty()) return {};
    const auto lane = p.findClip(ids.front())->trackId;
    Sample first = (std::numeric_limits<Sample>::max)(), last = 0;
    for (const auto& id : ids) { const auto* c = p.findClip(id); first = std::min(first, c->timelineStartSample); last = std::max(last, c->timelineEnd()); }
    const Clip* candidate = nullptr;
    for (const auto& t : p.tracks) if (t.trackId == lane) for (const auto& c : t.clips.items())
    {
        if (!p.isActive(c) || contains(ids, c.clipId)) continue;
        if (later ? c.timelineStartSample >= last : c.timelineEnd() <= first)
            if (!candidate || (later ? c.timelineStartSample < candidate->timelineStartSample : c.timelineEnd() > candidate->timelineEnd())) candidate = &c;
    }
    return candidate ? expandLinks(p, {candidate->clipId}) : std::vector<Id>{};
}
ClipEditResult TimelineEditController::apply(const RecorderProject& p, TimelineAction a, const std::vector<Id>& ids, Sample at, bool exact) const
{
    switch (a)
    {
        case TimelineAction::split: return ClipEdits::split(p, ids, cursor);
        case TimelineAction::trimIn: return ClipEdits::trimIn(p, ids, at, !exact, !exact);
        case TimelineAction::trimOut: return ClipEdits::trimOut(p, ids, at, !exact, !exact);
        case TimelineAction::move:
            if (ids.empty() || at < 0) break;
            return ClipEdits::move(p, ids, at - p.findClip(ids.front())->timelineStartSample, !exact, !exact);
        case TimelineAction::remove:
            if (range) return ids.empty() ? ClipEdits::remove(p, *range) : ClipEdits::remove(p, ids, *range);
            return ClipEdits::remove(p, ids);
        case TimelineAction::rippleAll: if (range) return ClipEdits::rippleDeleteAll(p, *range); break;
        case TimelineAction::rippleAudio: if (range) return ClipEdits::rippleDeleteTracks(p, *range, trackIds); break;
        case TimelineAction::unlink: return ClipEdits::unlink(p, explicitClips.empty() ? ids : explicitClips);
        case TimelineAction::link: return ClipEdits::link(p, ids);
        case TimelineAction::earlier: case TimelineAction::later:
        {
            const auto target = neighbour(p, ids, a == TimelineAction::later);
            if (target.empty()) return failure(p, k("인접한 클립/묶음이 없습니다."));
            return ClipEdits::reorder(p, ids, a == TimelineAction::later ? ClipEdits::Placement::after : ClipEdits::Placement::before, target);
        }
        default: break;
    }
    return failure(p, k("편집할 클립이나 구간을 선택하세요."));
}
ClipEditResult TimelineEditController::preview(TimelineAction a, Sample at, bool exact) const
{ return isLocked() ? failure(document.getProject(), blocked().getErrorMessage()) : apply(document.getProject(), a, targets(), at, exact); }
juce::Result TimelineEditController::execute(TimelineAction a, Sample at, bool exact, const juce::String& key)
{
    if (a == TimelineAction::addMarker) return addMarker();
    if (isLocked()) return blocked();
    cancelDrag(); reconcileSelection();
    juce::Result result = juce::Result::ok();
    if (a == TimelineAction::undo) result = document.undo();
    else if (a == TimelineAction::redo) result = document.redo();
    else
    {
        const auto ids = targets();
        result = document.performEdit(editName(a), key, [&](const RecorderProject& p) { return apply(p, a, ids, at, exact); });
    }
    if (result.wasOk())
    {
        reconcileSelection();
        if (a == TimelineAction::remove || a == TimelineAction::rippleAll || a == TimelineAction::rippleAudio) range.reset();
    }
    return result;
}
RipplePrompt TimelineEditController::ripplePrompt() const
{
    RipplePrompt prompt; prompt.base = document.snapshot(); prompt.range = range.value_or(SampleRange{}); prompt.tracks = trackIds; prompt.expandedTracks = trackIds;
    const auto& p = *prompt.base;
    for (const auto& t : p.tracks) if (contains(trackIds, t.trackId)) for (const auto& c : t.clips.items())
        if (p.isActive(c) && c.timelineEnd() > prompt.range.start) prompt.detachClips.push_back(c.clipId);
    for (;;)
    {
        const auto count = prompt.expandedTracks.size(); std::vector<Id> affected;
        for (const auto& t : p.tracks) if (contains(prompt.expandedTracks, t.trackId)) for (const auto& c : t.clips.items())
            if (p.isActive(c) && c.timelineEnd() > prompt.range.start) affected.push_back(c.clipId);
        for (const auto& id : expandLinks(p, affected))
        { const auto lane = p.findClip(id)->trackId; if (!contains(prompt.expandedTracks, lane)) prompt.expandedTracks.push_back(lane); }
        if (count == prompt.expandedTracks.size()) break;
    }
    prompt.conflict = prompt.expandedTracks.size() != trackIds.size();
    for (const auto& t : p.tracks) if (contains(prompt.expandedTracks, t.trackId) && !audio(t)) prompt.requiresAllTracks = true;
    return prompt;
}
juce::Result TimelineEditController::resolveRipple(const RipplePrompt& prompt, RippleChoice choice)
{
    if (choice == RippleChoice::cancel) return juce::Result::ok();
    if (isLocked()) return blocked();
    if (document.snapshot() != prompt.base || !range || range->start != prompt.range.start || range->length != prompt.range.length || trackIds != prompt.tracks)
        return juce::Result::fail(k("대상이 바뀌었습니다. 구간과 트랙을 다시 확인하세요."));
    const auto result = document.performEdit(text(TimelineAction::rippleAudio), {}, [&](const RecorderProject& p)
    {
        if (choice == RippleChoice::expand) return prompt.requiresAllTracks ? ClipEdits::rippleDeleteAll(p, prompt.range) : ClipEdits::rippleDeleteTracks(p, prompt.range, prompt.expandedTracks);
        auto detached = ClipEdits::unlink(p, prompt.detachClips);
        return detached.status.failed() ? detached : ClipEdits::rippleDeleteTracks(detached.project, prompt.range, prompt.tracks);
    });
    if (result.wasOk()) { range.reset(); reconcileSelection(); }
    return result;
}
juce::Result TimelineEditController::setTrackListening(const Id& id, bool solo)
{
    if (isLocked()) return blocked();
    const auto& ts = document.getProject().tracks;
    if (std::none_of(ts.begin(), ts.end(), [&](const Track& t) { return t.trackId == id && audio(t); })) return juce::Result::fail(k("오디오 트랙을 선택하세요."));
    return document.performEdit(text(solo ? TimelineAction::solo : TimelineAction::mute), [=](EditState& e)
    { for (auto& t : e.tracks) if (t.trackId == id) { if (solo) t.solo = !t.solo; else t.mute = !t.mute; } });
}
juce::Result TimelineEditController::setTrackHidden(const Id& id, bool hidden)
{
    if (isLocked()) return blocked();
    const auto& ts = document.getProject().tracks;
    if (std::none_of(ts.begin(), ts.end(), [&](const Track& t) { return t.trackId == id; }))
        return juce::Result::fail(juce::String::fromUTF8("트랙을 선택하세요."));
    return document.performEdit(text(hidden ? TimelineAction::hideTrack : TimelineAction::showTrack), [=](EditState& e)
    { for (auto& t : e.tracks) if (t.trackId == id) t.hidden = hidden; });
}
juce::Result TimelineEditController::setTracksHidden(const std::vector<Id>& ids, bool hidden)
{
    if (isLocked()) return blocked();
    if (ids.empty()) return juce::Result::ok();
    const auto name = text(hidden ? TimelineAction::hideTrack : TimelineAction::showTrack) + (ids.size() > 1 ? juce::String(" (") + juce::String(int(ids.size())) + ")" : juce::String());
    return document.performEdit(name, [ids, hidden](EditState& e)
    { for (auto& t : e.tracks) if (std::find(ids.begin(), ids.end(), t.trackId) != ids.end()) t.hidden = hidden; });
}
juce::Result TimelineEditController::addMarker(const juce::String& name, const juce::String& colour)
{
    Marker marker; marker.sample = cursor; marker.name = name.isEmpty() ? k("마커 ") + juce::String(document.getProject().markers.size() + 1) : name; marker.colour = colour;
    return document.addMarker(std::move(marker));
}
juce::Result TimelineEditController::editMarker(const Id& id, Sample at, const juce::String& name, const juce::String& colour)
{
    if (isLocked()) return blocked();
    if (at < 0 || name.trim().isEmpty() || colour.length() != 7 || colour[0] != '#'
        || !colour.substring(1).containsOnly("0123456789abcdefABCDEF")) return juce::Result::fail(k("마커 위치·이름·색을 확인하세요. 색은 #RRGGBB입니다."));
    return document.performEdit(text(TimelineAction::editMarker), [=](EditState& e)
    { for (auto& m : e.markers) if (m.markerId == id) { m.sample = at; m.name = name; m.colour = colour; return; } throw std::invalid_argument("Marker missing"); });
}
juce::Result TimelineEditController::deleteMarker(const Id& id)
{
    if (isLocked()) return blocked();
    return document.performEdit(text(TimelineAction::deleteMarker), [=](EditState& e)
    { e.markers.erase(std::remove_if(e.markers.begin(), e.markers.end(), [&](const Marker& m) { return m.markerId == id; }), e.markers.end()); });
}
bool TimelineEditController::beginDrag(TimelineAction a)
{
    cancelDrag(); reconcileSelection();
    if (isLocked() || targets().empty() || (a != TimelineAction::move && a != TimelineAction::trimIn && a != TimelineAction::trimOut)) return false;
    document.endGesture(); dragBase = document.snapshot(); dragIds = targets(); dragSelection = document.getSelection(); dragKind = a; return true;
}
const ClipEditResult* TimelineEditController::dragTo(Sample at, bool exact)
{
    if (!dragBase || isLocked()) return nullptr;
    dragAt = at; dragExact = exact;
    dragResult = std::make_unique<ClipEditResult>(apply(*dragBase, dragKind, dragIds, at, exact)); return dragResult.get();
}
std::optional<Sample> TimelineEditController::dragNeighbourGuide() const
{
    if (!dragResult || dragResult->status.failed()) return {};
    const auto& p = dragResult->project;
    for (const auto& id : dragIds) if (const auto* edited = p.findClip(id))
        for (const auto& track : p.tracks) if (track.trackId == edited->trackId)
            for (const auto& other : track.clips.items()) if (!contains(dragIds, other.clipId) && p.isActive(other))
            {
                if (dragKind != TimelineAction::trimOut && edited->timelineStartSample == other.timelineEnd()) return other.timelineEnd();
                if (dragKind != TimelineAction::trimIn && edited->timelineEnd() == other.timelineStartSample) return other.timelineStartSample;
            }
    return {};
}
juce::Result TimelineEditController::commitDrag()
{
    if (isLocked()) { cancelDrag(); return blocked(); }
    if (!dragBase || !dragResult) { cancelDrag(); return juce::Result::ok(); }
    if (dragBase != document.snapshot() || dragSelection != document.getSelection())
    { cancelDrag(); return juce::Result::fail(k("드래그 중 편집 대상이 바뀌었습니다. 다시 이동하세요.")); }
    if (dragResult->status.failed()) { const auto result = dragResult->status; cancelDrag(); return result; }
    const auto result = document.performEdit(editName(dragKind), {}, [&](const RecorderProject& p) { return apply(p, dragKind, dragIds, dragAt, dragExact); });
    cancelDrag(); document.endGesture(); reconcileSelection(); return result;
}
void TimelineEditController::cancelDrag() { dragResult.reset(); dragBase.reset(); dragIds.clear(); dragSelection.clear(); }
bool TimelineEditController::parseSample(const juce::String& input, Sample& value)
{
    const auto s = input.trim().toStdString(); Sample parsed = 0; const auto r = std::from_chars(s.data(), s.data() + s.size(), parsed);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || s.empty() || parsed < 0) return false; value = parsed; return true;
}
bool TimelineEditController::parseTimecode(const juce::String& input, unsigned Fs, Sample& value)
{
    if (Fs == 0) return false;
    const auto parts = juce::StringArray::fromTokens(input.trim(), ":", "");
    if (parts.size() != 2 && parts.size() != 3) return false;
    const auto maximum = (std::numeric_limits<Sample>::max)();
    Sample seconds = 0;
    for (int i = 0; i < parts.size() - 1; ++i)
    {
        Sample field = 0;
        if (parts[i].isEmpty() || !parts[i].containsOnly("0123456789") || !parseSample(parts[i], field)) return false;
        // Minutes (the field before the seconds) may be typed as 1 or 2 digits and must stay below 60; hours are free-form.
        if ((parts.size() == 2 || i == 1) && (parts[i].length() > 2 || field >= 60)) return false;
        if (seconds > (maximum - field) / 60) return false;
        seconds = seconds * 60 + field;
    }
    const auto last = parts[parts.size() - 1]; const auto dot = last.indexOfChar('.');
    const auto whole = dot < 0 ? last : last.substring(0, dot);
    Sample field = 0, millis = 0;
    if (whole.isEmpty() || whole.length() > 2 || !whole.containsOnly("0123456789") || !parseSample(whole, field) || field >= 60) return false;
    if (dot >= 0)
    {
        const auto fraction = last.substring(dot + 1);
        if (fraction.isEmpty() || fraction.length() > 3 || !fraction.containsOnly("0123456789")
            || !parseSample(fraction.paddedRight('0', 3), millis)) return false;
    }
    if (seconds > (maximum - field) / 60) return false;
    seconds = seconds * 60 + field;
    const auto fractionSamples = (millis * Fs + 500) / 1000; // nearest sample, without floating point or overflow
    if (seconds > (maximum - fractionSamples) / Fs) return false;
    value = seconds * Fs + fractionSamples; return true;
}
}
