#pragma once
#include "media/MediaIndex.h"
#include <thread>
#include <mutex>

namespace gocue::recorder
{
struct PlaybackAudioClip { RenderClip mapping; std::shared_ptr<const WavSource> source; };
struct PlaybackAudioTrack
{
    Id trackId;
    bool mute = false, solo = false;
    std::vector<PlaybackAudioClip> clips;
};
// Prepared PCM is the replacement boundary for round-09 RecorderAudioEngine.
// One render producer / one RT consumer. Storage is allocated and touched at open.
class PlaybackPcmQueue
{
public:
    PlaybackPcmQueue(std::uint32_t Fs, std::uint32_t blockFrames);
    bool push(Sample first, std::uint64_t generation, const float* left, const float* right, std::uint32_t frames) noexcept;
    // All-or-nothing: underrun never consumes half a callback block.
    bool consume(Sample first, std::uint64_t generation, float* left, float* right, std::uint32_t frames) noexcept;
    std::uint64_t queuedFrames() const noexcept { return queued.load(std::memory_order_acquire); }
    void reset(); // only after producer join AND consumer acknowledged quiescence
    const std::uint32_t blockFrames;
    const std::size_t capacityBlocks;
private:
    struct Block { Sample first = 0; std::uint64_t generation = 0; std::uint32_t frames = 0; };
    std::vector<Block> blocks;
    std::vector<float> pcm;
    alignas(64) std::atomic<std::uint64_t> written{0}, read{0}, queued{0};
    std::uint32_t offset = 0; // consumer only
};
class TimelineAudioRenderer
{
public:
    TimelineAudioRenderer(std::uint32_t Fs, std::uint32_t blockFrames);
    ~TimelineAudioRenderer();
    void setPlan(std::vector<PlaybackAudioTrack>, Sample timelineEnd); // quiescent control owner
    void prepare(Sample, std::uint64_t generation); // starts >=250ms read-ahead worker
    void stopWorker(); // callback must be detached/quiescent before reset/destruction
    bool ready() const noexcept;
    juce::Result status() const;
    Sample length() const noexcept { return end; }
    PlaybackPcmQueue& queue() noexcept { return pcmQueue; }
    // Worker/offline seam for continuous PCM tests, never call alongside prepare().
    void renderAudio(Sample first, std::uint32_t frames, float* left, float* right);
private:
    const std::uint32_t rate;
    PlaybackPcmQueue pcmQueue;
    std::vector<PlaybackAudioTrack> tracks;
    Sample end = 0, preparedAt = 0;
    std::thread worker;
    std::atomic<bool> stopping{false}, failed{false};
    mutable std::mutex errorMutex;
    juce::String error;
};
}
