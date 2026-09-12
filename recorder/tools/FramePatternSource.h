#pragma once
#include "capture/CaptureDecodeRecovery.h"
#include "record/VideoCfrScheduler.h"
#include "record/Ffmpeg.h"
#include <functional>
#include <future>
#include <optional>

namespace gocue::recorder::probe
{
struct PatternId { std::uint32_t frame = 0; unsigned camera = 0; };
// Pixel payload: magic16 + camera8 + frame32 + checksum8. Large neutral luma
// cells survive JPEG/H.264 quantisation; CRC/contrast rejects corrupted IDs.
void paintPattern(VideoSurface&, PatternId);
std::optional<PatternId> readPattern(const std::uint8_t* y, int stride, int width, int height);

class FramePatternOracle
{
public:
    FramePatternOracle(unsigned camera, Rational native, Rational project, unsigned seconds);
    void observe(std::optional<PatternId>, std::optional<std::int64_t> outputPts = {});
    juce::var finish() const;
    std::uint64_t expectedSource(std::uint64_t outputIndex) const;
private:
    unsigned camera;
    Rational native, project;
    std::uint64_t nativeCount, outputCount, decoded = 0, invalid = 0, wrongCamera = 0, mismatches = 0, ptsErrors = 0, badPositions = 0;
    std::uint64_t previous = 0, repeated = 0, regressions = 0;
    std::vector<bool> requiredIds, observedIds; // Offline oracle only, <=216001 bits per set.
    juce::Array<juce::var> firstErrors; // bounded to 32
};
// Full software decode, including flush/EOF. Never certifies a file by CFR count
// alone. Physical camera files have no built-in pixel oracle.
juce::var inspectPatternMp4(const juce::File&, unsigned camera, Rational native, Rational project, unsigned seconds);
juce::var inspectMappedPatternMp4(const juce::File&, const juce::File& sourceTrace, unsigned camera, Rational project);

class FramePatternSource
{
public:
    struct Config
    {
        unsigned camera = 1, fps = 60, seconds = 60;
        CaptureSubtype subtype = CaptureSubtype::nv12;
        ColourDevice colourDevice = ColourDevice::hardware;
        std::uint64_t generation = 1;
    };
    FramePatternSource(Config, VideoSurfacePool&, std::shared_ptr<CaptureTelemetry>, std::function<void(const VideoSurface&)>);
    ~FramePatternSource();
    void start(std::shared_future<void>, const std::atomic<std::int64_t>& originQpc);
    void stop();
    bool finished() const noexcept;
    bool failed() const noexcept; // Includes synthetic source deadline misses.
    juce::var toJson() const; // after stop
    CameraMode mode() const;
    // Producer-only fixture seam, also used by the realtime source thread.
    // bytes is pre-reserved by the caller in live use; JPEG allocation is source
    // generation, never a borrowed-driver callback or a decode worker operation.
    void makePacket(std::uint32_t frame, std::vector<std::uint8_t>& bytes);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
