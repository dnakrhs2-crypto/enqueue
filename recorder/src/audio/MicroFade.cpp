#include "MicroFade.h"
#include <algorithm>
#include <limits>

namespace gocue::recorder
{
Sample MicroFade::defaultLength(std::uint32_t Fs) noexcept { return (std::uint64_t(Fs) * 3 + 500) / 1000; }
Sample MicroFade::clampLength(Sample requested, Sample length) noexcept
{ return (std::max)(Sample{0}, (std::min)(requested, length / 2)); }
float MicroFade::in(Sample offset, Sample length) noexcept
{
    if (length <= 0 || offset >= length) return 1;
    if (offset <= 0 || length == 1) return 0;
    return static_cast<float>(offset) / static_cast<float>(length - 1);
}
float MicroFade::out(Sample offset, Sample length) noexcept
{ return length <= 0 ? 1 : in(length - 1 - offset, length); }
bool MicroFade::continuous(const RenderSpan& a, const RenderSpan& b, float ga, float gb) noexcept
{
    const auto limit = (std::numeric_limits<Sample>::max)();
    return !a.isGap() && !b.isGap() && a.timeline.length > 0 && a.timeline.start >= 0 && a.sourceIn >= 0
        && a.timeline.length <= limit - a.timeline.start && a.timeline.length <= limit - a.sourceIn
        && a.timeline.start + a.timeline.length == b.timeline.start && a.sourceIn + a.timeline.length == b.sourceIn
        && a.assetId == b.assetId && a.mediaGeneration == b.mediaGeneration && ga == gb
        && a.sourceUnitsNumerator == b.sourceUnitsNumerator && a.sourceUnitsDenominator == b.sourceUnitsDenominator;
}
std::vector<MicrofadeBoundary> MicroFade::boundaries(const RenderTrackPlan& track, std::uint32_t Fs)
{
    std::vector<MicrofadeBoundary> result;
    if (track.kind != TrackKind::mic && track.kind != TrackKind::importAudio) return result;
    std::vector<RenderSpan> runs;
    for (const auto& span : track.spans)
        if (!runs.empty() && continuous(runs.back(), span)) runs.back().timeline.length += span.timeline.length;
        else runs.push_back(span);
    for (std::size_t i = 0; i <= runs.size(); ++i)
    {
        const auto* a = i ? &runs[i - 1] : nullptr;
        const auto* b = i < runs.size() ? &runs[i] : nullptr;
        const auto before = a && !a->isGap() ? clampLength(defaultLength(Fs), a->timeline.length) : 0;
        const auto after = b && !b->isGap() ? clampLength(defaultLength(Fs), b->timeline.length) : 0;
        if (before || after) result.push_back({track.trackId, b ? b->timeline.start : a->timeline.start + a->timeline.length, before, after});
    }
    return result;
}
float MicroFade::gain(const MicrofadeBoundary& b, Sample at) noexcept
{
    if (at < b.timelineSample && b.timelineSample - at <= b.beforeSamples)
        return out(at - (b.timelineSample - b.beforeSamples), b.beforeSamples);
    if (at >= b.timelineSample && at - b.timelineSample < b.afterSamples)
        return in(at - b.timelineSample, b.afterSamples);
    return 1;
}
}
