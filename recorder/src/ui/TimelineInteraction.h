#pragma once
#include "TimelineView.logic.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace gocue::recorder
{
// Display calculations must not wrap into a different, editable sample.
struct TimelineSamples
{
    static std::optional<Sample> add(Sample a, Sample b)
    {
        constexpr auto hi = (std::numeric_limits<Sample>::max)(), lo = (std::numeric_limits<Sample>::min)();
        if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b)) return {};
        return a + b;
    }
    static std::optional<Sample> subtract(Sample a, Sample b)
    {
        constexpr auto hi = (std::numeric_limits<Sample>::max)(), lo = (std::numeric_limits<Sample>::min)();
        if ((b > 0 && a < lo + b) || (b < 0 && a > hi + b)) return {};
        return a - b;
    }
    static std::uint64_t distance(Sample a, Sample b)
    {
        // Unsigned subtraction represents even the full INT64_MIN..INT64_MAX span.
        return a >= b ? std::uint64_t(a) - std::uint64_t(b) : std::uint64_t(b) - std::uint64_t(a);
    }
    static Sample roundNonnegative(double value, Sample maximum = (std::numeric_limits<Sample>::max)())
    {
        if (!(value > 0)) return 0;
        // double(INT64_MAX) rounds up to 2^63; reject it before llround.
        if (!std::isfinite(value) || value >= double(maximum)) return maximum;
        return Sample(std::llround(value));
    }
    static std::optional<Sample> sourceAt(const Clip& clip, Sample timeline)
    {
        const auto offset = subtract(timeline, clip.timelineStartSample);
        return offset ? add(clip.sourceIn, *offset) : std::nullopt;
    }
};
// Interaction policy shared by the view and device-free regressions.
struct TimelineInteraction
{
    static constexpr int dragThreshold = 4, snapPixels = 12, edgePixels = 32;
    static Sample snapTolerance(double seconds, unsigned sampleRate, int width, FrameRate fps = {})
    {
        const auto pixels = TimelineSamples::roundNonnegative(seconds * (double(sampleRate) * snapPixels / (std::max)(1, width)));
        const auto numerator = std::uint64_t(sampleRate) * fps.denominator;
        const auto denominator = (std::max)(1u, fps.numerator);
        const auto frame = numerator / denominator + (numerator % denominator != 0);
        const auto halfDenominator = std::uint64_t(denominator) * 2;
        const auto halfFrame = numerator / halfDenominator + (numerator % halfDenominator != 0);
        const auto limit = std::uint64_t((std::numeric_limits<Sample>::max)());
        // One frame for seam closure, plus half a frame for subsequent grid rounding.
        // Thus an unsnapped pointer outside this radius cannot round into a tiny seam.
        const auto seamRadius = frame >= limit || halfFrame >= limit - frame ? limit : frame + halfFrame;
        const auto minimum = (std::max)(seamRadius, std::uint64_t(sampleRate / 50 + (sampleRate % 50 != 0))); // 20 ms
        return (std::max)({Sample{1}, pixels, Sample((std::min)(minimum, std::uint64_t((std::numeric_limits<Sample>::max)())))});
    }
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
    struct Result { Sample value; std::optional<Sample> guide; bool clipBoundary = false; };
    void build(const RecorderProject& p, const std::vector<Id>& selected, Sample playhead, TimelineAction action)
    {
        points = {0, playhead}; clipPoints.clear(); offsets.clear();
        const std::set<Id> excluded(selected.begin(), selected.end());
        const auto* reference = selected.empty() ? nullptr : p.findClip(selected.front());
        const auto addOffset = [&](Sample edge, Sample base)
        { if (const auto offset = TimelineSamples::subtract(edge, base)) offsets.push_back(*offset); };
        for (const auto& track : p.tracks) for (const auto& clip : track.clips.items()) if (p.isActive(clip))
        {
            if (!excluded.count(clip.clipId)) { clipPoints.push_back(clip.timelineStartSample); clipPoints.push_back(clip.timelineEnd()); }
            else if (reference)
            {
                if (action == TimelineAction::move)
                { addOffset(clip.timelineStartSample, reference->timelineStartSample); addOffset(clip.timelineEnd(), reference->timelineStartSample); }
                else if (action == TimelineAction::trimIn) addOffset(clip.timelineStartSample, reference->timelineStartSample);
                else addOffset(clip.timelineEnd(), reference->timelineEnd());
            }
        }
        for (const auto& marker : p.markers) points.push_back(marker.sample);
        if (offsets.empty()) offsets.push_back(0);
        std::sort(points.begin(), points.end()); points.erase(std::unique(points.begin(), points.end()), points.end());
        std::sort(clipPoints.begin(), clipPoints.end()); clipPoints.erase(std::unique(clipPoints.begin(), clipPoints.end()), clipPoints.end());
        std::sort(offsets.begin(), offsets.end()); offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    }
    Result snap(Sample value, Sample tolerance, bool bypass) const
    {
        Result result {value, {}}; if (bypass || tolerance < 0) return result;
        auto best = std::uint64_t(tolerance);
        for (const auto offset : offsets)
        {
            const auto edge = TimelineSamples::add(value, offset); if (!edge) continue;
            const auto consider = [&](Sample target, bool boundary)
            {
                const auto distance = TimelineSamples::distance(target, *edge);
                const auto candidate = TimelineSamples::subtract(target, offset);
                if (candidate && *candidate >= 0 && distance <= best
                    && (!result.guide || distance < best || (distance == best && boundary && !result.clipBoundary)))
                { best = distance; result = {*candidate, target, boundary}; }
            };
            const auto search = [&](const std::vector<Sample>& candidates, bool boundary)
            {
                const auto next = std::lower_bound(candidates.begin(), candidates.end(), *edge);
                if (next != candidates.end()) consider(*next, boundary);
                if (next != candidates.begin()) consider(*std::prev(next), boundary);
            };
            search(clipPoints, true); search(points, false);
        }
        return result;
    }
private:
    std::vector<Sample> points, clipPoints, offsets;
};
}
