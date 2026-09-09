#pragma once
#include "NvencEncoder.h"
#include "VideoCfrScheduler.h"
#include <thread>

namespace gocue::recorder
{
class FileIoFaultAdapter;
struct EncodeQueueSnapshot
{
    unsigned surfaces = 0;
    std::uint64_t packets = 0, packetBytes = 0;
};
class EncodePipeline
{
public:
    EncodePipeline(NvencProfile, Rational nativeRate, unsigned seconds, juce::File output,
                   std::shared_ptr<CaptureTelemetry>, std::unique_ptr<CameraTimeMapper> = {}, FileIoFaultAdapter* = nullptr);
    ~EncodePipeline();
    void start();
    // Decode worker ONLY, before its preview surface is published. Bounded copy,
    // no encode, disk I/O, wait, mutex or preview-owned frame references retained.
    void offer(const VideoSurface&) noexcept;
    void stop(); // after capture.stop(); drain/join on a non-UI worker
    double secondsSinceOrigin() const noexcept;
    bool failed() const noexcept; // Atomic live partial-camera failure; preview/audio continue.
    EncodeQueueSnapshot queueSnapshot() const noexcept; // Approximate, atomic live diagnostics.
    juce::var toJson() const; // after stop only
    static juce::var headroom(NvencProfile, unsigned wallSeconds);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
