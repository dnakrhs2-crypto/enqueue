#pragma once
#include "audio/RecorderAudioEngine.h"
#include "app/RecorderDocument.h"
#include "NvencEncoder.h"
#include "VideoCfrScheduler.h"
#include <functional>

namespace gocue::recorder
{
// One camera stream seam. The production implementation owns independent encode
// and mux workers. Tests can inject a bounded lifecycle double without a GPU.
class ITakeVideoStream
{
public:
    virtual ~ITakeVideoStream() = default;
    virtual void prepare(const juce::File& finalFile, NvencProfile, Rational nativeRate, const AVCodecContext& audio) = 0;
    virtual void startAt(ClockMapping, std::int64_t N0, unsigned Fs, std::function<std::int64_t()> acceptedLength) = 0;
    virtual void offer(const VideoSurface&) noexcept = 0;
    virtual void audioPacket(const AVPacket&) = 0; // AAC worker, retain a packet ref if queued
    virtual bool ready() const noexcept = 0;
    virtual void sourceFailed(std::int64_t relativeSample) noexcept = 0;
    virtual void endAt(std::int64_t logicalLength) noexcept = 0;
    virtual void audioDone() noexcept = 0;
    virtual void finish() = 0; // finalization worker only
    virtual bool failed() const noexcept = 0;
    virtual std::int64_t availableSamples() const noexcept = 0;
    virtual bool thumbnailReady() const noexcept = 0;
    virtual juce::var report() const = 0; // after finish
};

class TakeController
{
public:
    enum class State { idle, preparing, armed, recording, stopping, finalizing, done, partialFailure };
    struct Config
    {
        juce::File projectDirectory;
        juce::Uuid takeId;
        std::string cameraSymbolicLink;
        CameraMode cameraMode;
        bool synthetic = false;
        int projectFps = 60;
    };
    struct PlacementMetadata
    {
        bool ready = false, peaksComplete = false;
        std::int64_t N0 = -1, Nstop = -1, timelineSample = 0;
        unsigned sampleRate = 0;
        std::array<float, 8> peaks{};
        juce::File firstThumbnail;
    };
    using VideoFactory = std::function<std::unique_ptr<ITakeVideoStream>()>;
    TakeController(RecorderDocument&, RecorderAudioEngine&, VideoFactory = {});
    ~TakeController();
    // Owner/message thread commands; slow work is deferred to workers. tick must
    // run regularly (e.g. 5-10ms during stop) on this same document owner thread.
    juce::Result prepare(Config);
    juce::Result start(std::int64_t N0 = -1);
    juce::Result stop(std::int64_t Nstop = -1);
    void tick();
    State state() const noexcept;
    bool structureEditingLocked() const noexcept;
    juce::String statusText() const;
    juce::String warning() const;
    juce::String error() const;
    std::int64_t scheduledStart() const noexcept;
    std::int64_t logicalLength() const noexcept;
    std::int64_t placementSample() const noexcept;
    // Owner-thread cache is populated with placeTake, without scanning media.
    // Peaks contain the converted prefix until peaksComplete after worker drain.
    const PlacementMetadata& placementMetadata() const noexcept;
    // Decode producer (or synthetic probe) only; callback never holds preview refs.
    void offer(const VideoSurface&) noexcept;
    void cameraFailed() noexcept;
    std::shared_ptr<VideoSurfacePool> previewPool() const;
    juce::var report() const; // done/partialFailure only
    static std::int64_t frameCount(std::int64_t samples, unsigned Fs, FrameRate fps);
    static const char* stateName(State) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
