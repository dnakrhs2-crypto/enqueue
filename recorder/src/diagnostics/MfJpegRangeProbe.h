#pragma once
#include "support/Platform.h"
#include <array>
#include <vector>

namespace gocue::recorder
{
using YuvValue = std::array<double, 3>;
enum class JpegOutputModel { unknown, full601, limited601, full709, limited709 };
struct JpegPatch { const char* name; YuvValue input; };
const std::array<JpegPatch, 11>& jpegRangePatches();
const char* jpegOutputModelName(JpegOutputModel) noexcept;
YuvValue expectedJpegOutput(YuvValue full601, JpegOutputModel);
struct JpegRangeDecision
{
    JpegOutputModel model = JpegOutputModel::unknown;
    std::array<double, 4> rmse{};
    double bestMaxError = 0, runnerUpMargin = 0;
    juce::var toJson() const;
};
JpegRangeDecision classifyJpegOutput(const std::vector<YuvValue>& observed);
// MCU-aligned flat patches, sampled well away from JPEG/chroma boundaries.
std::vector<std::uint8_t> makeJpegRangeFixture(unsigned width, unsigned height);
// Caller owns COM + MF startup. No camera, HWND, GPU, or SourceReader is opened.
juce::var probeMfJpegRange();
}
