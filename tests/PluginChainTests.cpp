#include "audio/AudioEngine.h"
#include "audio/PluginChain.h"
#include "TestGainPlugin.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <thread>

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
            expectWithinAbsoluteError (buffer.getSample (0, 400), 0.5f, 1e-6f);   // output of the bypassed plugin is discarded (once the 240-sample crossfade is over)
            expectGreaterThan (buffer.getSample (0, 0), 0.25f);                    // the switch does not jump: the first samples still carry most of the plugin's output...
            expectLessThan (buffer.getSample (0, 0), buffer.getSample (0, 100));   // ...and the dry signal takes over gradually
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
            expectWithinAbsoluteError (buffer.getSample (0, 400), 0.25f, 1e-6f);   // back in, after the crossfade
            chain.setBypassed (1, true);
            fill (buffer, 1.0f);
            chain.process (buffer, 512);   // the crossfade to dry runs its course

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

        beginTest ("delay compensation: a bypassed plugin's dry signal is delayed by the plugin's latency");
        {
            PluginChain chain;
            chain.prepare (48000.0, 64);   // the bypass crossfade = 240 samples
            auto* lat = new TestGainPlugin (0.5f);
            lat->latencySamples = 16;
            chain.addPlugin (std::unique_ptr<juce::AudioPluginInstance> (lat));
            expectEquals (chain.getLatencySamples(), 16);

            juce::AudioBuffer<float> block (2, 64);
            auto silence = [&] (int blocks) { for (int i = 0; i < blocks; ++i) { block.clear(); chain.process (block, 64); } };
            auto impulse = [&] { block.clear(); block.setSample (0, 0, 1.0f); block.setSample (1, 0, 1.0f); chain.process (block, 64); };
            auto peakIndex = [&]
            {
                int best = -1;
                float bestValue = 0.0f;

                for (int i = 0; i < 64; ++i)
                    if (std::abs (block.getSample (0, i)) > bestValue)
                    {
                        bestValue = std::abs (block.getSample (0, i));
                        best = i;
                    }

                return best;
            };

            impulse();
            expectEquals (peakIndex(), 16);                                     // active: the plugin's own delay
            expectWithinAbsoluteError (block.getSample (0, 16), 0.5f, 1e-6f);
            expectWithinAbsoluteError (block.getSample (1, 16), 0.5f, 1e-6f);

            chain.setBypassed (0, true);
            silence (4);                                                        // the crossfade runs out over silence
            impulse();
            expectEquals (peakIndex(), 16);                                     // bypassed: still 16 late - the dry signal went through the same delay
            expectWithinAbsoluteError (block.getSample (0, 16), 1.0f, 1e-6f);   // ...without the plugin's gain
            expectWithinAbsoluteError (block.getSample (0, 0), 0.0f, 1e-6f);    // and nothing early

            chain.setBypassed (0, false);
            silence (4);
            impulse();
            expectEquals (peakIndex(), 16);
            expectWithinAbsoluteError (block.getSample (0, 16), 0.5f, 1e-6f);

            beginTest ("delay compensation: the switch crossfades instead of jumping");
            for (int i = 0; i < 4; ++i) { fill (block, 1.0f); chain.process (block, 64); }   // DC: the plugin and the line primed
            expectWithinAbsoluteError (block.getSample (0, 63), 0.5f, 1e-6f);
            chain.setBypassed (0, true);
            fill (block, 1.0f);
            chain.process (block, 64);
            expectGreaterThan (block.getSample (0, 0), 0.5f);                   // the first sample moved one step towards dry (1.0)...
            expectLessThan (block.getSample (0, 0), 0.51f);
            expectGreaterThan (block.getSample (0, 63), block.getSample (0, 0));
            expectLessThan (block.getSample (0, 63), 1.0f);                     // ...64 of 240 samples in, still on its way
            for (int i = 0; i < 4; ++i) { fill (block, 1.0f); chain.process (block, 64); }
            expectWithinAbsoluteError (block.getSample (0, 63), 1.0f, 1e-6f);   // settled on dry

            beginTest ("delay compensation: a busy plugin (its callback lock held) passes the dry signal through the same delay");
            chain.setBypassed (0, false);
            silence (4);
            {
                // the lock is held from another thread (a JUCE CriticalSection re-enters on its own thread): what a
                // state capture on the message thread does while the callback runs
                juce::WaitableEvent locked, release;
                std::thread holder ([&]
                {
                    const juce::ScopedLock held (lat->getCallbackLock());
                    locked.signal();
                    release.wait();
                });
                locked.wait();
                const int calls = lat->processCount;
                impulse();
                expectEquals (lat->processCount, calls);                        // the plugin did not run...
                expectEquals (peakIndex(), 16);                                 // ...and the dry signal is not early
                expectWithinAbsoluteError (block.getSample (0, 16), 1.0f, 1e-6f);
                release.signal();
                holder.join();
            }

            beginTest ("delay compensation: a latency the plugin reports later resizes the line");
            lat->setLatencyLive (32);
            expect (chain.consumeStateChanged());                               // audioProcessorChanged (latencyChanged)
            chain.refreshPluginCaches();                                        // what the engine does on that flag
            expectEquals (chain.getLatencySamples(), 32);
            chain.setBypassed (0, true);
            silence (4);
            impulse();
            expectEquals (peakIndex(), 32);
            expectWithinAbsoluteError (block.getSample (0, 32), 1.0f, 1e-6f);

            beginTest ("delay compensation: a latency change reaches the tail the cue plays on for");
            expectWithinAbsoluteError (chain.getTailSeconds(), 32.0 / 48000.0, 1e-6);   // (the cache is a float) refreshPluginCaches sized the line first, then counted it
            lat->setLatencyLive (4800);
            expect (chain.consumeStateChanged());
            chain.refreshPluginCaches();
            expectWithinAbsoluteError (chain.getTailSeconds(), 0.1, 1e-6);
            lat->setLatencyLive (32);
            chain.refreshPluginCaches();
            chain.consumeStateChanged();

            beginTest ("delay compensation: a faulted plugin's dry pass keeps the delay too");
            chain.setBypassed (0, false);
            silence (4);
            lat->emitNaN = true;
            silence (1);                                                        // NaN: the slot is faulted from here
            expect (chain.getSlot (0).faulted.load());
            impulse();
            expectEquals (peakIndex(), 32);
            expectWithinAbsoluteError (block.getSample (0, 32), 1.0f, 1e-6f);
            expectEquals (chain.getLatencySamples(), 32);

            beginTest ("delay compensation: a panic reset clears a faulted slot's line too");
            block.clear();
            block.setSample (0, 60, 1.0f);   // this would come out 32 samples later, in the next block
            chain.process (block, 64);
            chain.resetProcessing();
            silence (1);
            expectEquals (peakIndex(), -1);   // nothing from before the panic
        }

        beginTest ("delay compensation: a block the plugin missed is fed to it afterwards - no repeat, no lasting shift");
        {
            PluginChain chain;
            chain.prepare (48000.0, 64);
            auto* lat = new TestGainPlugin (0.5f);
            lat->latencySamples = 128;   // longer than a block: what it holds when a block is missed comes out later
            chain.addPlugin (std::unique_ptr<juce::AudioPluginInstance> (lat));

            juce::AudioBuffer<float> block (2, 64);
            auto process = [&] (bool withImpulse)
            {
                block.clear();

                if (withImpulse)
                    block.setSample (0, 0, 1.0f);

                chain.process (block, 64);
            };
            auto peak = [&]
            {
                int best = -1;
                float bestValue = 0.0f;

                for (int i = 0; i < 64; ++i)
                    if (std::abs (block.getSample (0, i)) > bestValue)
                    {
                        bestValue = std::abs (block.getSample (0, i));
                        best = i;
                    }

                return std::make_pair (best, bestValue);
            };

            process (true);                                                      // block A: the impulse goes in
            expectEquals (peak().first, -1);
            process (false);                                                     // block B: still inside the plugin
            expectEquals (peak().first, -1);

            {
                juce::WaitableEvent locked, release;
                std::thread holder ([&] { const juce::ScopedLock held (lat->getCallbackLock()); locked.signal(); release.wait(); });
                locked.wait();
                process (false);                                                 // block C, the plugin busy: the dry line delivers the impulse on time...
                expectEquals (peak().first, 0);
                expectWithinAbsoluteError (peak().second, 1.0f, 1e-6f);          // ...dry (the plugin's gain not on it)
                release.signal();
                holder.join();
            }

            process (false);                                                     // block D: the plugin is back, fed block C first
            expectEquals (peak().first, -1);                                     // the impulse it still held is not played a second time

            for (int i = 0; i < 4; ++i)
                process (false);                                                 // the crossfade back to the plugin's output (240 samples) runs out

            process (true);                                                      // block E: a new impulse
            process (false);                                                     // F
            process (false);                                                     // G: 128 samples after E
            expectEquals (peak().first, 0);                                      // on time - the missed block did not set the plugin back
            expectWithinAbsoluteError (peak().second, 0.5f, 1e-6f);              // and it is the plugin's output again
        }

        beginTest ("delay compensation: a longer stall is caught up a couple of blocks per callback, dry meanwhile");
        {
            PluginChain chain;
            chain.prepare (48000.0, 64);
            auto* p = new TestGainPlugin (0.5f);   // no latency: the ring still records every block
            chain.addPlugin (std::unique_ptr<juce::AudioPluginInstance> (p));
            juce::AudioBuffer<float> block (2, 64);
            auto dc = [&] { fill (block, 1.0f); chain.process (block, 64); return block.getSample (0, 63); };

            for (int i = 0; i < 6; ++i)
                dc();

            expectWithinAbsoluteError (dc(), 0.5f, 1e-6f);   // wet
            int stalled = 0;

            {
                juce::WaitableEvent locked, release;
                std::thread holder ([&] { const juce::ScopedLock held (p->getCallbackLock()); locked.signal(); release.wait(); });
                locked.wait();

                for (int i = 0; i < 4; ++i)
                {
                    expectWithinAbsoluteError (dc(), 1.0f, 1e-6f);   // dry (no latency: as it is)
                    ++stalled;
                }

                release.signal();
                holder.join();
            }

            const int before = p->processCount;
            int maxPerCallback = 0;
            float last = 0.0f;

            for (int i = 0; i < 12; ++i)
            {
                const int c0 = p->processCount;
                last = dc();
                maxPerCallback = juce::jmax (maxPerCallback, p->processCount - c0);
            }

            expectLessOrEqual (maxPerCallback, 3);                    // catchUpBlocks fed back + the current block, never more
            expectEquals (p->processCount - before, stalled + 12);    // every block went through the plugin exactly once, in order
            expectWithinAbsoluteError (last, 0.5f, 1e-6f);            // and it is wet again (after the crossfade)
        }

        beginTest ("delay compensation: a stall longer than the ring holds is answered by a reset on the message thread");
        {
            PluginChain chain;
            chain.prepare (48000.0, 64);
            auto* p = new TestGainPlugin (0.5f);
            chain.addPlugin (std::unique_ptr<juce::AudioPluginInstance> (p));
            juce::AudioBuffer<float> block (2, 64);
            auto dc = [&] { fill (block, 1.0f); chain.process (block, 64); return block.getSample (0, 63); };

            for (int i = 0; i < 6; ++i)
                dc();

            {
                juce::WaitableEvent locked, release;
                std::thread holder ([&] { const juce::ScopedLock held (p->getCallbackLock()); locked.signal(); release.wait(); });
                locked.wait();

                for (int i = 0; i < 12; ++i)
                    dc();   // 12 blocks: more than the ring (8 blocks) holds

                release.signal();
                holder.join();
            }

            const int resets = p->resetCount;

            for (int i = 0; i < 10; ++i)
                expectWithinAbsoluteError (dc(), 1.0f, 1e-6f);   // dry until the message thread has had its turn: what was lost cannot be fed back

            chain.recoverAfterStalls();
            expectEquals (p->resetCount, resets + 1);
            float last = 0.0f;

            for (int i = 0; i < 8; ++i)
                last = dc();

            expectWithinAbsoluteError (last, 0.5f, 1e-6f);   // wet again, its time the show's
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
