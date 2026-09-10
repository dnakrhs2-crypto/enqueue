#pragma once
#include "TimelineView.logic.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace gocue::recorder
{
// Pixel-based interaction policy shared by the view and device-free regressions.
struct TimelineInteraction
{
    static constexpr int dragThreshold = 4, snapPixels = 8, edgePixels = 32;
    static double zoomStart(double start, double seconds, double nextSeconds, double fraction)
    { return std::max(0.0, start + fraction * (seconds - nextSeconds)); }
    static double revealStart(double start, double seconds, double at)
    { return at >= start && at <= start + seconds ? start : std::max(0.0, at - seconds * (at < start ? .05 : .95)); }
    static double edgeScroll(double x, double left, double right, double seconds, double elapsed)
    {
        if (right <= left) return 0;
        const auto speed = x < left + edgePixels ? -std::clamp((left + edgePixels - x) / edgePixels, 0.0, 1.0)
                         : x > right - edgePixels ? std::clamp((x - right + edgePixels) / edgePixels, 0.0, 1.0) : 0.0;
        return speed * seconds * .6 * elapsed;
    }
};
class TimelineSnapIndex
{
public:
    struct Result { Sample value; std::optional<Sample> guide; };
    void build(const RecorderProject& p, const std::vector<Id>& selected, Sample playhead, TimelineAction action)
    {
        points = {0, playhead}; offsets.clear();
        const std::set<Id> excluded(selected.begin(), selected.end());
        const auto* reference = selected.empty() ? nullptr : p.findClip(selected.front());
        for (const auto& track : p.tracks) for (const auto& clip : track.clips.items()) if (p.isActive(clip))
        {
            if (!excluded.count(clip.clipId)) { points.push_back(clip.timelineStartSample); points.push_back(clip.timelineEnd()); }
            else if (reference)
            {
                if (action == TimelineAction::move)
                { offsets.push_back(clip.timelineStartSample - reference->timelineStartSample); offsets.push_back(clip.timelineEnd() - reference->timelineStartSample); }
                else offsets.push_back(action == TimelineAction::trimIn ? clip.timelineStartSample - reference->timelineStartSample : clip.timelineEnd() - reference->timelineEnd());
            }
        }
        for (const auto& marker : p.markers) points.push_back(marker.sample);
        if (offsets.empty()) offsets.push_back(0);
        std::sort(points.begin(), points.end()); points.erase(std::unique(points.begin(), points.end()), points.end());
        std::sort(offsets.begin(), offsets.end()); offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    }
    Result snap(Sample value, Sample tolerance, bool bypass) const
    {
        Result result {value, {}}; if (bypass) return result;
        auto best = tolerance + 1;
        for (const auto offset : offsets)
        {
            const auto edge = value + offset; const auto next = std::lower_bound(points.begin(), points.end(), edge);
            const auto consider = [&](Sample target)
            {
                const auto distance = std::abs(target - edge);
                if (distance < best && target - offset >= 0) { best = distance; result = {target - offset, target}; }
            };
            if (next != points.end()) consider(*next);
            if (next != points.begin()) consider(*std::prev(next));
        }
        return result;
    }
private:
    std::vector<Sample> points, offsets;
};
}
