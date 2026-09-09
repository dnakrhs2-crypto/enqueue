#pragma once
#include "TakeController.h"
#include "model/TakeStackEdits.h"
#include "sync/CalibrationProfile.h"
#include "sync/CameraClockMapper.h"

namespace gocue::recorder
{
class TimelineTransport;
class DubbingController
{
public:
    using State = TakeController::State;
    enum class Failure { none, playbackUnderrun, asioReset, clockDiscontinuity, preparation, storage, cancelled };
    struct Camera
    {
        std::string symbolicLink;
        CameraMode mode;
        CalibrationProfile calibration;
        std::uint64_t generation = 0;
        std::string exposure = "uncontrolled";
    };
    struct Config
    {
        juce::File projectDirectory;
        juce::Uuid takeId;
        Id audioTrackId;
        Sample Pstart = 0;
        // Zero means the first take runs until Stop or the selected audio's end.
        Sample spanSamples = 0;
        Id retakeStack;
        std::vector<Camera> cameras;
        bool recordMicrophones = false, synthetic = false;
        bool externalCapture = false;
        std::vector<int> outputMapping;
    };
    struct Placement
    {
        Sample Pstart = 0, O0 = -1, Ostop = -1, outputSubmissionSample = -1;
        Sample spanSamples = 0, recordedSamples = 0;
        Id stackId, versionId;
    };
    using VideoFactory = std::function<std::unique_ptr<ITakeVideoStream>(unsigned, std::unique_ptr<CameraTimeMapper>)>;
    using AudioFactory = std::function<std::unique_ptr<IPlaybackBlockProvider>(const RecorderProject&, const juce::File&, const Id&)>;
    DubbingController(RecorderDocument&, RecorderAudioEngine&, TimelineTransport* = nullptr, VideoFactory = {}, AudioFactory = {});
    ~DubbingController();
    // Owner thread; all file/cache/codec work is on workers. tick() at 5-10ms
    // during stop publishes the captured version before file finalization.
    juce::Result prepare(Config);
    juce::Result start(Sample outputSubmissionSample = -1);
    juce::Result stop(Sample Ostop = -1);
    juce::Result retake(); // prepares the last stack's current anchor/span; new take ID
    juce::Result retake(bool recordMicrophones);
    void tick();
    void abort(Failure = Failure::cancelled) noexcept;
    State state() const noexcept;
    bool locked() const noexcept;
    Failure failure() const noexcept;
    juce::String error() const;
    juce::String statusText() const;
    const Placement& placement() const noexcept;
    const Id& selectedAudioTrack() const noexcept;
    void offer(unsigned camera, const VideoSurface&) noexcept; // sole decode producer per camera
    TakeVideoQueues cameraQueues(unsigned camera) const noexcept; // owner thread, atomic worker counters
    void cameraFailed(unsigned camera, std::uint64_t generation = 0) noexcept;
    std::shared_ptr<VideoSurfacePool> previewPool(unsigned camera) const;
    juce::var report() const; // after done/partialFailure
    juce::var calibrationOffsetReport() const; // same captured stamp + frozen clock, changed profile values

    // Worker/offline adapter over round-16 verified PCM caches. Both playback and
    // reference AAC use independent readers of this same selected track mapping.
    static std::unique_ptr<IPlaybackBlockProvider> prepareReferenceAudio(const RecorderProject&,
        const juce::File& projectDirectory, const Id& audioTrack);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
