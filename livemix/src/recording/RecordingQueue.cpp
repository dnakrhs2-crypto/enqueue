#include "RecordingQueue.h"
#include <algorithm>
#include <cmath>

namespace gocue::livemix::recording
{
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
RecordingQueue::RecordingQueue(DeviceFormat f)
    : format(f), audio(static_cast<std::size_t>(std::ceil(f.sampleRate * queueSeconds))), blocks(audio.size()) {}
void RecordingQueue::reset() noexcept
{
    readFrames = 0; readBlocks = 0; writeBlocks = 0; writeFrames = 0; sealed = false;
}
bool RecordingQueue::push(collab::contracts::AudioSourceView a, collab::contracts::AudioSourceView b,
                          collab::contracts::AudioSourceView c, int n, std::uint64_t first) noexcept
{
    if (n <= 0 || n > format.maxBlockFrames || static_cast<std::uint64_t>(n) > audio.size()
        || writeFrames - readFrames.load(std::memory_order_acquire) + static_cast<unsigned>(n) > audio.size())
        return false;
    const collab::contracts::AudioSourceView views[] { a, b, c };
    unsigned occupied = 0;
    for (unsigned track = 0; track != 3; ++track)
    {
        const auto& v = views[track];
        if (v.left || v.right) occupied |= 1u << track;
        const auto* right = v.channels == 1 ? v.left : v.right;
        for (int i = 0; i < n; ++i)
        {
            auto& out = audio[(writeFrames + static_cast<unsigned>(i)) % audio.size()];
            out.samples[track * 2] = v.left ? v.left[i] : 0.0f;
            out.samples[track * 2 + 1] = right ? right[i] : 0.0f;
        }
    }
    const auto w = writeBlocks.load(std::memory_order_relaxed);
    blocks[w % blocks.size()] = { format, first, n, occupied };
    writeFrames += static_cast<unsigned>(n);
    writeBlocks.store(w + 1, std::memory_order_release);
    return true;
}
bool RecordingQueue::pop(Block& block, Frame* destination) noexcept
{
    const auto r = readBlocks.load(std::memory_order_relaxed);
    if (r == writeBlocks.load(std::memory_order_acquire)) return false;
    block = blocks[r % blocks.size()];
    const auto frame = readFrames.load(std::memory_order_relaxed);
    for (int i = 0; i < block.frames; ++i)
        destination[i] = audio[(frame + static_cast<unsigned>(i)) % audio.size()];
    // Descriptors are protected by the same frame ownership (each block has at least one frame).
    readBlocks.store(r + 1, std::memory_order_relaxed);
    readFrames.store(frame + static_cast<unsigned>(block.frames), std::memory_order_release);
    return true;
}
bool RecordingQueue::empty() const noexcept
{
    return readBlocks.load(std::memory_order_relaxed) == writeBlocks.load(std::memory_order_acquire);
}
}
