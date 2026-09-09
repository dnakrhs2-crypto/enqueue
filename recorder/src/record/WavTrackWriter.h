#pragma once
#include "storage/RecordingJournal.h"
#include "media/PeakCache.h"
#include <memory>
#include <functional>

namespace gocue::recorder
{
// One set of mono tracks is owned by one worker, so all microphones cross the
// same 30 * Fs sample boundary, including when an input block straddles it.
class WavTrackWriter
{
public:
    static constexpr unsigned chunkSeconds = 30, queueSeconds = 4;
    enum class State { idle, starting, running, stopping, stopped, failed };
    enum class Error { none, io, queueOverflow, invalidBlock, discontinuity, invalidPcm, internal };
    struct Config
    {
        juce::File projectDirectory;
        juce::Uuid takeId;
        std::uint32_t sampleRate = 48000, mics = 8, framesPerBlock = 480;
        std::int64_t n0 = 0, o0 = 0, pstart = 0;
        bool usesOutputOrigin = false;
        juce::String placementMode = "normal";
        juce::String nativeFormat = "signed PCM24 in right-aligned int32; no conversion";
        std::vector<JournalDeviceMapping> devices;
        std::vector<JournalFileDescription> additionalFiles; // Camera/index owner remains separate.
        std::uint64_t journalRotationBytes = 8 * 1024 * 1024;
        FileIoFaultAdapter* faults = nullptr;
        // Product connection: the take coordinator owns lifecycle records and
        // serialises this worker's checkpoints with MP4/take commits. Empty keeps
        // round 06's standalone journal behaviour. Must outlive stop/join.
        std::function<juce::Result(const JournalCheckpoint&)> checkpointSink;
        // One entry per packed channel; logical microphone IDs need not be dense.
        std::vector<unsigned> logicalMicrophones;
        std::uint64_t testChunkFrames = 0; // Nonzero only with a fault adapter; production remains 30s.
        std::shared_ptr<PeakCache> peakCache; // derived min/max, accumulated on this writer worker
    };
    explicit WavTrackWriter(Config); // Allocates/touches the entire four-second PCM queue.
    ~WavTrackWriter();
    WavTrackWriter(const WavTrackWriter&) = delete;
    WavTrackWriter& operator=(const WavTrackWriter&) = delete;
    juce::Result start(); // Control thread waits for initial WAV + TakeStarted durable flush.
    // SINGLE RT producer. Only bounded copies and lock-free atomics; no I/O,
    // allocation, clock query, packing, sleep, notifications or JSON here.
    // Frames are interleaved signed right-aligned PCM24; firstSample is take-relative.
    // callbackQpc is optional diagnostic provenance (0 = unavailable).
    bool tryPush(const std::int32_t* interleaved, std::uint32_t frames,
                 std::uint64_t firstSample, std::int64_t callbackQpc = 0) noexcept;
    // Detach/join the producer BEFORE calling stop. Nstop is exclusive in the
    // configured N0/O0 clock. Failure preserves originals and never finalizes.
    juce::Result stop(std::int64_t nstop, const juce::Uuid& placementEditId);
    State state() const noexcept;
    Error error() const noexcept;
    juce::Result status() const; // Control thread only; copies the error text.
    std::uint64_t queueFrames() const noexcept;
    std::uint64_t queueCapacityFrames() const noexcept;
    std::uint64_t writtenSamples() const noexcept;
    std::uint64_t mediaDurableSamples() const noexcept;
    std::uint64_t journalDurableSamples() const noexcept;
    juce::var telemetry() const; // Only after stop/join.
    static juce::String chunkPath(const juce::Uuid& take, unsigned mic, std::uint64_t chunk);
    static bool packPcm24(std::int32_t sample, std::uint8_t* threeBytes) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
