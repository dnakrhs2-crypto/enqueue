#pragma once
#include "audio/AsioTapAbi.h"
#include <juce_core/juce_core.h>
#include <memory>

namespace gocue::recorder
{
class IAudioOutputClient
{
public:
    virtual ~IAudioOutputClient() = default;
    // No allocation, I/O, locks, COM, waits or owning object releases here.
    virtual void processOutput(const BlockStamp&, float* left, float* right) noexcept = 0;
};
struct AudioOutputConfig
{
    std::uint32_t sampleRate = 48000;
    int deviceIndex = 0, bufferFrames = 0, leftPhysical = 0, rightPhysical = 1;
};
struct AudioOutputInfo
{
    juce::String driver;
    std::uint32_t sampleRate = 0, blockFrames = 0;
    int outputLatency = 0, leftPhysical = 0, rightPhysical = 1;
};
class IAudioOutput
{
public:
    virtual ~IAudioOutput() = default;
    virtual AudioOutputInfo open(const AudioOutputConfig&) = 0;
    virtual void start(IAudioOutputClient&) = 0;
    virtual void close() noexcept = 0; // callback barrier before clients/queues die
    virtual std::int64_t latestOutputSample() const noexcept = 0;
    virtual juce::Result status() const = 0;
    virtual void drainTiming() = 0; // non-RT owner
};
// Standalone, playback-only JUCE ASIO owner. Round 09 can implement IAudioOutput
// on its RecorderAudioEngine instead. Does not open inputs or synthesize a clock.
std::unique_ptr<IAudioOutput> makeAsioPlaybackOutput();
}
