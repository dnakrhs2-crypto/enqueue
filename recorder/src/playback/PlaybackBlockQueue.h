#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gocue::recorder
{
enum class PlaybackQueueState { idle, prefilling, ready, playing, buffering, failed, ended };
struct PlaybackGenerationGuard
{
    std::shared_ptr<const std::atomic<std::uint64_t>> epoch;
    std::uint64_t expected = 0;
};
// One worker/control producer, one audio consumer. Three preallocated banks allow
// pending revisions to be replaced while the callback owns a different bank.
// Callback ownership is raw storage only; object destruction stays on workers.
class PlaybackBlockQueue
{
public:
    PlaybackBlockQueue(unsigned maxFrames, unsigned blocks);
    ~PlaybackBlockQueue();
    PlaybackBlockQueue(const PlaybackBlockQueue&) = delete;
    bool tryPush(const float* stereoInterleaved, unsigned frames) noexcept;
    unsigned consume(float* left, float* right, unsigned frames) noexcept;
    bool push(std::int64_t first, std::uint64_t generation, const float*, const float*, unsigned frames) noexcept;
    // Transactional: a short/mismatched callback emits silence and consumes nothing.
    bool consume(std::int64_t first, std::uint64_t generation, float*, float*, unsigned frames) noexcept;
    std::uint64_t queuedFrames() const noexcept;
    std::uint64_t underruns() const noexcept;
    unsigned maxBlockFrames() const noexcept;
    PlaybackQueueState state() const noexcept;
    std::int64_t activeRevision() const noexcept;
    std::int64_t submittedEnd() const noexcept;
    // Join producer before begin/reset; reset also requires callback quiescence.
    // Replacement runs during playback at an explicit future callback boundary.
    // Missed prefill returns false at that boundary -> transport buffering.
    void reset();
    std::uint64_t begin(std::int64_t first, std::uint64_t generation, std::int64_t revision,
                        std::int64_t end, std::uint64_t minimumFrames, unsigned rampFrames = 0, bool replacement = false,
                        std::vector<PlaybackGenerationGuard> guards = {});
    void cancelProducer() noexcept;
    bool pushPrepared(std::uint64_t ticket, std::int64_t first, const float*, const float*, unsigned frames) noexcept;
    void fail(std::uint64_t ticket) noexcept;
    bool ready() const noexcept;
    std::uint64_t prefetchedFrames() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// Round-11 transport compatibility; the same SPSC implementation can be passed
// directly to RecorderAudioEngine::setPlaybackQueue().
class PlaybackPcmQueue final : public PlaybackBlockQueue
{
public:
    PlaybackPcmQueue(std::uint32_t Fs, std::uint32_t frames);
    const std::uint32_t blockFrames;
    const std::size_t capacityBlocks;
};
class IPlaybackBlockProvider
{
public:
    virtual ~IPlaybackBlockProvider() = default;
    virtual void render(float* stereo, unsigned frames, std::int64_t firstSample, unsigned sampleRate) = 0;
};
class SilenceBlockProvider final : public IPlaybackBlockProvider
{
public:
    void render(float*, unsigned, std::int64_t, unsigned) override;
};
class ToneBlockProvider final : public IPlaybackBlockProvider
{
public:
    void render(float*, unsigned, std::int64_t, unsigned) override;
};
// Initial prefetch >=250ms; exceptions are observable through queue().state().
// Detach the engine before destruction.
class PlaybackProviderPump
{
public:
    PlaybackProviderPump(unsigned Fs, unsigned block, std::unique_ptr<IPlaybackBlockProvider>);
    ~PlaybackProviderPump();
    PlaybackBlockQueue& queue();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
