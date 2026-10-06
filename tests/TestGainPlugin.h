#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstring>
#include <array>
#include <limits>
#include <stdexcept>

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
        ++liveInstances;
    }

    ~TestGainPlugin() override { --liveInstances; }

    const juce::String getName() const override { return "TestGain"; }
    void prepareToPlay (double sr, int bs) override
    {
        preparedSampleRate = sr;
        preparedBlockSize = bs;
        ++prepareCount;
        playHeadAtPrepare = getPlayHead() != nullptr;

        if (throwOnPrepare)
            throw std::runtime_error ("prepare failed");

        resetDelay();
        setLatencySamples (reportLatency ? latencySamples : 0);   // reported to the host, as a look-ahead limiter does
    }
    void releaseResources() override { ++releaseCount; }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        lastNumChannels = buffer.getNumChannels();
        lastNumSamples = buffer.getNumSamples();
        ++processCount;
        if (recordPositions && positionCount < (int) positions.size())
        {
            auto& seen = positions[(size_t) positionCount++];
            seen.hasPlayHead = getPlayHead() != nullptr;
            seen.position = seen.hasPlayHead ? getPlayHead()->getPosition() : juce::nullopt;
            seen.numSamples = buffer.getNumSamples();
        }
        const int n = buffer.getNumSamples();

        if (latencySamples > 0)
        {
            // a real delay of latencySamples (the ring is exactly that long): the output lags the input by the latency reported
            for (int ch = 0; ch < 2 && ch < buffer.getNumChannels(); ++ch)
            {
                float* d = buffer.getWritePointer (ch);
                float* ring = delayLine.getWritePointer (ch);
                int p = delayPos;

                for (int i = 0; i < n; ++i)
                {
                    const float in = d[i];
                    d[i] = ring[p];
                    ring[p] = in;

                    if (++p == latencySamples)
                        p = 0;
                }
            }

            delayPos = (delayPos + n) % latencySamples;
        }

        buffer.applyGain (gain);

        if (emitNaN && buffer.getNumSamples() > 0)
            buffer.setSample (0, 0, std::numeric_limits<float>::quiet_NaN());   // a broken plugin: poison in the output
    }

    /** A new latency while running (a look-ahead setting changed): the host hears of it through audioProcessorChanged. */
    void setLatencyLive (int newLatency)
    {
        latencySamples = juce::jmax (0, newLatency);
        resetDelay();
        setLatencySamples (reportLatency ? latencySamples : 0);
    }

    void resetDelay()
    {
        delayLine.setSize (2, juce::jmax (1, latencySamples));
        delayLine.clear();
        delayPos = 0;
    }

    double getTailLengthSeconds() const override { return tail; }
    void reset() override { ++resetCount; resetDelay(); }   // as a real plugin: its delay line starts empty
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
        ++stateReads;
        if (throwOnGetState)
            throw std::runtime_error ("state failed");
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
    struct PositionSeen
    {
        bool hasPlayHead = false;
        juce::Optional<juce::AudioPlayHead::PositionInfo> position;
        int numSamples = 0;
    };
    std::array<PositionSeen, 256> positions;
    bool recordPositions = false, playHeadAtPrepare = false, throwOnGetState = false;
    int positionCount = 0, stateReads = 0;
    double tail;
    double preparedSampleRate = 0.0;
    int preparedBlockSize = 0;
    int prepareCount = 0;
    int releaseCount = 0;
    int resetCount = 0;
    int processCount = 0;
    int lastNumChannels = 0;
    int lastNumSamples = 0;
    bool emitNaN = false;          // writes a NaN into the first sample of every block
    bool throwOnPrepare = false;   // prepareToPlay throws
    int latencySamples = 0;        // set before the plugin is added: reported at prepare, and the output really lags by as much
    bool reportLatency = true;     // false: the lag is not reported (an echo, 100 % wet: its repeat told as a tail instead)
    juce::AudioBuffer<float> delayLine;
    int delayPos = 0;
    static inline int liveInstances = 0;   // instances alive right now (a chain rebuild must destroy the old ones)
};

} // namespace gocue::tests
