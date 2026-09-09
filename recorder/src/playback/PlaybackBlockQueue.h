#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gocue::recorder
{
// Single render worker -> single ASIO consumer. Storage is allocated/touched at
// construction. No ownership transfer or source rendering on the audio callback.
class PlaybackBlockQueue
{
public:
    PlaybackBlockQueue(unsigned maxFrames, unsigned blocks);
    bool tryPush(const float* stereoInterleaved, unsigned frames) noexcept;
    unsigned consume(float* left, float* right, unsigned frames) noexcept;
    std::uint64_t underruns() const noexcept { return missing.load(); }
    unsigned maxBlockFrames() const noexcept { return maximum; }
private:
    const unsigned maximum, capacity;
    std::vector<float> pcm;
    std::vector<unsigned> sizes;
    unsigned offset = 0; // consumer only
    alignas(64) std::atomic<std::uint64_t> written{0}, read{0};
    std::atomic<std::uint64_t> missing{0};
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
    void render(float*, unsigned, std::int64_t, unsigned) override; // L 440 Hz, R 660 Hz, peak .05
};
// Minimal provider pump, replaceable by round 14's renderer. Destruction joins;
// detach queue from the engine before destroying this object.
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
