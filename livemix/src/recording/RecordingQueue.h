#pragma once
#include "RecordingTypes.h"
#include <atomic>
#include <vector>

#if defined(_MSC_VER)
 #pragma warning(push)
 #pragma warning(disable: 4324) // Intentional cache-line separation of producer and consumer counters.
#endif
namespace gocue::livemix::recording
{
// Single audio producer, single disk consumer. No allocation or locks after construction.
class RecordingQueue
{
public:
    struct Frame { float samples[6] {}; }; // A L/R, B L/R, C L/R
    struct Block
    {
        DeviceFormat format;
        std::uint64_t firstFrame = 0;
        int frames = 0;
        unsigned occupied = 0;
    };
    explicit RecordingQueue(DeviceFormat);
    void reset() noexcept; // stopped producer and consumer only
    bool push(collab::contracts::AudioSourceView, collab::contracts::AudioSourceView,
              collab::contracts::AudioSourceView, int, std::uint64_t) noexcept;
    bool pop(Block&, Frame* destination) noexcept; // destination has maxBlockFrames elements
    bool empty() const noexcept;
    std::size_t capacity() const noexcept { return audio.size(); }
    const DeviceFormat format;
    std::atomic<bool> sealed { false };
private:
    std::vector<Frame> audio;
    // A descriptor per frame also supports variable blocks down to one frame without reducing capacity.
    std::vector<Block> blocks;
    alignas(64) std::atomic<std::uint64_t> readFrames { 0 }, readBlocks { 0 };
    alignas(64) std::atomic<std::uint64_t> writeBlocks { 0 };
    std::uint64_t writeFrames = 0;
};
}
#if defined(_MSC_VER)
 #pragma warning(pop)
#endif
