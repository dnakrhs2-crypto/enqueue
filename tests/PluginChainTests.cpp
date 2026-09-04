#include "audio/AudioEngine.h"
#include "audio/PluginChain.h"
#include "TestGainPlugin.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

class PluginChainTests : public juce::UnitTest
{
public:
    PluginChainTests() : juce::UnitTest ("PluginChain", "Enqueue") {}

    struct RecordingListener : public PluginChain::Listener
    {
        juce::AudioPluginInstance* lastRemoved = nullptr;
        int removedCount = 0, changedCount = 0;

        void pluginAboutToBeRemoved (PluginChain&, juce::AudioPluginInstance& p) override { lastRemoved = &p; ++removedCount; }
        void chainChanged (PluginChain&) override { ++changedCount; }
    };

    static void fill (juce::AudioBuffer<float>& buffer, float value)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), value, buffer.getNumSamples());
    }

    static PluginChain::Factory testFactory (bool succeed)
    {
        return [succeed] (const PluginSlotState& state, juce::String& error) -> std::unique_ptr<juce::AudioPluginInstance>
        {
            if (! succeed || state.fileOrIdentifier != "test://gain")
            {
                error = "not installed";
                return nullptr;
            }

            return std::make_unique<TestGainPlugin> (1.0f);
        };
    }

    juce::File writeSine (const juce::File& dir, double sampleRate, double seconds, float amplitude)
    {
        const auto file = dir.getChildFile ("tone.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (sampleRate)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (16));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int numSamples = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));

        writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
        return file;
    }

    void runTest() override
    {
        juce::AudioBuffer<float> buffer (2, 512);

        beginTest ("a plugin that throws while preparing is faulted, not run; a throwing release does not escape");
        {
            PluginChain chain;
            chain.prepare (48000.0, 64);
            auto* bad = new TestGainPlugin (2.0f);
            bad->throwOnPrepare = true;
            chain.addPlugin (std::unique_ptr<juce::AudioPluginInstance> (bad));
            juce::AudioBuffer<float> dry (2, 64);
            dry.clear();
            dry.setSample (0, 0, 0.5f);
            chain.process (dry, 64);
            expectWithinAbsoluteError (dry.getSample (0, 0), 0.5f, 1e-6f);   // dry: never run
            expectEquals (chain.takeNewFaults().joinIntoString (","), juce::String ("TestGain"));
            chain.clear();
        }

        beginTest ("plugins are prepared, processed in order and can be bypassed");
        {
            PluginChain chain;
            RecordingListener listener;
            chain.setListener (&listener);
            chain.prepare (48000.0, 256);

            auto* first = new TestGainPlugin (0.5f);
            auto* second = new TestGainPlugin (0.5f);
            chain.addPlugin (std::unique_ptr<juce::AudioPluginInstance> (first));
            chain.addPlugin (std::unique_ptr<juce::AudioPluginInstance> (second));

            expectEquals (chain.getNumSlots(), 2);
            expectEquals (listener.changedCount, 2);
            expectEquals (first->prepareCount, 1);
            expectWithinAbsoluteError (first->preparedSampleRate, 48000.0, 1e-9);
            expectEquals (first->preparedBlockSize, 256);
            expectEquals (first->getTotalNumInputChannels(), 2);
            expectEquals (first->getTotalNumOutputChannels(), 2);

            fill (buffer, 1.0f);
            chain.process (buffer, 512);
            expectWithinAbsoluteError (buffer.getSample (0, 100), 0.25f, 1e-6f);
            expectWithinAbsoluteError (buffer.getSample (1, 511), 0.25f, 1e-6f);
            expectEquals (first->lastNumChannels, 2);
            expectEquals (first->lastNumSamples, 256);   // a 512 block arrives in two pieces: a plugin never sees more than it was prepared for

            chain.setBypassed (1, true);
            const int bypassedCallsBefore = second->processCount;
            fill (buffer, 1.0f);
            chain.process (buffer, 512);
            expectWithinAbsoluteError (buffer.getSample (0, 10), 0.5f, 1e-6f);   // output of the bypassed plugin is discarded
            expect (chain.getSlot (1).bypassed.load());
            expectEquals (second->processCount, bypassedCallsBefore + 2);        // ...but it keeps running (time advances): two pieces of 256

            beginTest ("a suspended plugin is skipped with a dry pass");
            second->suspendProcessing (true);
            chain.setBypassed (1, false);
            const int suspendedCallsBefore = second->processCount;
            fill (buffer, 1.0f);
            chain.process (buffer, 512);
            expectWithinAbsoluteError (buffer.getSample (0, 10), 0.5f, 1e-6f);
            expectEquals (second->processCount, suspendedCallsBefore);
            second->suspendProcessing (false);
            fill (buffer, 1.0f);
            chain.process (buffer, 512);
            expectWithinAbsoluteError (buffer.getSample (0, 10), 0.25f, 1e-6f);
            chain.setBypassed (1, true);

            beginTest ("plugin parameter / state changes are flagged for dirty tracking");
            expect (! chain.consumeStateChanged());
            first->updateHostDisplay();
            expect (chain.consumeStateChanged());
            expect (! chain.consumeStateChanged());

            // partial blocks only touch the requested samples
            fill (buffer, 1.0f);
            chain.process (buffer, 100);
            expectWithinAbsoluteError (buffer.getSample (0, 99), 0.5f, 1e-6f);
            expectWithinAbsoluteError (buffer.getSample (0, 100), 1.0f, 1e-6f);

            chain.prepare (44100.0, 512);
            expectEquals (first->prepareCount, 2);
            expectEquals (second->preparedBlockSize, 512);

            beginTest ("removing / clearing tells the listener before destroying the instance");
            chain.removePlugin (0);
            expect (listener.lastRemoved == first);
            expectEquals (listener.removedCount, 1);
            expectEquals (chain.getNumSlots(), 1);

            chain.clear();
            expect (listener.lastRemoved == second);
            expectEquals (listener.removedCount, 2);
            expectEquals (chain.getNumSlots(), 0);
        }

        beginTest ("movePlugin reorders the chain");
        {
            PluginChain chain;
            chain.prepare (44100.0, 512);
            chain.addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            chain.addPlugin (std::make_unique<TestGainPlugin> (2.0f));
            expect (chain.movePlugin (1, 0));
            expectWithinAbsoluteError (static_cast<TestGainPlugin*> (chain.getSlot (0).plugin.get())->gain, 2.0f, 1e-6f);
            expect (! chain.movePlugin (0, 5));
            expect (! chain.movePlugin (0, 0));
        }

        beginTest ("states capture description + plugin state and restore through a factory");
        {
            PluginChain chain;
            chain.prepare (44100.0, 512);
            chain.addPlugin (std::make_unique<TestGainPlugin> (0.25f));
            chain.addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            chain.setBypassed (1, true);

            const auto states = chain.getStates();
            expectEquals ((int) states.size(), 2);
            expectEquals (states[0].name, juce::String ("TestGain"));
            expectEquals (states[0].format, juce::String ("Test"));
            expectEquals (states[0].fileOrIdentifier, juce::String ("test://gain"));
            expectEquals (states[0].uniqueId, 1234);
            expect (states[0].stateBase64.isNotEmpty());
            expect (states[0].descriptionXml.contains ("TestGain"));
            expect (! states[0].bypassed);
            expect (states[1].bypassed);

            PluginChain restored;
            restored.prepare (44100.0, 512);
            const auto errors = restored.restore (states, testFactory (true));
            expectEquals (errors.size(), 0);
            expectEquals (restored.getNumSlots(), 2);
            expect (restored.getSlot (1).bypassed.load());

            fill (buffer, 1.0f);
            restored.process (buffer, 512);
            expectWithinAbsoluteError (buffer.getSample (0, 5), 0.25f, 1e-6f);   // gain came back through setStateInformation, slot 1 bypassed

            beginTest ("unavailable plugins are kept as missing slots and survive a re-save");
            PluginChain missing;
            const auto missingErrors = missing.restore (states, testFactory (false));
            expectEquals (missingErrors.size(), 2);
            expectEquals (missing.getNumSlots(), 2);
            expect (missing.getSlot (0).isMissing());

            const auto resaved = missing.getStates();
            expectEquals (resaved[0].fileOrIdentifier, juce::String ("test://gain"));
            expectEquals (resaved[0].stateBase64, states[0].stateBase64);
            expect (resaved[1].bypassed);

            fill (buffer, 1.0f);
            missing.process (buffer, 512);
            expectWithinAbsoluteError (buffer.getSample (0, 5), 1.0f, 1e-6f);   // missing slots pass audio through
        }

        beginTest ("tail is the sum of the active tails in series, capped at maxTailSeconds");
        {
            PluginChain chain;
            chain.prepare (44100.0, 512);
            expectWithinAbsoluteError (chain.getTailSeconds(), 0.0, 1e-12);
            chain.addPlugin (std::make_unique<TestGainPlugin> (1.0f, 0.5));
            chain.addPlugin (std::make_unique<TestGainPlugin> (1.0f, 0.25));
            expectWithinAbsoluteError (chain.getTailSeconds(), 0.75, 1e-12);     // reverb into delay rings for both
            chain.addPlugin (std::make_unique<TestGainPlugin> (1.0f, 99.0));
            expectWithinAbsoluteError (chain.getTailSeconds(), PluginChain::maxTailSeconds, 1e-12);
            chain.setBypassed (2, true);
            expectWithinAbsoluteError (chain.getTailSeconds(), 0.75, 1e-12);
            chain.addPlugin (std::make_unique<TestGainPlugin> (1.0f, std::numeric_limits<double>::infinity()));
            expectWithinAbsoluteError (chain.getTailSeconds(), PluginChain::maxTailSeconds, 1e-12);
        }

        beginTest ("engine: cue chain, master chain, tails and chain removal while playing");
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("gocue_chain_" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());
            const double sampleRate = 44100.0;
            const int block = 512;
            const auto tone = writeSine (dir, sampleRate, 1.0, 0.5f);

            AudioEngine engine (0);
            engine.prepare (sampleRate, block);
            juce::AudioBuffer<float> out (2, block);
            auto render = [&] (int blocks) { for (int i = 0; i < blocks; ++i) engine.renderBlock (out, block); };
            auto rms = [&] { return out.getRMSLevel (0, 0, block); };

            Cue cue;
            cue.file = tone;

            auto* cueGain = new TestGainPlugin (0.5f, 0.2);
            engine.getCueChain (cue.id).addPlugin (std::unique_ptr<juce::AudioPluginInstance> (cueGain));
            expectWithinAbsoluteError (cueGain->preparedSampleRate, sampleRate, 1e-9);

            expect (engine.play (cue));
            render (5);
            expectWithinAbsoluteError (rms(), 0.1768f, 0.01f);          // 0.5 file * 0.5 plugin / sqrt2
            expectGreaterThan (cueGain->processCount, 0);

            auto* masterGain = new TestGainPlugin (0.5f);
            engine.getMasterChain().addPlugin (std::unique_ptr<juce::AudioPluginInstance> (masterGain));
            render (5);
            expectWithinAbsoluteError (rms(), 0.0884f, 0.006f);         // master halves it again

            // file ends after 1 s (87 blocks); the 0.2 s tail keeps the player alive ~17 more blocks
            render (80);                                                 // 90 blocks total
            engine.reapFinishedPlayers();
            expect (engine.isPlaying (cue.id));
            expectWithinAbsoluteError (rms(), 0.0f, 1e-4f);              // silence flows through the chain during the tail
            render (20);                                                 // 110 blocks total > 87 + 18
            engine.reapFinishedPlayers();
            expect (! engine.isPlaying (cue.id));

            beginTest ("engine: a hard stop skips the tail, a fade-out keeps it");
            expect (engine.play (cue));
            render (5);
            engine.stop (cue.id);
            render (2);
            engine.reapFinishedPlayers();
            expect (! engine.isPlaying (cue.id));

            expect (engine.play (cue));
            render (5);
            engine.fadeOutAndStop (cue.id);                              // fadeOutMs 0 -> 5 ms de-click, then 0.2 s tail
            render (3);
            engine.reapFinishedPlayers();
            expect (engine.isPlaying (cue.id));
            render (20);
            engine.reapFinishedPlayers();
            expect (! engine.isPlaying (cue.id));

            beginTest ("engine: removing a cue chain while it plays detaches it safely");
            expect (engine.play (cue));
            render (5);
            expectWithinAbsoluteError (rms(), 0.0884f, 0.006f);
            engine.removeCueChain (cue.id);
            expect (engine.findCueChain (cue.id) == nullptr);
            render (3);
            expectWithinAbsoluteError (rms(), 0.1768f, 0.01f);           // only the master chain remains
            engine.getMasterChain().clear();
            render (3);
            expectWithinAbsoluteError (rms(), 0.3536f, 0.01f);

            beginTest ("engine: restarting a cue hands the chain to the new instance");
            engine.getCueChain (cue.id).addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            expect (engine.play (cue));
            render (5);
            expect (engine.play (cue));
            render (3);
            engine.reapFinishedPlayers();
            expectEquals (engine.getNumPlaying(), 1);
            expectWithinAbsoluteError (rms(), 0.1768f, 0.01f);

            engine.stopAll();
            render (3);
            engine.reapFinishedPlayers();
            engine.shutdown();
            expect (dir.deleteRecursively());
        }
    }
};

static PluginChainTests pluginChainTests;

} // namespace gocue::tests
