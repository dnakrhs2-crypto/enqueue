#pragma once
#include "model/RenderPlanCompiler.h"

namespace gocue::recorder
{
// Internal linear envelopes only. All coordinates are project samples, [start,end).
struct MicroFade
{
    static Sample defaultLength(std::uint32_t Fs) noexcept;
    static Sample clampLength(Sample requested, Sample validRunLength) noexcept;
    // The sample touching silence is exactly zero; N=1 is a single zero sample.
    static float in(Sample offset, Sample length) noexcept;
    static float out(Sample offset, Sample length) noexcept;
    static bool continuous(const RenderSpan&, const RenderSpan&, float beforeGain = 1, float afterGain = 1) noexcept;
    static std::vector<MicrofadeBoundary> boundaries(const RenderTrackPlan&, std::uint32_t Fs);
    static float gain(const MicrofadeBoundary&, Sample timelineSample) noexcept;
};
}
