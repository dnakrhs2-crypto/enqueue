#pragma once
#include "AsioTapAbi.h"
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace gocue::recorder
{
class RawAudioTap
{
public:
    static constexpr std::size_t maxChannels = 8, maxStrideBytes = 8;
    struct Block
    {
        BlockStamp stamp{};
        std::uint32_t numChannels = 0;
        std::array<NativeInputView, maxChannels> channels{};
    };
    // Stopped driver + stopped consumer only. Ascending, unique physical indices
    // match JUCE's compact active array. Slots own bytes; borrowed driver pointers
    // are never stored in a committed slot.
    void prepare(const std::vector<int>& activeToPhysical, std::uint32_t maxBlockSamples, std::size_t queueBlocks);
    bool onAsioBlock(const BlockStamp&, const NativeInputView*, std::uint32_t channels) noexcept;
    const Block* front() const noexcept; // sole worker; valid until release()
    void release() noexcept;
    std::size_t capacity() const noexcept { return slots.size(); }
    std::size_t allocatedBytes() const noexcept { return byteCount; }
    std::size_t highWater() const noexcept { return peak.load(std::memory_order_relaxed); }
    std::uint64_t overflows() const noexcept { return full.load(std::memory_order_relaxed); }
    std::uint64_t invalidBlocks() const noexcept { return invalid.load(std::memory_order_relaxed); }
    const std::vector<int>& activeIndexToPhysicalIndex() const noexcept { return mapping; }
private:
    std::vector<int> mapping;
    std::vector<Block> slots;
    std::unique_ptr<std::uint8_t[]> bytes;
    std::uint32_t maximumSamples = 0;
    std::size_t bytesPerChannel = 0, byteCount = 0;
    alignas(64) std::atomic<std::uint64_t> written{0};
    alignas(64) std::atomic<std::uint64_t> read{0};
    std::atomic<std::uint64_t> full{0}, invalid{0};
    std::atomic<std::size_t> peak{0};
};
}
