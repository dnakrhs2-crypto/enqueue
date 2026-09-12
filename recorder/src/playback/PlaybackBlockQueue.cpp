#include "PlaybackBlockQueue.h"
#include "audio/MicroFade.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace gocue::recorder
{
namespace
{
unsigned queueBlocks(unsigned rate, unsigned frames)
{
    if (!rate || rate > 768000 || !frames || frames > 262144) throw std::invalid_argument("Invalid playback dimensions");
    return static_cast<unsigned>((std::max)(3ull, (rate + 2ull * frames - 1) / (2ull * frames) + 1));
}
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<void*>::is_always_lock_free
              && std::atomic<int>::is_always_lock_free, "Playback requires lock-free atomics");
}
struct PlaybackBlockQueue::Impl
{
    struct Block { std::int64_t first = 0; std::uint64_t generation = 0; unsigned frames = 0; };
    struct Bank
    {
        std::vector<float> pcm;
        std::vector<Block> blocks;
        std::vector<PlaybackGenerationGuard> guards; // released only on producer reuse/destruction
        alignas(64) std::atomic<std::uint64_t> written{0}, read{0}, queued{0};
        std::atomic<bool> occupied{false}, prepared{false}, failed{false};
        // Immutable after publication, except producerEnd (worker) and offset (RT).
        std::int64_t first = 0, end = -1, revision = 0, producerEnd = 0;
        std::uint64_t generation = 0, ticket = 0, minimum = 0;
        unsigned ramp = 0, offset = 0;
        void clear() noexcept
        { written = 0; read = 0; queued = 0; prepared = false; failed = false; offset = 0; guards.clear(); }
        bool current() const noexcept
        {
            for (const auto& guard : guards) if (guard.epoch->load(std::memory_order_acquire) != guard.expected) return false;
            return true;
        }
    };
    const unsigned maximum, capacity;
    std::array<Bank, 3> banks;
    std::atomic<Bank*> active{nullptr}, pending{nullptr};
    Bank* producer = nullptr; // non-RT producer owner
    std::atomic<std::uint64_t> ticket{0}, missing{0};
    std::atomic<std::int64_t> revision{0}, cursor{0};
    std::atomic<PlaybackQueueState> state{PlaybackQueueState::idle};
    Impl(unsigned frames, unsigned count) : maximum(frames), capacity(count)
    {
        if (!frames || frames > 262144 || !count || count > 262144 || std::uint64_t(frames) * count * 2 > 32 * 1024 * 1024)
            throw std::invalid_argument("Playback queue capacity out of range");
        for (auto& bank : banks) { bank.pcm.resize(std::size_t(frames) * count * 2, 0); bank.blocks.resize(count); }
        reset();
    }
    void reset() noexcept
    {
        pending = nullptr;
        for (auto& b : banks) { b.clear(); b.occupied = false; b.end = -1; b.first = b.producerEnd = 0; b.generation = 0; b.ramp = 0; b.ticket = 0; }
        producer = &banks[0]; producer->occupied = true; active = producer;
        revision = 0; cursor = 0; state = PlaybackQueueState::idle;
    }
    bool push(Bank& b, std::int64_t first, std::uint64_t gen, const float* l, const float* r, const float* interleaved, unsigned frames) noexcept
    {
        if (!frames || frames > maximum || (!interleaved && (!l || !r)) || first < 0
            || first > (std::numeric_limits<std::int64_t>::max)() - frames || b.failed.load(std::memory_order_acquire)) return false;
        if (b.end >= 0 && (b.ticket != ticket.load(std::memory_order_acquire) || first != b.producerEnd
            || gen != b.generation || first > b.end || frames > static_cast<std::uint64_t>(b.end - first))) return false;
        const auto w = b.written.load(std::memory_order_relaxed);
        if (w - b.read.load(std::memory_order_acquire) >= capacity) return false;
        const auto slot = static_cast<std::size_t>(w % capacity);
        auto* data = b.pcm.data() + slot * maximum * 2;
        if (interleaved) for (unsigned i = 0; i < frames; ++i) { data[i] = interleaved[i * 2]; data[maximum + i] = interleaved[i * 2 + 1]; }
        else { std::memcpy(data, l, frames * sizeof(float)); std::memcpy(data + maximum, r, frames * sizeof(float)); }
        b.blocks[slot] = {first, gen, frames}; b.producerEnd = first + frames;
        b.queued.fetch_add(frames, std::memory_order_relaxed);
        b.written.store(w + 1, std::memory_order_release);
        if (b.end >= 0 && b.queued.load(std::memory_order_acquire) >= b.minimum)
        {
            b.prepared.store(true, std::memory_order_release);
            auto filling = PlaybackQueueState::prefilling;
            state.compare_exchange_strong(filling, PlaybackQueueState::ready);
        }
        return true;
    }
    void restore(Bank* b) noexcept
    {
        if (!b) return;
        if (b->ticket != ticket.load(std::memory_order_acquire)) { b->occupied.store(false, std::memory_order_release); return; }
        Bank* empty = nullptr;
        if (!pending.compare_exchange_strong(empty, b, std::memory_order_release, std::memory_order_relaxed)) b->occupied.store(false, std::memory_order_release);
    }
    bool miss(float* l, float* r, unsigned frames, Bank* next) noexcept
    {
        restore(next); std::fill_n(l, frames, 0.0f); std::fill_n(r, frames, 0.0f);
        missing.fetch_add(1, std::memory_order_relaxed);
        if (state.load() != PlaybackQueueState::failed) state.store(PlaybackQueueState::buffering);
        return false;
    }
    bool consume(std::int64_t first, std::uint64_t gen, float* l, float* r, unsigned frames, bool automatic = false) noexcept
    {
        if (!frames) return true;
        auto* current = active.load(std::memory_order_acquire);
        // Exclusive descriptor ownership prevents producer reclamation during reads.
        auto* next = pending.exchange(nullptr, std::memory_order_acq_rel);
        if (first < 0 || first > (std::numeric_limits<std::int64_t>::max)() - frames) return miss(l, r, frames, next);
        if (next && next->ticket != ticket.load(std::memory_order_acquire))
        { next->occupied.store(false, std::memory_order_release); return miss(l, r, frames, nullptr); }
        if (next && first + frames > next->first)
        {
            if (automatic) gen = next->generation;
            if (!next->current()) { next->failed.store(true); state = PlaybackQueueState::failed; }
            if (first != next->first || gen != next->generation || !next->prepared.load(std::memory_order_acquire)
                || next->failed.load(std::memory_order_acquire)) return miss(l, r, frames, next);
            active.store(next, std::memory_order_release); current->occupied.store(false, std::memory_order_release);
            current = next; next = nullptr; revision.store(current->revision, std::memory_order_release);
        }
        if (!current->current()) { current->failed.store(true); state = PlaybackQueueState::failed; }
        if (current->failed.load(std::memory_order_acquire) || (current->end >= 0 && !current->prepared.load(std::memory_order_acquire)))
            return miss(l, r, frames, next);
        if (automatic)
        {
            frames = static_cast<unsigned>((std::min)(std::uint64_t(frames), static_cast<std::uint64_t>((std::max)(std::int64_t{0}, current->end - first))));
            if (!frames) { restore(next); state = PlaybackQueueState::ended; return true; }
        }
        const auto w = current->written.load(std::memory_order_acquire);
        auto at = current->read.load(std::memory_order_relaxed);
        auto offset = current->offset; unsigned remaining = frames; auto expected = first;
        while (remaining && at < w)
        {
            const auto& b = current->blocks[at % capacity];
            if (offset >= b.frames || b.generation != gen || b.first + offset != expected) return miss(l, r, frames, next);
            const auto n = (std::min)(remaining, b.frames - offset);
            remaining -= n; expected += n; ++at; offset = 0;
        }
        if (remaining) return miss(l, r, frames, next);
        at = current->read.load(std::memory_order_relaxed); remaining = frames; unsigned copied = 0;
        while (remaining)
        {
            const auto slot = static_cast<std::size_t>(at % capacity);
            const auto n = (std::min)(remaining, current->blocks[slot].frames - current->offset);
            const auto* data = current->pcm.data() + slot * maximum * 2 + current->offset;
            std::memcpy(l + copied, data, n * sizeof(float)); std::memcpy(r + copied, data + maximum, n * sizeof(float));
            copied += n; remaining -= n; current->offset += n; current->queued.fetch_sub(n, std::memory_order_relaxed);
            if (current->offset == current->blocks[slot].frames)
            { current->offset = 0; current->read.store(++at, std::memory_order_release); }
        }
        for (unsigned i = 0; i < frames; ++i)
        {
            const auto sample = first + i;
            float gain = MicroFade::in(sample - current->first, current->ramp);
            if (next && sample < next->first && next->first - sample <= next->ramp)
                gain *= MicroFade::out(sample - (next->first - next->ramp), next->ramp);
            l[i] *= gain; r[i] *= gain;
        }
        restore(next); cursor.store(first + frames, std::memory_order_release);
        state.store(PlaybackQueueState::playing); return true;
    }
};
PlaybackBlockQueue::PlaybackBlockQueue(unsigned frames, unsigned blocks) : impl(std::make_unique<Impl>(frames, blocks)) {}
PlaybackBlockQueue::~PlaybackBlockQueue() = default;
void PlaybackBlockQueue::reset() { cancelProducer(); impl->reset(); }
bool PlaybackBlockQueue::tryPush(const float* data, unsigned frames) noexcept
{ auto& b = *impl->producer; return data && impl->push(b, b.producerEnd, b.generation, nullptr, nullptr, data, frames); }
bool PlaybackBlockQueue::push(std::int64_t first, std::uint64_t gen, const float* l, const float* r, unsigned frames) noexcept
{ return impl->push(*impl->producer, first, gen, l, r, nullptr, frames); }
bool PlaybackBlockQueue::consume(std::int64_t first, std::uint64_t gen, float* l, float* r, unsigned frames) noexcept
{ return impl->consume(first, gen, l, r, frames); }
unsigned PlaybackBlockQueue::consume(float* l, float* r, unsigned frames) noexcept
{
    const auto* bank = impl->active.load(std::memory_order_acquire);
    if (bank->end >= 0)
    {
        const auto first = impl->cursor.load(std::memory_order_relaxed);
        std::fill_n(l, frames, 0.0f); std::fill_n(r, frames, 0.0f);
        return impl->consume(first, bank->generation, l, r, frames, true)
            ? static_cast<unsigned>(impl->cursor.load(std::memory_order_relaxed) - first) : 0;
    }
    // Legacy untimed providers permit a partial tail (reference mix consumer).
    const auto rd = bank->read.load(std::memory_order_relaxed);
    const auto w = bank->written.load(std::memory_order_acquire);
    const auto count = w == rd ? 0u : static_cast<unsigned>((std::min)(std::uint64_t(frames), queuedFrames()));
    const auto first = count ? bank->blocks[rd % impl->capacity].first + bank->offset : 0;
    const auto gen = count ? bank->blocks[rd % impl->capacity].generation : 0;
    if (count && !consume(first, gen, l, r, count)) { std::fill_n(l, frames, 0.0f); std::fill_n(r, frames, 0.0f); return 0; }
    std::fill(l + count, l + frames, 0.0f); std::fill(r + count, r + frames, 0.0f);
    if (count != frames) { impl->missing.fetch_add(1); impl->state = PlaybackQueueState::buffering; }
    return count;
}
std::uint64_t PlaybackBlockQueue::queuedFrames() const noexcept { return impl->active.load()->queued.load(std::memory_order_acquire); }
std::uint64_t PlaybackBlockQueue::underruns() const noexcept { return impl->missing.load(); }
unsigned PlaybackBlockQueue::maxBlockFrames() const noexcept { return impl->maximum; }
PlaybackQueueState PlaybackBlockQueue::state() const noexcept { return impl->state.load(); }
std::int64_t PlaybackBlockQueue::activeRevision() const noexcept { return impl->revision.load(); }
std::int64_t PlaybackBlockQueue::submittedEnd() const noexcept { return impl->cursor.load(); }
void PlaybackBlockQueue::cancelProducer() noexcept { impl->ticket.fetch_add(1, std::memory_order_acq_rel); }
std::uint64_t PlaybackBlockQueue::begin(std::int64_t first, std::uint64_t gen, std::int64_t revision, std::int64_t end,
                                      std::uint64_t minimum, unsigned ramp, bool replacement, std::vector<PlaybackGenerationGuard> guards)
{
    if (first < 0 || end < first || !gen || revision < 0 || minimum > std::uint64_t(impl->maximum) * impl->capacity)
        throw std::invalid_argument("Invalid playback preparation range/depth");
    for (const auto& guard : guards) if (!guard.epoch) throw std::invalid_argument("Missing playback media epoch");
    if (!replacement) reset();
    auto* retired = impl->pending.exchange(nullptr, std::memory_order_acq_rel);
    if (retired) retired->occupied.store(false, std::memory_order_release);
    auto* bank = replacement ? nullptr : impl->producer;
    if (replacement) for (auto& candidate : impl->banks)
    {
        bool free = false;
        if (candidate.occupied.compare_exchange_strong(free, true)) { bank = &candidate; break; }
    }
    if (!bank) throw std::runtime_error("Playback revision bank unavailable");
    bank->clear(); bank->first = bank->producerEnd = first; bank->end = end; bank->generation = gen; bank->revision = revision;
    bank->guards = std::move(guards);
    bank->minimum = (std::min)(minimum, static_cast<std::uint64_t>(end - first)); bank->ramp = ramp;
    bank->ticket = impl->ticket.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (!bank->minimum) bank->prepared.store(true);
    impl->producer = bank;
    impl->state = bank->minimum ? PlaybackQueueState::prefilling : PlaybackQueueState::ready;
    if (replacement)
    {
        // A callback may have restored a descriptor while this producer prepared
        // the new bank. Reclaim that descriptor instead of losing its ownership.
        auto* replaced = impl->pending.exchange(bank, std::memory_order_acq_rel);
        if (replaced) replaced->occupied.store(false, std::memory_order_release);
    }
    else { impl->revision = revision; impl->cursor = first; }
    return bank->ticket;
}
bool PlaybackBlockQueue::pushPrepared(std::uint64_t ticket, std::int64_t first, const float* l, const float* r, unsigned frames) noexcept
{
    return ticket == impl->ticket.load(std::memory_order_acquire)
        && impl->push(*impl->producer, first, impl->producer->generation, l, r, nullptr, frames);
}
void PlaybackBlockQueue::fail(std::uint64_t ticket) noexcept
{
    if (ticket == impl->ticket.load(std::memory_order_acquire))
    { impl->producer->failed.store(true, std::memory_order_release); impl->state = PlaybackQueueState::failed; }
}
bool PlaybackBlockQueue::ready() const noexcept
{ return impl->producer->prepared.load(std::memory_order_acquire) && !impl->producer->failed.load(std::memory_order_acquire) && impl->producer->current(); }
std::uint64_t PlaybackBlockQueue::prefetchedFrames() const noexcept { return impl->producer->queued.load(std::memory_order_acquire); }
PlaybackPcmQueue::PlaybackPcmQueue(std::uint32_t Fs, std::uint32_t frames)
    : PlaybackBlockQueue(frames, queueBlocks(Fs, frames)), blockFrames(frames), capacityBlocks(queueBlocks(Fs, frames)) {}
