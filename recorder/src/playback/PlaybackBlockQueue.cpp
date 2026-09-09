#include "PlaybackBlockQueue.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace gocue::recorder
{
PlaybackBlockQueue::PlaybackBlockQueue(unsigned frames, unsigned blocks)
    : maximum(frames), capacity(blocks)
{
    if (!frames || frames > 16384 || !blocks || blocks > 65536
        || std::uint64_t(frames) * blocks * 2 > 32 * 1024 * 1024)
        throw std::invalid_argument("Playback queue capacity out of range");
    pcm.resize(std::size_t(frames) * blocks * 2, 0); sizes.resize(blocks, 0);
}
bool PlaybackBlockQueue::tryPush(const float* data, unsigned frames) noexcept
{
    const auto w = written.load(std::memory_order_relaxed);
    if (!data || !frames || frames > maximum || w - read.load(std::memory_order_acquire) == capacity) return false;
    std::memcpy(pcm.data() + (w % capacity) * maximum * 2, data, std::size_t(frames) * 2 * sizeof(float));
    sizes[w % capacity] = frames; written.store(w + 1, std::memory_order_release); return true;
}
unsigned PlaybackBlockQueue::consume(float* left, float* right, unsigned frames) noexcept
{
    unsigned copied = 0;
    while (copied < frames)
    {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == written.load(std::memory_order_acquire)) break;
        const auto n = std::min(frames - copied, sizes[r % capacity] - offset);
        const auto* source = pcm.data() + (r % capacity) * maximum * 2 + offset * 2;
        for (unsigned i = 0; i < n; ++i) { left[copied + i] = source[i * 2]; right[copied + i] = source[i * 2 + 1]; }
        copied += n; offset += n;
        if (offset == sizes[r % capacity]) { offset = 0; read.store(r + 1, std::memory_order_release); }
    }
    std::fill(left + copied, left + frames, 0.0f); std::fill(right + copied, right + frames, 0.0f);
    if (copied != frames) missing.fetch_add(1, std::memory_order_relaxed);
    return copied;
}
void SilenceBlockProvider::render(float* out, unsigned frames, std::int64_t, unsigned)
{ std::fill(out, out + std::size_t(frames) * 2, 0.0f); }
void ToneBlockProvider::render(float* out, unsigned frames, std::int64_t first, unsigned Fs)
{
    if (!Fs) throw std::invalid_argument("Tone sample rate is zero");
    for (unsigned i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c)
            out[i * 2 + c] = float(.05 * std::sin(6.283185307179586 * (c ? 660 : 440) * double(first + i) / Fs));
}
struct PlaybackProviderPump::Impl
{
    PlaybackBlockQueue blocks;
    std::atomic<bool> stopping{false};
    std::thread worker;
    Impl(unsigned Fs, unsigned block, std::unique_ptr<IPlaybackBlockProvider> provider) : blocks(block, 16)
    {
        if (!provider || !Fs) throw std::invalid_argument("Missing playback provider/timebase");
        std::vector<float> initial(std::size_t(block) * 2);
        for (int i = 0; i < 8; ++i) { provider->render(initial.data(), block, std::int64_t(i) * block, Fs); blocks.tryPush(initial.data(), block); }
        worker = std::thread([this, Fs, block, provider = std::move(provider)]
        {
            std::vector<float> data(std::size_t(block) * 2); std::int64_t first = std::int64_t(block) * 8;
            while (!stopping.load())
            {
                provider->render(data.data(), block, first, Fs);
                if (blocks.tryPush(data.data(), block)) first += block;
                else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    ~Impl() { stopping = true; if (worker.joinable()) worker.join(); }
};
PlaybackProviderPump::PlaybackProviderPump(unsigned Fs, unsigned block, std::unique_ptr<IPlaybackBlockProvider> provider)
    : impl(std::make_unique<Impl>(Fs, block, std::move(provider))) {}
PlaybackProviderPump::~PlaybackProviderPump() = default;
PlaybackBlockQueue& PlaybackProviderPump::queue() { return impl->blocks; }
}
