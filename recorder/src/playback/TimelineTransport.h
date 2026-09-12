#pragma once
#include "IAudioOutput.h"
#include "TimelineAudioRenderer.h"
#include "VideoPlaybackEngine.h"
#include "support/BoundedSpscQueue.h"

namespace gocue::recorder
{
class VideoPlaybackEngine;
enum class TransportState { stopped, preparing, ready, scheduled, playing, paused, buffering, draining, failed };
const char* transportStateName(TransportState) noexcept;
struct TransportSnapshot
{
    TransportState state = TransportState::stopped;
    std::uint64_t generation = 0, underruns = 0;
    Sample frozenSample = 0, timelineOrigin = 0, outputOrigin = -1, submittedEnd = 0;
    Sample renderedEnd = 0, softwareQueuedSamples = 0, outputSample = 0;
    std::int64_t callbackQpc = 0, firstBlockQpc = 0, firstAudibleQpc = 0;
    std::uint32_t blockFrames = 0;
    int outputLatency = 0;
};
class TimelineTransport final : public IAudioOutputClient
{
public:
    TimelineTransport(std::uint32_t Fs, std::int64_t qpcFrequency, PlaybackPcmQueue&, Sample timelineEnd);
    // One non-RT command owner. Seeks coalesce at the next callback boundary.
    void seek(Sample);
    void scrub(Sample, bool mouseReleased, std::int64_t nowQpc); // drag <=15Hz, release immediately exact
    void play();
    void pause();
    void stop();
    void goToStart();
    void stagePlan(std::shared_ptr<const CompiledRenderPlan>, std::vector<AudioSourceBinding>,
                   std::vector<PlaybackVideoClip>, AudioSourceMask = {});
    // Single-owner coordinator: wait on wakeHandle() plus window messages, then
    // service. Video publication/receipt and ASIO publication signal immediately.
    // File/GPU preparation remains on workers; no service() call from the callback.
    void service(TimelineAudioRenderer&, VideoPlaybackEngine&, IAudioOutput&, std::int64_t nowQpc);
    void* wakeHandle() const noexcept { return wake->nativeHandle(); }
    void prepared(std::int64_t reservedOutputSample, bool start); // readiness seam for offline stubs
    void processOutput(const BlockStamp&, float*, float*) noexcept override;
    TransportSnapshot snapshot() const noexcept;
    Sample playhead(std::int64_t nowQpc) const noexcept; // owner: includes unacknowledged/throttled scrubs
    std::uint64_t generation() const noexcept { return requestedGeneration; } // control owner
    juce::Result status() const;
    // Call only after normal output is detached. Dubbing shares the Recorder
    // ASIO device; while locked no seek/play/pause/output preparation can mutate it.
    void setDubbingLocked(bool locked) noexcept { dubbingLocked = locked; }
    bool isDubbingLocked() const noexcept { return dubbingLocked; }
    juce::var telemetry() const; // control owner; no JSON in the ASIO callback
    static Sample audibleCursor(const TransportSnapshot&, std::uint32_t Fs, std::int64_t qpcFrequency,
                                std::int64_t nowQpc, std::int64_t displayLeadTicks = 0) noexcept;
private:
    enum class Kind { prepare, ready, start, pause, stopped, fail };
    struct Command { Kind kind; std::uint64_t generation; Sample target; std::int64_t output; Sample timelineEnd = 0; };
    void send(Command);
    void publish(const BlockStamp&) noexcept;
    const std::uint32_t rate;
    const std::int64_t frequency;
    PlaybackPcmQueue& queue;
    Sample end;
    Sample rtEnd; // callback-owned; changes with the acknowledged Prepare command
    struct PendingPlan
    {
        std::shared_ptr<const CompiledRenderPlan> plan;
        std::vector<AudioSourceBinding> sources;
        std::vector<PlaybackVideoClip> videos;
        AudioSourceMask mask;
    };
    std::unique_ptr<PendingPlan> pendingPlan; // control owner only
    const std::shared_ptr<PlaybackWakeEvent> wake = std::make_shared<PlaybackWakeEvent>();
    BoundedSpscQueue<Command, 64> commands;
    struct SeekMailbox
    {
        std::atomic<std::uint64_t> sequence{0}, generation{0};
        std::atomic<Sample> target{0}, timelineEnd{0};
    } latestSeek;
    // RT-owned state. The control owner only reads the atomic publication below.
    TransportSnapshot rt;
    BlockStamp previous{};
    bool havePrevious = false;
    struct Publication
    {
        std::atomic<std::uint64_t> sequence{0}, generation{0}, underruns{0};
        std::atomic<int> state{0}, latency{0};
        std::atomic<std::uint32_t> frames{0};
        std::atomic<Sample> frozen{0}, origin{0}, outputOrigin{-1}, submitted{0}, rendered{0}, queued{0}, output{0};
        std::atomic<std::int64_t> qpc{0}, first{0}, audible{0};
    } published;
    std::uint64_t requestedGeneration = 0, preparingGeneration = 0, videoGeneration = 0, armedGeneration = 0;
    struct PreparationTiming
    {
        std::int64_t request = 0, callbackAck = 0, audioBegin = 0, audioEnd = 0;
        std::int64_t audioReady = 0, videoReady = 0, armed = 0;
    } timing;
    Sample requestedSample = 0;
    bool wantPlay = false;
    bool stopAfterPrepare = false;
    Sample pendingScrub = 0;
    std::int64_t lastScrubQpc = 0;
    bool scrubPending = false;
    std::uint64_t scrubInputs = 0, scrubDispatches = 0;
    juce::String error; // control owner only
    bool dubbingLocked = false;
};
}
