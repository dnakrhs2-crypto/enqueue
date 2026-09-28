#pragma once

#include "audio/PluginChain.h"

#include <atomic>

namespace gocue::livemix
{

/** One sample clock for the graph. Only prepare (under the graph lock) and the graph callback touch the clock;
    the message thread can change the preference atomically. Each callback snapshots it for every chain. */
class HostTransport final : public PluginChain::TimingHook
{
public:
    void setSendTransport (bool enabled) noexcept { sendTransport.store (enabled, std::memory_order_relaxed); }
    bool getSendTransport() const noexcept { return sendTransport.load (std::memory_order_relaxed); }
    void prepare (double newSampleRate) noexcept;
    void beginBlock() noexcept;
    void setChunkOffset (int offset) noexcept { chunkStart = nextSample + offset; }
    void advance (int numSamples) noexcept { nextSample += numSamples; }

    juce::int64 getBlockStart() const noexcept override { return chunkStart; }
    juce::AudioPlayHead::PositionInfo getPositionAt (juce::int64 sample) const noexcept override;

private:
    std::atomic<bool> sendTransport { true };
    bool playing = true;
    double sampleRate = 0.0;
    juce::int64 nextSample = 0, chunkStart = 0;
};

} // namespace gocue::livemix
