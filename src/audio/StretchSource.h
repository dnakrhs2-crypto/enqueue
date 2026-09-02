#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>
#include <vector>

namespace gocue
{

/** Time-stretch stage (Signalsmith Stretch): plays the upstream source at 'rate' while keeping the pitch.
    Sits between the disk read-ahead and the device-rate resampler, so it works in file samples: for every
    output block it pulls rate x that many input samples. Position changes (start, seek, live trim) re-seek the
    stretcher with a pre-roll so the first output lines up with the requested sample. Live rate changes are
    smooth (the ratio is inferred per block). Not audio-thread-safe to construct; process() runs on the
    audio thread without allocating once prepared. */
class StretchSource : public juce::PositionableAudioSource
{
public:
    /** Rates outside this range fall back to the edges (time-stretching further sounds bad anyway). */
    static constexpr double minRate = 0.25;
    static constexpr double maxRate = 4.0;

    /** @param upstream  the source to stretch (not owned). */
    StretchSource (juce::PositionableAudioSource& upstream, int numChannels, double fileSampleRate);
    ~StretchSource() override;

    /** Any thread. Applied from the next block. */
    void setRate (double newRate) noexcept { rate.store (juce::jlimit (minRate, maxRate, newRate), std::memory_order_relaxed); }
    double getRate() const noexcept { return rate.load (std::memory_order_relaxed); }

    /** Samples the upstream must be able to deliver before a position for the pre-roll (seek needs). */
    int getPreRollSamples() const noexcept;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override;

    /** Moves the upstream and re-seeks the stretcher so the next output starts at 'newPosition'. */
    void setNextReadPosition (juce::int64 newPosition) override;
    juce::int64 getNextReadPosition() const override { return position.load (std::memory_order_relaxed); }
    juce::int64 getTotalLength() const override { return upstream.getTotalLength(); }
    bool isLooping() const override { return false; }

private:
    struct Impl;
    void seekTo (juce::int64 newPosition);
    void pull (int numInputSamples);

    juce::PositionableAudioSource& upstream;
    std::unique_ptr<Impl> impl;
    const int channels;
    const double fileSampleRate;
    std::atomic<double> rate { 1.0 };
    std::atomic<juce::int64> position { 0 };       // next output sample (in upstream / file terms)
    std::atomic<juce::int64> pendingSeek { -1 };
    double inputCarry = 0.0;                        // fractional input samples owed
    int maxBlock = 512;
    juce::AudioBuffer<float> inputBuffer;           // channels x (maxBlock * maxRate + slack)
    juce::AudioBuffer<float> spareOutput;           // channels the caller's buffer does not have land here
    std::vector<float*> inputPointers, outputPointers;
    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StretchSource)
};

} // namespace gocue
