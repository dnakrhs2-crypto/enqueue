#include "TakeStackEdits.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace gocue::recorder
{
namespace
{
void need(bool b, const char* text) { if (!b) throw std::invalid_argument(text); }
void checked(const RecorderProject& p)
{ const auto r = p.validate(); if (r.failed()) throw std::invalid_argument(r.getErrorMessage().toStdString()); }
Sample endOf(SampleRange r)
{
    need(r.start >= 0 && r.length > 0 && r.start <= (std::numeric_limits<Sample>::max)() - r.length, "테이크 구간이 잘못되었습니다.");
    return r.start + r.length;
}
template<class F> ClipEditResult apply(const RecorderProject& p, F f)
{
    try { checked(p); auto next = p; f(next); checked(next); return ClipEditResult(std::move(next)); }
    catch (const std::exception& e) { ClipEditResult r(p); r.status = juce::Result::fail(juce::String::fromUTF8(e.what())); return r; }
}
void rebuild(RecorderProject& p)
{
    for (auto& g : p.linkGroups) g.clipIds.clear();
    for (auto& s : p.takeStacks) for (auto& v : s.versions) v.clipIds.clear();
    for (const auto& t : p.tracks) for (const auto& c : t.clips.items())
    {
        if (c.linkGroupId.isNotEmpty())
        {
            auto g = std::find_if(p.linkGroups.begin(), p.linkGroups.end(), [&](const auto& x) { return x.linkGroupId == c.linkGroupId; });
            if (g == p.linkGroups.end()) { p.linkGroups.push_back({c.linkGroupId, {}}); g = std::prev(p.linkGroups.end()); }
            g->clipIds.push_back(c.clipId);
        }
        if (c.takeStackId.isNotEmpty())
            for (auto& s : p.takeStacks) if (s.stackId == c.takeStackId)
                for (auto& v : s.versions) if (v.versionId == c.versionId) v.clipIds.push_back(c.clipId);
    }
    std::set<Id> single;
    for (const auto& g : p.linkGroups) if (g.clipIds.size() < 2) single.insert(g.linkGroupId);
    for (auto& t : p.tracks) for (auto& c : t.clips.edit()) if (single.count(c.linkGroupId)) c.linkGroupId.clear();
    p.linkGroups.erase(std::remove_if(p.linkGroups.begin(), p.linkGroups.end(), [&](const auto& g) { return single.count(g.linkGroupId) != 0; }), p.linkGroups.end());
    p.takeStacks.erase(std::remove_if(p.takeStacks.begin(), p.takeStacks.end(), [](const auto& s)
    { return std::all_of(s.versions.begin(), s.versions.end(), [](const auto& v) { return v.clipIds.empty(); }); }), p.takeStacks.end());
}
std::vector<Id> videoSelection(const RecorderProject& p, const Id& id)
{
    const auto* s = TakeStackEdits::find(p, id); std::vector<Id> out;
    if (s) for (const auto& t : p.tracks) if (t.kind == TrackKind::cam1 || t.kind == TrackKind::cam2)
        for (const auto& c : t.clips.items()) if (c.takeStackId == id && c.versionId == s->activeVersionId) out.push_back(c.clipId);
    return out;
}
ClipEditResult trimStack(const RecorderProject& p, const Id& id, Sample t, bool in)
{
    return apply(p, [&](RecorderProject& next)
    {
        const auto* stack = TakeStackEdits::find(p,id); need(stack && t >= 0,"트림할 테이크 구간이 없습니다.");
        const auto oldStart = stack->anchorSample, oldEnd = endOf({oldStart,stack->spanSamples});
        if (t != (in ? oldStart : oldEnd)) t = frameToSample(sampleToFrame(t,p.Fs,p.fps),p.Fs,p.fps);
        const auto start = in ? t : oldStart, end = in ? oldEnd : t; endOf({start,end - start});
        for (auto& track : next.tracks)
        {
            std::vector<Clip> clips;
            for (const auto& old : track.clips.items())
            {
                if (old.takeStackId != id) { clips.push_back(old); continue; }
                // Independently placed microphones outside the recording range
                // remain members, but an edge trim is not a global time ripple.
                if (track.kind == TrackKind::mic && (old.timelineEnd() <= oldStart || old.timelineStartSample >= oldEnd)) { clips.push_back(old); continue; }
                auto first = (std::max)(old.timelineStartSample,start), last = (std::min)(old.timelineEnd(),end);
                if (start < oldStart && old.timelineStartSample == oldStart) first = start;
                if (end > oldEnd && old.timelineEnd() == oldEnd) last = end;
                if (last <= first) continue;
                auto c = old; c.sourceIn += first - old.timelineStartSample; c.timelineStartSample = first; c.lengthSamples = last - first;
                const auto* asset = p.media->findAsset(c.assetId);
                need(c.sourceIn >= 0 && c.lengthSamples <= asset->logicalLength - c.sourceIn,"테이크 버전의 원본 핸들을 벗어났습니다.");
                clips.push_back(c);
            }
            track.clips.edit() = std::move(clips);
        }
        for (auto& s : next.takeStacks) if (s.stackId == id) { s.anchorSample = start; s.spanSamples = end - start; }
        rebuild(next);
    });
}
// Cut all old versions at exact dubbing boundaries (Pstart is not frame-snapped).
// The currently visible overlap becomes one previous version. Other historical
// overlaps remain as whole-range alternatives, with the other lanes/segments
// copied from that same previous version. No source bytes or imports are touched.
void archiveOverlap(RecorderProject& p, TakeStack& target)
{
    const auto a = target.anchorSample, b = a + target.spanSamples;
    std::set<Id> stacks, targets, groups;
    for (const auto& t : p.tracks) if (t.kind == TrackKind::cam1 || t.kind == TrackKind::cam2)
        for (const auto& c : t.clips.items()) if (p.isActive(c) && c.timelineStartSample < b && c.timelineEnd() > a)
        { targets.insert(c.clipId); if (c.takeStackId.isNotEmpty()) stacks.insert(c.takeStackId); if (c.linkGroupId.isNotEmpty()) groups.insert(c.linkGroupId); }
    for (const auto& t : p.tracks) for (const auto& c : t.clips.items())
        if (stacks.count(c.takeStackId) || groups.count(c.linkGroupId))
        { need(t.kind != TrackKind::importAudio, "완성 오디오와 연결된 기존 영상을 먼저 링크 해제하세요."); targets.insert(c.clipId); }
    struct Piece { Clip clip; Id oldStack, oldVersion; bool active; };
    std::vector<Piece> inside;
    std::map<std::pair<Id, int>, Id> splitGroups;
    const auto groupFor = [&](const Id& old, int side)
    {
        if (old.isEmpty()) return Id{};
        auto [i, fresh] = splitGroups.emplace(std::make_pair(old, side), Id{}); if (fresh) i->second = newId(); return i->second;
    };
    for (auto& t : p.tracks)
    {
        std::vector<Clip> out;
        for (const auto& c : t.clips.items())
        {
            if (!targets.count(c.clipId) || c.timelineEnd() <= a || c.timelineStartSample >= b) { out.push_back(c); continue; }
            const auto part = [&](Sample first, Sample last, int side)
            {
                if (last <= first) return;
                auto x = c; x.clipId = newId(); x.sourceIn += first - c.timelineStartSample;
                x.timelineStartSample = first; x.lengthSamples = last - first; x.linkGroupId = groupFor(c.linkGroupId, side);
                if (side == 1) inside.push_back({x, c.takeStackId, c.versionId, p.isActive(c)}); else out.push_back(x);
            };
            part(c.timelineStartSample, (std::min)(a, c.timelineEnd()), 0);
            part((std::max)(a, c.timelineStartSample), (std::min)(b, c.timelineEnd()), 1);
            part((std::max)(b, c.timelineStartSample), c.timelineEnd(), 2);
        }
        t.clips.edit() = std::move(out);
    }
    if (inside.empty()) return;
    const auto appendVersion = [&](const Id& changedStack, const Id& changedVersion)
    {
        TakeVersion v; std::map<Id, Id> links;
        for (const auto& piece : inside)
        {
            const bool use = changedStack.isNotEmpty() && piece.oldStack == changedStack ? piece.oldVersion == changedVersion : piece.active;
            if (!use) continue;
            auto c = piece.clip; c.clipId = newId(); c.takeStackId = target.stackId; c.versionId = v.versionId;
            if (c.linkGroupId.isNotEmpty()) { auto [i, fresh] = links.emplace(c.linkGroupId, Id{}); if (fresh) i->second = newId(); c.linkGroupId = i->second; }
            for (auto& t : p.tracks) if (t.trackId == c.trackId) t.clips.edit().push_back(c);
            v.clipIds.push_back(c.clipId);
        }
        target.versions.push_back(std::move(v));
    };
    appendVersion({}, {});
    for (const auto& s : p.takeStacks) if (stacks.count(s.stackId))
        for (const auto& v : s.versions) if (v.versionId != s.activeVersionId) appendVersion(s.stackId, v.versionId);
}
}
const TakeStack* TakeStackEdits::find(const RecorderProject& p, const Id& id)
{ for (const auto& s : p.takeStacks) if (s.stackId == id) return &s; return nullptr; }
ClipEditResult TakeStackEdits::addTake(const RecorderProject& p, const Id& takeId, SampleRange r,
                                     const std::vector<int>& mics, const Id& retake)
{
    return apply(p, [&](RecorderProject& next)
    {
        endOf(r); const auto* take = p.media->findTake(takeId);
        need(take && take->mode == TakeMode::dub && take->placementSample == r.start && take->logicalLength <= r.length, "더빙 원본과 녹화 구간이 다릅니다.");
        need(mics.size() == take->microphoneAssetIds.size(), "마이크 트랙 정보가 빠졌습니다.");
        std::set<int> lanes; for (auto m : mics) need(m >= 0 && m < 8 && lanes.insert(m).second, "마이크 트랙이 중복되거나 잘못되었습니다.");
        TakeStack stack;
        if (retake.isNotEmpty())
        {
            const auto* old = find(p, retake); need(old && old->anchorSample == r.start && old->spanSamples == r.length, "리테이크 범위가 바뀌었습니다.");
            stack = *old;
            next.takeStacks.erase(std::remove_if(next.takeStacks.begin(), next.takeStacks.end(), [&](const auto& s) { return s.stackId == retake; }), next.takeStacks.end());
        }
        else { stack.anchorSample = r.start; stack.spanSamples = r.length; archiveOverlap(next, stack); }
        TakeVersion version; LinkGroup group;
        const auto add = [&](const Id& assetId, TrackKind kind, int mic)
        {
            if (assetId.isEmpty()) return;
            const auto* asset = p.media->findAsset(assetId); need(asset && asset->logicalLength == take->logicalLength, "테이크 원본 길이가 다릅니다.");
            auto lane = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto& t) { return t.kind == kind && t.microphoneIndex == mic; });
            if (lane == next.tracks.end()) { Track t; t.kind = kind; t.microphoneIndex = mic; t.name = kind == TrackKind::cam1 ? juce::String::fromUTF8("캠1") : kind == TrackKind::cam2 ? juce::String::fromUTF8("캠2") : juce::String::fromUTF8("마이크 ") + juce::String(mic + 1); next.tracks.push_back(t); lane = std::prev(next.tracks.end()); }
            Clip c; c.trackId = lane->trackId; c.assetId = assetId; c.timelineStartSample = r.start; c.lengthSamples = take->logicalLength;
            c.takeStackId = stack.stackId; c.versionId = version.versionId; c.linkGroupId = group.linkGroupId;
            lane->clips.edit().push_back(c); version.clipIds.push_back(c.clipId);
        };
        add(take->cam1AssetId, TrackKind::cam1, -1); add(take->cam2AssetId, TrackKind::cam2, -1);
        for (size_t i = 0; i < mics.size(); ++i) add(take->microphoneAssetIds[i], TrackKind::mic, mics[i]);
        need(!version.clipIds.empty(), "빈 테이크 버전은 추가할 수 없습니다.");
        stack.activeVersionId = version.versionId; stack.versions.insert(stack.versions.begin(), version);
        next.takeStacks.push_back(std::move(stack)); rebuild(next);
    });
}
TakeVersionImpact TakeStackEdits::impact(const RecorderProject& p, const Id& id, const Id& version)
{
    TakeVersionImpact out;
    try
    {
        checked(p); const auto* s = find(p, id); need(s != nullptr, "테이크 구간을 찾을 수 없습니다.");
        Sample first = s->anchorSample, last = endOf({s->anchorSample, s->spanSamples}); bool found = false;
        for (const auto& v : s->versions) if (v.versionId == version || v.versionId == s->activeVersionId)
        {
            if (v.versionId == version) { found = true; out.restored = v.clipIds; }
            if (v.versionId == s->activeVersionId) out.removed = v.clipIds;
            for (const auto& clipId : v.clipIds) { const auto& c = *p.findClip(clipId); first = (std::min)(first, c.timelineStartSample); last = (std::max)(last, c.timelineEnd()); }
        }
        need(found, "테이크 버전을 찾을 수 없습니다."); out.range = {first, last - first};
        auto next = p; for (auto& stack : next.takeStacks) if (stack.stackId == id) stack.activeVersionId = version;
        checked(next); // includes actual displaced microphones, external links and lane collisions
    }
    catch (const std::exception& e) { out.status = juce::Result::fail(juce::String::fromUTF8(e.what())); }
    return out;
}
ClipEditResult TakeStackEdits::useVersion(const RecorderProject& p, const Id& stack, const Id& version)
{
    const auto review = impact(p, stack, version); ClipEditResult out(p); out.status = review.status;
    if (out.status.wasOk()) { for (auto& s : out.project.takeStacks) if (s.stackId == stack) s.activeVersionId = version; out.selection = review.restored; }
    return out;
}
ClipEditResult TakeStackEdits::split(const RecorderProject& p, const Id& s, Sample t) { return ClipEdits::split(p, videoSelection(p, s), t); }
ClipEditResult TakeStackEdits::trimIn(const RecorderProject& p, const Id& s, Sample t) { return trimStack(p,s,t,true); }
ClipEditResult TakeStackEdits::trimOut(const RecorderProject& p, const Id& s, Sample t) { return trimStack(p,s,t,false); }
ClipEditResult TakeStackEdits::rippleDeleteAll(const RecorderProject& p, SampleRange r) { return ClipEdits::rippleDeleteAll(p, r); }
}
