#pragma once
#include "RecorderModel.h"

namespace gocue::recorder
{
struct RenderSpan
{
    SampleRange timeline;
    Id clipId, assetId; // empty assetId means silence/black, including unavailable source
    Sample sourceIn = 0, mediaGeneration = 0;
    std::uint64_t sourceUnitsNumerator = 1, sourceUnitsDenominator = 1;
    bool isGap() const { return assetId.isEmpty(); }
};
struct RenderTrackPlan
{
    Id trackId;
    TrackKind kind;
    bool mute = false, solo = false, audible = false;
    std::vector<RenderSpan> spans; // disjoint partition of [0, timelineEnd), including tail padding
};
struct MicrofadeBoundary
{
    Id trackId;
    Sample timelineSample = 0, beforeSamples = 0, afterSamples = 0;
};
// Extends round 08's RenderPlan without changing the shared model ABI.
// RenderClip::gaps are offsets from that clip's timelineStartSample.
struct CompiledRenderPlan final : RenderPlan
{
    Sample timelineEnd = 0;
    size_t audibleTrackCount = 0; // fixed K: includes selected lanes even during gaps
    std::vector<RenderTrackPlan> tracks;
    // Authoritative envelopes over continuous source runs. An envelope may cross a
    // source-continuous split; introducing that split must not change rendered PCM.
    std::vector<MicrofadeBoundary> microfadeBoundaries;
    explicit CompiledRenderPlan(const RecorderProject&, std::vector<RenderClip>);
};
class RenderPlanCompiler
{
public:
    // Owner/worker thread only. Throws on invalid input/overflow; no handles or cache state.
    static std::shared_ptr<const CompiledRenderPlan> compile(const RecorderProject&);
    // Absolute source logical coordinate -> native sample or CFR frame index.
    // Video floors (last PTS <= u); audio rounds. VFR readers must apply the PTS oracle
    // to their actual source index, using the same logical u instead of assuming CFR.
    static Sample sourceUnitAt(const RenderSpan&, Sample timelineSample, bool video);
};
}
