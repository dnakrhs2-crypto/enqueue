#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstring>

namespace gocue::tests
{

/** A stand-in for a real VST3: stereo in/out, multiplies by 'gain', reports a tail,
    and round-trips its gain through get/setStateInformation. */
class TestGainPlugin : public juce::AudioPluginInstance
{
public:
    explicit TestGainPlugin (float initialGain, double tailSeconds = 0.0)
        : juce::AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                      .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          gain (initialGain), tail (tailSeconds)
    {
    }

    const juce::String getName() const override { return "TestGain"; }
    void prepareToPlay (double sr, int bs) override { preparedSampleRate = sr; preparedBlockSize = bs; ++prepareCount; }
    void releaseResources() override { ++releaseCount; }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        lastNumChannels = buffer.getNumChannels();
        lastNumSamples = buffer.getNumSamples();
        ++processCount;
        buffer.applyGain (gain);
    }

    double getTailLengthSeconds() const override { return tail; }
    void reset() override { ++resetCount; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        destData.setSize (sizeof (float));
        std::memcpy (destData.getData(), &gain, sizeof (float));
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (sizeInBytes == (int) sizeof (float))
            std::memcpy (&gain, data, sizeof (float));
    }

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "TestGain";
        d.pluginFormatName = "Test";
        d.fileOrIdentifier = "test://gain";
        d.manufacturerName = "GoCue tests";
        d.uniqueId = 1234;
        d.isInstrument = false;
    }

    float gain;
    double tail;
    double preparedSampleRate = 0.0;
    int preparedBlockSize = 0;
    int prepareCount = 0;
    int releaseCount = 0;
    int resetCount = 0;
    int processCount = 0;
    int lastNumChannels = 0;
    int lastNumSamples = 0;
};

} // namespace gocue::tests