void SilenceBlockProvider::render(float* out, unsigned frames, std::int64_t, unsigned)
{ std::fill_n(out, std::size_t(frames) * 2, 0.0f); }
void ToneBlockProvider::render(float* out, unsigned frames, std::int64_t first, unsigned Fs)
{
    if (!Fs) throw std::invalid_argument("Tone sample rate is zero");
    for (unsigned i = 0; i < frames; ++i) for (int c = 0; c < 2; ++c)
        out[i * 2 + c] = float(.05 * std::sin(6.283185307179586 * (c ? 660 : 440) * double(first + i) / Fs));
}
struct PlaybackProviderPump::Impl
{
    PlaybackBlockQueue blocks;
    std::atomic<bool> stopping{false};
    std::thread worker;
    Impl(unsigned Fs, unsigned block, std::unique_ptr<IPlaybackBlockProvider> provider) : blocks(block, queueBlocks(Fs, block))
    {
        if (!provider) throw std::invalid_argument("Missing playback provider");
        const auto ticket = blocks.begin(0, 1, 0, (std::numeric_limits<std::int64_t>::max)(), (Fs + 3ull) / 4);
        std::vector<float> initial(std::size_t(block) * 2);
        std::int64_t first = 0;
        while (!blocks.ready())
        { provider->render(initial.data(), block, first, Fs); if (!blocks.tryPush(initial.data(), block)) throw std::runtime_error("Initial playback prefill failed"); first += block; }
        worker = std::thread([this, Fs, block, first, ticket, provider = std::move(provider)]() mutable
        {
            try
            {
                std::vector<float> data(std::size_t(block) * 2);
                while (!stopping.load(std::memory_order_acquire))
                {
                    provider->render(data.data(), block, first, Fs);
                    while (!stopping.load(std::memory_order_acquire) && !blocks.tryPush(data.data(), block))
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    first += block;
                }
            }
            catch (...) { blocks.fail(ticket); }
        });
    }
    ~Impl() { stopping = true; blocks.cancelProducer(); if (worker.joinable()) worker.join(); }
};
PlaybackProviderPump::PlaybackProviderPump(unsigned Fs, unsigned block, std::unique_ptr<IPlaybackBlockProvider> provider)
    : impl(std::make_unique<Impl>(Fs, block, std::move(provider))) {}
PlaybackProviderPump::~PlaybackProviderPump() = default;
PlaybackBlockQueue& PlaybackProviderPump::queue() { return impl->blocks; }
}
