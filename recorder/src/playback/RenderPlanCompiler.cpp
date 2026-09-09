#include "RenderPlanCompiler.h"
#include "audio/MicroFade.h"
#include <algorithm>
#include <intrin.h>
#include <limits>
#include <stdexcept>

namespace gocue::recorder
{
namespace
{
bool audio(TrackKind kind) { return kind == TrackKind::mic || kind == TrackKind::importAudio; }


}
CompiledRenderPlan::CompiledRenderPlan(const RecorderProject& p, std::vector<RenderClip> clips)
    : RenderPlan{p.projectId, p.editRevision, p.Fs, p.fps, std::move(clips)}, timelineEnd(p.activeTimelineEnd()) {}
std::shared_ptr<const CompiledRenderPlan> RenderPlanCompiler::compile(const RecorderProject& p)
{
    const auto valid = p.validate(); if (valid.failed()) throw std::invalid_argument(valid.getErrorMessage().toStdString());
    if (p.Fs > 768000) throw std::invalid_argument("Unsupported render sample rate");
    std::vector<RenderClip> clips; std::vector<RenderTrackPlan> tracks;
    std::vector<MicrofadeBoundary> fades;
    const auto timelineEnd = p.activeTimelineEnd();
    const bool anySolo = std::any_of(p.tracks.begin(), p.tracks.end(), [](const Track& t) { return audio(t.kind) && t.solo; });

    size_t audibleCount = 0;
    for (const auto& t : p.tracks)
    {
        const auto firstClip = clips.size();
        RenderTrackPlan lane{t.trackId, t.kind, t.mute, t.solo, audio(t.kind) && !t.mute && (!anySolo || t.solo), {}};
        if (lane.audible) ++audibleCount;
        std::vector<const Clip*> active;
        for (const auto& c : t.clips.items()) if (p.isActive(c)) active.push_back(&c);
        std::sort(active.begin(), active.end(), [](const Clip* a, const Clip* b) { return a->timelineStartSample < b->timelineStartSample; });
        Sample cursor = 0;
        const auto gap = [&](Sample start, Sample finish, const Id& clipId = Id{})
        { if (finish > start) { RenderSpan span; span.timeline = {start, finish - start}; span.clipId = clipId; lane.spans.push_back(span); } };
        for (const auto* c : active)
        {
            const auto& asset = *p.media->findAsset(c->assetId);
            gap(cursor, c->timelineStartSample);
            RenderClip render; render.clipId = c->clipId; render.trackId = t.trackId; render.assetId = c->assetId;
            render.sourceIn = c->sourceIn; render.timelineStartSample = c->timelineStartSample; render.lengthSamples = c->lengthSamples;
            render.mediaGeneration = asset.mediaGeneration; render.sourceUnitsNumerator = asset.sourceUnitsNumerator; render.sourceUnitsDenominator = asset.sourceUnitsDenominator;
            render.mute = t.mute; render.solo = t.solo;
            auto partition = asset.availableRanges; partition.insert(partition.end(), asset.gaps.begin(), asset.gaps.end());
            std::sort(partition.begin(), partition.end(), [](SampleRange a, SampleRange b) { return a.start < b.start; });
            for (const auto r : partition)
            {
                const auto a = (std::max)(r.start, c->sourceIn), b = (std::min)(r.start + r.length, c->sourceIn + c->lengthSamples);
                if (a >= b) continue;
                const auto start = c->timelineStartSample + (a - c->sourceIn);
                const bool missing = std::any_of(asset.gaps.begin(), asset.gaps.end(), [&](SampleRange g) { return g.start == r.start; });
                if (missing) { gap(start, start + (b - a), c->clipId); render.gaps.push_back({a - c->sourceIn, b - a}); }
                else lane.spans.push_back({{start, b - a}, c->clipId, c->assetId, a, asset.mediaGeneration, asset.sourceUnitsNumerator, asset.sourceUnitsDenominator});
            }
            clips.push_back(render); cursor = c->timelineEnd();
        }
        gap(cursor, timelineEnd);
        if (audio(t.kind))
        {
            for (const auto& boundary : MicroFade::boundaries(lane, p.Fs))
            {
                fades.push_back(boundary);
                // Valid active clips on this lane are ordered and disjoint, so
                // both their starts and ends are strictly increasing. Avoid a
                // full-project scan per fade (quadratic at 10,000 clips).
                const auto begin = clips.begin() + static_cast<std::ptrdiff_t>(firstClip), end = clips.end();
                const auto in = std::lower_bound(begin, end, boundary.timelineSample,
                    [](const RenderClip& c, Sample at) { return c.timelineStartSample < at; });
                if (in != end && in->timelineStartSample == boundary.timelineSample) in->microfadeInSamples = boundary.afterSamples;
                const auto out = std::lower_bound(begin, end, boundary.timelineSample,
                    [](const RenderClip& c, Sample at) { return c.timelineStartSample + c.lengthSamples < at; });
                if (out != end && out->timelineStartSample + out->lengthSamples == boundary.timelineSample) out->microfadeOutSamples = boundary.beforeSamples;
            }
        }
        tracks.push_back(std::move(lane));
    }
    auto plan = std::make_shared<CompiledRenderPlan>(p, std::move(clips)); plan->tracks = std::move(tracks);
    plan->audibleTrackCount = audibleCount; plan->microfadeBoundaries = std::move(fades); return plan;
}
std::shared_ptr<const AudioRenderPlan> compileAudioRenderPlan(const RecorderProject& p)
{
    auto result = std::make_shared<AudioRenderPlan>();
    result->timeline = RenderPlanCompiler::compile(p);
    for (const auto& asset : p.media->assets)
        if (asset.kind != AssetKind::camera && std::any_of(result->timeline->activeClips.begin(), result->timeline->activeClips.end(),
            [&](const RenderClip& c) { return c.assetId == asset.assetId; })) result->sources.push_back(asset);
    return result;
}
Sample RenderPlanCompiler::sourceUnitAt(const RenderSpan& s, Sample t, bool video)
{
    if (s.isGap() || s.timeline.start < 0 || s.timeline.length <= 0 || s.sourceIn < 0
        || t < s.timeline.start || t - s.timeline.start >= s.timeline.length
        || s.sourceIn > (std::numeric_limits<Sample>::max)() - (t - s.timeline.start)
        || s.sourceUnitsNumerator == 0 || s.sourceUnitsDenominator == 0)
        throw std::invalid_argument("유효한 원본 조각 내부의 시각이 필요합니다.");
    const auto u = s.sourceIn + (t - s.timeline.start);
    if (!video) return rescaleRound(u, s.sourceUnitsNumerator, s.sourceUnitsDenominator);
    unsigned __int64 high = 0, remainder = 0;
    const auto low = _umul128(static_cast<std::uint64_t>(u), s.sourceUnitsNumerator, &high);
    if (high >= s.sourceUnitsDenominator) throw std::overflow_error("원본 프레임 범위를 초과했습니다.");
    const auto frame = _udiv128(high, low, s.sourceUnitsDenominator, &remainder);
    if (frame > static_cast<std::uint64_t>((std::numeric_limits<Sample>::max)())) throw std::overflow_error("원본 프레임 범위를 초과했습니다.");
    return static_cast<Sample>(frame);
}
}
