#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>
#include <vector>

namespace r8b { class CDSPResampler; }

namespace gocue
{

/** Sample-rate conversion from the file's rate to the device's rate with r8brain (windowed-sinc, linear phase,
    flat to ~21 kHz, aliasing below -140 dB). The ratio is fixed for the life of the player; playback speed is
    a separate stage. Equal rates pass straight through. The device rate is set explicitly (setDeviceRate) —
    the AudioSource prepare call cannot be trusted for it (the next stage passes a scaled rate). */
class HighQualityResampler : public juce::AudioSource
{
public:
    HighQualityResampler (juce::AudioSource& upstream, int numChannels, double sourceRate);
    ~HighQualityResampler() override;

    /** Builds the converters for this output rate (message thread; allocates). */
    void setDeviceRate (double deviceRate);

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override;

    /** After a jump: forget the filter history (a few milliseconds of silence instead of the old place). Audio thread. */
    void reset() noexcept;

    bool isBypassed() const noexcept { return bypass; }

private:
    void pullChunk();

    juce::AudioSource& upstream;
    const int numChannels;
    const double sourceRate;
    double deviceRate = 0.0;
    bool bypass = true;
    int inChunk = 0;

    std::vector<std::unique_ptr<r8b::CDSPResampler>> converters;   // one per channel
    juce::AudioBuffer<float> inBuffer;
    std::vector<std::vector<double>> inDouble;
    std::vector<std::vector<double>> fifo;   // converted output not yet handed on, per channel
    int fifoRead = 0, fifoFilled = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HighQualityResampler)
};

} // namespace gocue
