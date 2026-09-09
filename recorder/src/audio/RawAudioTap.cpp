#include "RawAudioTap.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gocue::recorder
{
void RawAudioTap::prepare(const std::vector<int>& channels, std::uint32_t maxBlock, std::size_t blocks)
{
    if (channels.size() > maxChannels || !maxBlock || maxBlock > 262144 || !blocks || blocks > 65536
        || !std::is_sorted(channels.begin(), channels.end()) || (!channels.empty() && channels.front() < 0)
        || std::adjacent_find(channels.begin(), channels.end()) != channels.end())
        throw std::invalid_argument("RawAudioTap: invalid channel map, block size or queue capacity");
    const auto total = static_cast<std::uint64_t>(maxBlock) * maxStrideBytes * channels.size() * blocks;
    if (total > 512ull * 1024 * 1024) throw std::invalid_argument("RawAudioTap: queue exceeds 512 MiB safety bound");
    auto storage = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(total)); // allocate and touch before callbacks
    std::vector<Block> prepared(blocks);
    mapping = channels; slots = std::move(prepared); bytes = std::move(storage);
    maximumSamples = maxBlock; bytesPerChannel = static_cast<std::size_t>(maxBlock) * maxStrideBytes;
    byteCount = static_cast<std::size_t>(total);
    written = 0; read = 0; full = 0; invalid = 0; peak = 0;
}
bool RawAudioTap::onAsioBlock(const BlockStamp& stamp, const NativeInputView* inputs, std::uint32_t count) noexcept
{
    if (slots.empty() || stamp.numSamples == 0 || stamp.numSamples > maximumSamples || count != mapping.size() || (count && !inputs))
    { invalid.fetch_add(1, std::memory_order_relaxed); return false; }
    for (std::uint32_t c = 0; c < count; ++c)
        if (!inputs[c].data || inputs[c].activeIndex != static_cast<int>(c) || inputs[c].physicalIndex != mapping[c]
            || inputs[c].format.strideBytes == 0 || inputs[c].format.strideBytes > maxStrideBytes
            || inputs[c].format.containerBytes == 0 || inputs[c].format.containerBytes > inputs[c].format.strideBytes)
        { invalid.fetch_add(1, std::memory_order_relaxed); return false; }
    const auto w = written.load(std::memory_order_relaxed), r = read.load(std::memory_order_acquire);
    if (w - r == slots.size()) { full.fetch_add(1, std::memory_order_relaxed); return false; }
    auto& block = slots[w % slots.size()];
    block.stamp = stamp; block.numChannels = count;
    for (std::uint32_t c = 0; c < count; ++c)
    {
        auto* destination = bytes.get() + ((w % slots.size()) * mapping.size() + c) * bytesPerChannel;
        const auto copyBytes = static_cast<std::size_t>(stamp.numSamples - 1) * inputs[c].format.strideBytes + inputs[c].format.containerBytes;
        std::memcpy(destination, inputs[c].data, copyBytes);
        block.channels[c] = inputs[c]; block.channels[c].data = destination;
    }
    const auto size = static_cast<std::size_t>(w - r + 1);
    if (size > peak.load(std::memory_order_relaxed)) peak.store(size, std::memory_order_relaxed);
    written.store(w + 1, std::memory_order_release);
    return true;
}
const RawAudioTap::Block* RawAudioTap::front() const noexcept
{
    const auto r = read.load(std::memory_order_relaxed);
    return r == written.load(std::memory_order_acquire) ? nullptr : &slots[r % slots.size()];
}
void RawAudioTap::release() noexcept { read.fetch_add(1, std::memory_order_release); }
}
