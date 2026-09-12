#include "ClipEdits.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace gocue::recorder
{
namespace
{
using Ids = std::set<Id>;
constexpr Sample maximum = (std::numeric_limits<Sample>::max)();
void need(bool yes, const char* message) { if (!yes) throw std::invalid_argument(message); }
void valid(const RecorderProject& p)
{ const auto r = p.validate(); if (r.failed()) throw std::invalid_argument(r.getErrorMessage().toStdString()); }
Sample add(Sample a, Sample b)
{
    need(b >= 0 ? a <= maximum - b : a >= (std::numeric_limits<Sample>::min)() - b, "편집 시간 범위를 초과했습니다.");
    return a + b;
}
bool video(const RecorderProject& p, const Clip& c) { return p.media->findAsset(c.assetId)->kind == AssetKind::camera; }
bool sameClip(const Clip& a, const Clip& b)
{
    return a.clipId == b.clipId && a.trackId == b.trackId && a.assetId == b.assetId
        && a.sourceIn == b.sourceIn && a.lengthSamples == b.lengthSamples && a.timelineStartSample == b.timelineStartSample
        && a.linkGroupId == b.linkGroupId && a.takeStackId == b.takeStackId && a.versionId == b.versionId;
}
Sample snap(const RecorderProject& p, Sample t) { return frameToSample(sampleToFrame(t, p.Fs, p.fps), p.Fs, p.fps); }
std::optional<Sample> neighbourDelta(const RecorderProject& p, const Ids& ids, const Clip& reference,
    Sample delta, bool move, bool in, Sample requestedDelta)
{
    // Largest integer distance strictly below one rational project frame.
    const auto tolerance = (std::uint64_t(p.Fs) * p.fps.denominator - 1) / p.fps.numerator;
    auto best = tolerance;
    std::optional<Sample> joined;
    const auto consider = [&](Sample originalEdge, Sample target, bool front)
    {
        const auto edge = add(originalEdge, delta), requestedEdge = add(originalEdge, requestedDelta);
        if (edge < 0) return;
        // Grid rounding must not turn an overlap of a full frame into an accepted join.
        if (front ? requestedEdge < target && std::uint64_t(target) - std::uint64_t(requestedEdge) > tolerance
                  : requestedEdge > target && std::uint64_t(requestedEdge) - std::uint64_t(target) > tolerance) return;
        const auto distance = edge >= target ? std::uint64_t(edge) - std::uint64_t(target) : std::uint64_t(target) - std::uint64_t(edge);
        if (distance <= best && (!joined || distance < best))
        { best = distance; joined = add(delta, target - edge); }
    };
    for (const auto& track : p.tracks) if (track.trackId == reference.trackId)
        for (const auto& c : track.clips.items()) if (!ids.count(c.clipId) && p.isActive(c))
        {
            if (move || in) consider(reference.timelineStartSample, c.timelineEnd(), true);
            if (move || !in) consider(reference.timelineEnd(), c.timelineStartSample, false);
        }
    return joined;
}
Ids explicitIds(const RecorderProject& p, const std::vector<Id>& ids)
{
    need(!ids.empty(), "편집할 클립을 선택하세요.");
    Ids out;
    for (const auto& id : ids) { need(p.findClip(id) != nullptr, "선택한 클립을 찾을 수 없습니다."); out.insert(id); }
    return out;
}
Ids linked(const RecorderProject& p, Ids ids)
{
    for (const auto& g : p.linkGroups)
        if (std::any_of(g.clipIds.begin(), g.clipIds.end(), [&](const Id& id) { return ids.count(id) != 0; }))
            ids.insert(g.clipIds.begin(), g.clipIds.end());
    return ids;
}
Ids stackIds(const RecorderProject& p, const Ids& ids)
{
    Ids stacks;
    for (const auto& id : ids) { const auto& c = *p.findClip(id); if (video(p, c) && c.takeStackId.isNotEmpty()) stacks.insert(c.takeStackId); }
    return stacks;
}
Ids expand(const RecorderProject& p, const std::vector<Id>& ids, bool versions = true)
{
    auto out = linked(p, explicitIds(p, ids));
    if (!versions) return out;
    for (;;)
    {
        const auto previous = out.size(); const auto stacks = stackIds(p, out);
        for (const auto& t : p.tracks) for (const auto& c : t.clips.items())
            if (stacks.count(c.takeStackId)) out.insert(c.clipId);
        out = linked(p, std::move(out));
        if (out.size() == previous) return out;
    }
}
bool hasVideo(const RecorderProject& p, const Ids& ids)
{ for (const auto& id : ids) if (video(p, *p.findClip(id))) return true; return false; }
SampleRange range(const RecorderProject& p, SampleRange r, bool frameSnap)
{
    need(r.start >= 0 && r.length > 0, "삭제 구간이 비었거나 음수입니다.");
    auto end = add(r.start, r.length);
    if (frameSnap) { r.start = snap(p, r.start); end = snap(p, end); }
    need(end > r.start, "프레임 격자에서 삭제 구간이 비었습니다.");
    return {r.start, end - r.start};
}
Sample pull(Sample t, SampleRange r)
{ return t <= r.start ? t : t < r.start + r.length ? r.start : t - r.length; }

struct Edit
{
    RecorderProject p;
    Ids used;
    Ids populatedStacks;
    Sample serial = 0;
    std::vector<Id> selection;
    explicit Edit(const RecorderProject& source) : p(source)
    {
        used.insert(p.projectId);
        for (const auto& a : p.media->assets) used.insert(a.assetId);
        for (const auto& t : p.media->takes) used.insert(t.takeId);
        for (const auto& t : p.tracks) { used.insert(t.trackId); for (const auto& c : t.clips.items()) used.insert(c.clipId); }
        for (const auto& m : p.markers) used.insert(m.markerId);
        for (const auto& g : p.linkGroups) used.insert(g.linkGroupId);
        for (const auto& s : p.takeStacks)
        {
            used.insert(s.stackId);
            for (const auto& v : s.versions) { used.insert(v.versionId); if (!v.clipIds.empty()) populatedStacks.insert(s.stackId); }
        }
    }
    Id id()
    {
        for (;;) { serial = add(serial, 1); auto candidate = p.projectId.substring(0, 16) + juce::String::toHexString(static_cast<juce::int64>(serial)).paddedLeft('0', 16); if (used.insert(candidate).second) return candidate; }
    }
    template<class F> void rewrite(const Ids& targets, F transform)
    {
        std::map<Id, std::vector<Id>> replacements;
        for (auto& t : p.tracks)
        {
            if (std::none_of(t.clips.items().begin(), t.clips.items().end(), [&](const Clip& c) { return targets.count(c.clipId) != 0; })) continue;
            std::vector<Clip> clips;
            for (const auto& c : t.clips.items())
            {
                if (!targets.count(c.clipId)) { clips.push_back(c); continue; }
                auto parts = transform(c); std::vector<Id> ids;
                for (const auto& part : parts) { ids.push_back(part.clipId); selection.push_back(part.clipId); clips.push_back(part); }
                replacements[c.clipId] = std::move(ids);
            }
            if (clips.size() != t.clips.items().size() || !std::equal(clips.begin(), clips.end(), t.clips.items().begin(), sameClip))
                t.clips.edit() = std::move(clips);
        }
        const auto remap = [&](std::vector<Id>& ids)
        {
            std::vector<Id> out;
            for (const auto& old : ids) { const auto it = replacements.find(old); if (it == replacements.end()) out.push_back(old); else out.insert(out.end(), it->second.begin(), it->second.end()); }
            ids = std::move(out);
        };
        for (auto& s : p.takeStacks) for (auto& v : s.versions) remap(v.clipIds);
        // Existing groups retain member order. New fragment groups are filled below.
        for (auto& g : p.linkGroups)
        {
            remap(g.clipIds);
            g.clipIds.erase(std::remove_if(g.clipIds.begin(), g.clipIds.end(), [&](const Id& value) { const auto* c = p.findClip(value); return !c || c->linkGroupId != g.linkGroupId; }), g.clipIds.end());
        }
    }
    void tidy()
    {
        for (const auto& t : p.tracks) for (const auto& c : t.clips.items()) if (c.linkGroupId.isNotEmpty())
        {
            auto it = std::find_if(p.linkGroups.begin(), p.linkGroups.end(), [&](const LinkGroup& g) { return g.linkGroupId == c.linkGroupId; });
            if (it == p.linkGroups.end()) { p.linkGroups.push_back({c.linkGroupId, {}}); it = std::prev(p.linkGroups.end()); }
            if (std::find(it->clipIds.begin(), it->clipIds.end(), c.clipId) == it->clipIds.end()) it->clipIds.push_back(c.clipId);
        }
        Ids singletons;
        for (const auto& g : p.linkGroups) if (g.clipIds.size() < 2) singletons.insert(g.linkGroupId);
        for (auto& t : p.tracks)
            if (std::any_of(t.clips.items().begin(), t.clips.items().end(), [&](const Clip& c) { return singletons.count(c.linkGroupId) != 0; }))
                for (auto& c : t.clips.edit()) if (singletons.count(c.linkGroupId)) c.linkGroupId.clear();
        p.linkGroups.erase(std::remove_if(p.linkGroups.begin(), p.linkGroups.end(), [&](const LinkGroup& g) { return singletons.count(g.linkGroupId) != 0; }), p.linkGroups.end());
        p.takeStacks.erase(std::remove_if(p.takeStacks.begin(), p.takeStacks.end(), [&](const TakeStack& s)
        {
            return (populatedStacks.count(s.stackId) || s.spanSamples == 0)
                && std::all_of(s.versions.begin(), s.versions.end(), [](const TakeVersion& v) { return v.clipIds.empty(); });
        }), p.takeStacks.end());
        selection.erase(std::remove_if(selection.begin(), selection.end(), [&](const Id& value) { const auto* c = p.findClip(value); return !c || !p.isActive(*c); }), selection.end());
    }
};
template<class F> ClipEditResult apply(const RecorderProject& p, F f)
{
    try
    {
        valid(p); Edit edit(p); f(edit); edit.tidy(); valid(edit.p);
        ClipEditResult out(std::move(edit.p)); out.selection = std::move(edit.selection); return out;
    }
    catch (const std::exception& e) { ClipEditResult out(p); out.status = juce::Result::fail(juce::String::fromUTF8(e.what())); return out; }
}
struct FragmentGroups
{
    std::map<Id, std::pair<Id, Id>> groups;
    Id side(Edit& e, const Id& old, bool right)
    {
        if (old.isEmpty()) return {};
        auto it = groups.find(old);
        if (it == groups.end()) it = groups.emplace(old, std::make_pair(e.id(), e.id())).first;
        return right ? it->second.second : it->second.first;
    }
};
void cut(Edit& e, const Ids& ids, SampleRange r, bool ripple, bool retainLinks = false)
{
    FragmentGroups groups;
    e.rewrite(ids, [&](const Clip& c)
    {
        std::vector<Clip> out; const auto end = c.timelineEnd(), b = r.start + r.length;
        if (end <= r.start || c.timelineStartSample >= b)
        { auto part = c; if (ripple && part.timelineStartSample >= b) part.timelineStartSample -= r.length; out.push_back(part); return out; }
        const auto piece = [&](Sample start, Sample finish, bool right)
        {
            if (start >= finish) return;
            auto part = c; part.clipId = e.id(); part.sourceIn += start - c.timelineStartSample;
            part.timelineStartSample = ripple ? pull(start, r) : start; part.lengthSamples = finish - start;
            part.linkGroupId = retainLinks ? c.linkGroupId : groups.side(e, c.linkGroupId, right); out.push_back(part);
        };
        piece(c.timelineStartSample, (std::min)(end, r.start), false);
        piece((std::max)(c.timelineStartSample, b), end, true);
        return out;
    });
}
void checkExternalLinks(const RecorderProject& p, const Ids& affected, const Ids& tracks)
{
    for (const auto& id : linked(p, affected))
        need(tracks.count(p.findClip(id)->trackId) != 0, "대상 밖 트랙과 연결되어 있습니다. 링크를 해제하거나 대상 트랙을 포함하세요.");
}
void rippleStacks(Edit& e, SampleRange r)
{
    for (auto& s : e.p.takeStacks)
    {
        const auto end = pull(s.anchorSample + s.spanSamples, r); s.anchorSample = pull(s.anchorSample, r); s.spanSamples = end - s.anchorSample;
        if (s.spanSamples != 0) continue;
        // An independently moved microphone can survive deletion of the whole recording
        // span. Retain its version membership using the hull of the surviving placements.
        Sample first = maximum, last = 0;
        for (const auto& v : s.versions) for (const auto& id : v.clipIds) { const auto& c = *e.p.findClip(id); first = (std::min)(first, c.timelineStartSample); last = (std::max)(last, c.timelineEnd()); }
        if (last > first) { s.anchorSample = first; s.spanSamples = last - first; }
    }
}
ClipEditResult trim(const RecorderProject& p, const std::vector<Id>& requested, Sample t, bool in, bool frameSnap, bool joinNeighbours)
{
    return apply(p, [&](Edit& e)
    {
        auto ids = expand(p, requested, false); const auto& reference = *p.findClip(requested.front());
        const auto edge = in ? reference.timelineStartSample : reference.timelineEnd();
        need(t >= 0, "트림 위치는 음수가 될 수 없습니다.");
        const auto requestedDelta = t - edge;
        auto joined = joinNeighbours && t != edge ? neighbourDelta(p, ids, reference, requestedDelta, false, in, requestedDelta) : std::nullopt;
        if (joined) t = add(edge, *joined);
        else if (frameSnap && hasVideo(p, ids) && t != edge)
        {
            t = snap(p, t);
            if (joinNeighbours) joined = neighbourDelta(p, ids, reference, t - edge, false, in, requestedDelta);
            if (joined) t = add(edge, *joined);
        }
        const auto delta = t - edge;
        const auto stacks = stackIds(p, ids);
        // Match the edited edge across versions. For an outer stack edge each version's
        // outermost piece participates, including a shorter previous take.
        const auto seeds = ids;
        for (const auto& seed : seeds)
        {
            const auto& original = *p.findClip(seed);
            if (!stacks.count(original.takeStackId)) continue;
            const auto oldEdge = in ? original.timelineStartSample : original.timelineEnd();
            const auto& stack = *std::find_if(p.takeStacks.begin(), p.takeStacks.end(), [&](const TakeStack& s) { return s.stackId == original.takeStackId; });
            const bool outer = oldEdge == (in ? stack.anchorSample : stack.anchorSample + stack.spanSamples);
            for (const auto& track : p.tracks) for (const auto& c : track.clips.items())
            {
                if (c.takeStackId != original.takeStackId) continue;
                const auto candidateEdge = in ? c.timelineStartSample : c.timelineEnd();
                bool outermost = outer;
                for (const auto& other : track.clips.items()) if (other.takeStackId == c.takeStackId && other.versionId == c.versionId)
                    if (in ? other.timelineStartSample < candidateEdge : other.timelineEnd() > candidateEdge) outermost = false;
                if (candidateEdge == oldEdge || outermost) ids.insert(c.clipId);
            }
        }
        ids = linked(p, std::move(ids));
        e.rewrite(ids, [&](const Clip& c)
        {
            auto out = c;
            if (in) { out.sourceIn = add(c.sourceIn, delta); out.timelineStartSample = add(c.timelineStartSample, delta); out.lengthSamples = add(c.lengthSamples, -delta); }
            else out.lengthSamples = add(c.lengthSamples, delta);
            need(out.sourceIn >= 0 && out.timelineStartSample >= 0 && out.lengthSamples > 0
                && out.sourceIn <= p.media->findAsset(out.assetId)->logicalLength
                && out.lengthSamples <= p.media->findAsset(out.assetId)->logicalLength - out.sourceIn, "링크 구성원의 공통 원본 핸들을 벗어났습니다.");
            return std::vector<Clip>{out};
        });
        for (auto& s : e.p.takeStacks) if (stacks.count(s.stackId))
        {
            if (in && edge == s.anchorSample) { s.anchorSample = add(s.anchorSample, delta); s.spanSamples = add(s.spanSamples, -delta); }
            else if (!in && edge == s.anchorSample + s.spanSamples) s.spanSamples = add(s.spanSamples, delta);
        }
    });
}
}
ClipEditResult ClipEdits::split(const RecorderProject& p, const std::vector<Id>& requested, Sample t)
{
    return apply(p, [&](Edit& e)
    {
        const auto ids = expand(p, requested); need(t >= 0, "분할 위치는 음수가 될 수 없습니다."); if (hasVideo(p, ids)) t = snap(p, t);
        FragmentGroups groups;
        e.rewrite(ids, [&](const Clip& c)
        {
            if (t <= c.timelineStartSample || t >= c.timelineEnd()) return std::vector<Clip>{c};
            auto left = c, right = c; left.clipId = e.id(); right.clipId = e.id();
            left.lengthSamples = t - c.timelineStartSample; right.sourceIn += left.lengthSamples;
            right.timelineStartSample = t; right.lengthSamples -= left.lengthSamples;
            left.linkGroupId = groups.side(e, c.linkGroupId, false); right.linkGroupId = groups.side(e, c.linkGroupId, true);
            return std::vector<Clip>{left, right};
        });
    });
}
ClipEditResult ClipEdits::trimIn(const RecorderProject& p, const std::vector<Id>& ids, Sample t, bool frameSnap, bool joinNeighbours) { return trim(p, ids, t, true, frameSnap, joinNeighbours); }
ClipEditResult ClipEdits::trimOut(const RecorderProject& p, const std::vector<Id>& ids, Sample t, bool frameSnap, bool joinNeighbours) { return trim(p, ids, t, false, frameSnap, joinNeighbours); }
ClipEditResult ClipEdits::remove(const RecorderProject& p, const std::vector<Id>& requested)
{ return apply(p, [&](Edit& e) { e.rewrite(expand(p, requested), [](const Clip&) { return std::vector<Clip>{}; }); }); }
ClipEditResult ClipEdits::remove(const RecorderProject& p, const std::vector<Id>& requested, SampleRange r)
{ return apply(p, [&](Edit& e) { const auto ids = expand(p, requested); cut(e, ids, range(p, r, hasVideo(p, ids)), false); }); }
ClipEditResult ClipEdits::carveOut(const RecorderProject& p, const std::vector<Id>& tracks, SampleRange r)
{
    return apply(p, [&](Edit& e)
    {
        r = range(p, r, false);
        Ids ids;
        for (const auto& id : tracks)
        {
            const auto lane = std::find_if(p.tracks.begin(), p.tracks.end(), [&](const Track& t) { return t.trackId == id; });
            need(lane != p.tracks.end(), "덮어쓸 트랙을 찾을 수 없습니다.");
            for (const auto& c : lane->clips.items())
                if (p.isActive(c) && c.timelineStartSample < r.start + r.length && c.timelineEnd() > r.start) ids.insert(c.clipId);
        }
        cut(e, ids, r, false, true);
    });
}
ClipEditResult ClipEdits::remove(const RecorderProject& p, SampleRange r)
{
    return apply(p, [&](Edit& e)
    {
        std::vector<Id> requested; for (const auto& t : p.tracks) for (const auto& c : t.clips.items()) if (p.isActive(c)) requested.push_back(c.clipId);
        const auto ids = requested.empty() ? Ids{} : expand(p, requested); cut(e, ids, range(p, r, hasVideo(p, ids)), false);
    });
}
ClipEditResult ClipEdits::rippleDeleteAll(const RecorderProject& p, SampleRange r)
{
    return apply(p, [&](Edit& e)
    {
        Ids ids; for (const auto& t : p.tracks) for (const auto& c : t.clips.items()) ids.insert(c.clipId);
        r = range(p, r, hasVideo(p, ids)); cut(e, ids, r, true); rippleStacks(e, r);
        e.p.markers.erase(std::remove_if(e.p.markers.begin(), e.p.markers.end(), [&](const Marker& m) { return m.sample >= r.start && m.sample < r.start + r.length; }), e.p.markers.end());
        for (auto& m : e.p.markers) m.sample = pull(m.sample, r);
    });
}
ClipEditResult ClipEdits::rippleDeleteTracks(const RecorderProject& p, SampleRange r, const std::vector<Id>& tracks)
{
    return apply(p, [&](Edit& e)
    {
        need(!tracks.empty(), "당길 오디오 트랙을 선택하세요."); Ids lanes(tracks.begin(), tracks.end()), ids;
        for (const auto& id : lanes)
        {
            const auto it = std::find_if(p.tracks.begin(), p.tracks.end(), [&](const Track& t) { return t.trackId == id; });
            need(it != p.tracks.end() && (it->kind == TrackKind::mic || it->kind == TrackKind::importAudio), "선택 트랙 당기기는 오디오 트랙에만 적용합니다.");
            for (const auto& c : it->clips.items()) if (p.isActive(c) && c.timelineEnd() > r.start) ids.insert(c.clipId);
        }
        r = range(p, r, false); checkExternalLinks(p, ids, lanes); cut(e, ids, r, true);
    });
}
ClipEditResult ClipEdits::move(const RecorderProject& p, const std::vector<Id>& requested, Sample delta, bool frameSnap, bool joinNeighbours)
{
    return apply(p, [&](Edit& e)
    {
        const auto ids = expand(p, requested); const auto stacks = stackIds(p, ids);
        // Snap one video destination at the project origin, then use exactly that delta
        // for every member. Off-grid dubbing offsets between members are retained.
        const Clip* anchor = nullptr;
        for (const auto& id : requested) { const auto* c = p.findClip(id); if (!anchor && video(p, *c)) anchor = c; }
        for (const auto& t : p.tracks) for (const auto& c : t.clips.items())
            if (!anchor && ids.count(c.clipId) && video(p, c) && p.isActive(c) == p.isActive(*p.findClip(requested.front()))) anchor = &c;
        const auto requestedDelta = delta;
        const auto& reference = anchor ? *anchor : *p.findClip(requested.front());
        auto joined = joinNeighbours && delta != 0 ? neighbourDelta(p, ids, reference, delta, true, false, requestedDelta) : std::nullopt;
        if (joined) delta = *joined;
        else if (frameSnap && anchor && delta != 0)
        {
            delta = add(snap(p, add(anchor->timelineStartSample, delta)), -anchor->timelineStartSample);
            if (joinNeighbours) joined = neighbourDelta(p, ids, reference, delta, true, false, requestedDelta);
            if (joined) delta = *joined;
        }
        e.rewrite(ids, [&](const Clip& c) { auto out = c; out.timelineStartSample = add(c.timelineStartSample, delta); return std::vector<Clip>{out}; });
        for (auto& s : e.p.takeStacks) if (stacks.count(s.stackId)) s.anchorSample = add(s.anchorSample, delta);
    });
}
ClipEditResult ClipEdits::link(const RecorderProject& p, const std::vector<Id>& requested)
{
    return apply(p, [&](Edit& e)
    {
        const auto ids = expand(p, requested, false); need(ids.size() >= 2, "링크할 클립을 두 개 이상 선택하세요.");
        const auto existing = p.findClip(*ids.begin())->linkGroupId;
        const bool same = existing.isNotEmpty() && std::all_of(ids.begin(), ids.end(), [&](const Id& id) { return p.findClip(id)->linkGroupId == existing; });
        const auto group = same ? existing : e.id();
        e.rewrite(ids, [&](const Clip& c) { auto out = c; out.linkGroupId = group; return std::vector<Clip>{out}; });
    });
}
ClipEditResult ClipEdits::unlink(const RecorderProject& p, const std::vector<Id>& requested)
{ return apply(p, [&](Edit& e) { e.rewrite(explicitIds(p, requested), [](const Clip& c) { auto out = c; out.linkGroupId.clear(); return std::vector<Clip>{out}; }); }); }
ClipEditResult ClipEdits::reorder(const RecorderProject& p, const std::vector<Id>& requested, Placement placement, const std::vector<Id>& target)
{
    return apply(p, [&](Edit& e)
    {
        need(placement == Placement::before || placement == Placement::after, "삽입 방향이 잘못되었습니다.");
        const auto ids = expand(p, requested), targets = expand(p, target);
        for (const auto& id : ids) need(!targets.count(id), "같은 묶음 안에는 삽입할 수 없습니다.");
        const auto bounds = [&](const Ids& clips)
        {
            std::map<Id, SampleRange> lanes; Sample first = maximum, last = 0;
            for (const auto& id : clips)
            {
                const auto& c = *p.findClip(id); first = (std::min)(first, c.timelineStartSample); last = (std::max)(last, c.timelineEnd());
                auto it = lanes.find(c.trackId);
                if (it == lanes.end()) lanes.emplace(c.trackId, SampleRange{c.timelineStartSample, c.lengthSamples});
                else { const auto end = (std::max)(it->second.start + it->second.length, c.timelineEnd()); it->second.start = (std::min)(it->second.start, c.timelineStartSample); it->second.length = end - it->second.start; }
            }
            for (const auto& lane : lanes) need(lane.second.start == first && lane.second.length == last - first, "비대칭 묶음은 링크 해제 또는 대상 정리가 필요합니다.");
            return std::make_pair(SampleRange{first, last - first}, lanes);
        };
        const auto from = bounds(ids), to = bounds(targets); Ids lanes;
        for (const auto& entry : from.second) lanes.insert(entry.first);
        need(from.second.size() == to.second.size(), "같은 트랙 목록의 묶음만 순서를 바꿀 수 있습니다.");
        for (const auto& entry : to.second) need(lanes.count(entry.first) != 0, "대상 묶음의 트랙이 다릅니다.");
        const auto start = from.first.start, end = start + from.first.length;
        need(end <= to.first.start || to.first.start + to.first.length <= start, "겹친 묶음의 순서를 바꿀 수 없습니다.");
        auto destination = placement == Placement::before ? to.first.start : to.first.start + to.first.length;
        destination = pull(destination, from.first);
        const auto transform = [&](const Clip& c)
        {
            if (ids.count(c.clipId)) return add(c.timelineStartSample, destination - start);
            need(c.timelineEnd() <= start || c.timelineStartSample >= end, "제거할 묶음 구간에 다른 클립이 있습니다.");
            const auto a = pull(c.timelineStartSample, from.first), b = pull(c.timelineEnd(), from.first);
            need(!(a < destination && b > destination), "삽입 경계가 다른 클립 내부입니다.");
            return a >= destination ? add(a, from.first.length) : a;
        };
        Ids affected;
        for (const auto& t : p.tracks) if (lanes.count(t.trackId)) for (const auto& c : t.clips.items()) if (transform(c) != c.timelineStartSample) affected.insert(c.clipId);
        checkExternalLinks(p, affected, lanes);
        // Any moved link member must have the same delta, including links within a lane.
        for (const auto& id : linked(p, affected))
        {
            const auto& c = *p.findClip(id);
            for (const auto& otherId : linked(p, Ids{id})) { const auto& other = *p.findClip(otherId); need(transform(c) - c.timelineStartSample == transform(other) - other.timelineStartSample, "비대칭 링크의 상대 위치가 바뀝니다. 링크를 해제하세요."); }
        }
        for (auto& s : e.p.takeStacks)
        {
            std::optional<Sample> delta;
            for (const auto& v : s.versions) for (const auto& id : v.clipIds)
            { const auto& c = *p.findClip(id); const auto d = affected.count(id) ? transform(c) - c.timelineStartSample : 0; if (!delta) delta = d; else need(*delta == d, "테이크 스택 전체를 같은 묶음으로 이동하세요."); }
            if (delta) s.anchorSample = add(s.anchorSample, *delta);
        }
        e.rewrite(affected, [&](const Clip& c) { auto out = c; out.timelineStartSample = transform(c); return std::vector<Clip>{out}; });
        e.selection.assign(ids.begin(), ids.end());
    });
}
}
