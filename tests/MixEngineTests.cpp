#include "MixEngine.h"
#include "TestGainPlugin.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

namespace gocue::tests
{

using namespace gocue::livemix;

/** The LiveMix graph rendered offline with DC inputs: routing, sends, the ON/OFF ramp, meters. */
class MixEngineTests : public juce::UnitTest
{
public:
    MixEngineTests() : juce::UnitTest ("LiveMix engine", "LiveMix") {}

    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 256;
    static constexpr int numIns = 4, numOuts = 6;

    struct Io
    {
        juce::AudioBuffer<float> in { numIns, blockSize }, out { numOuts, blockSize };

        void setInput (int channel, float value) { juce::FloatVectorOperations::fill (in.getWritePointer (channel), value, blockSize); }
        float last (int channel) const { return out.getSample (channel, blockSize - 1); }
        float first (int channel) const { return out.getSample (channel, 0); }
    };

    static void render (MixEngine& engine, Io& io, int blocks = 1)
    {
        for (int i = 0; i < blocks; ++i)
            engine.renderBlock (io.in.getArrayOfReadPointers(), numIns, io.out.getArrayOfWritePointers(), numOuts, blockSize);
    }

    void runTest() override
    {
        MixEngine engine;
        engine.prepare (sampleRate, blockSize);

        MixSession s;
        s.addFx (juce::String::fromUTF8 ("리버브"));
        s.addChannel ("A");   // input 0
        s.addChannel ("B");   // input 1
        s.channels[1].output.master = false;
        s.channels[1].output.direct = true;
        s.channels[1].output.directFirst = 2;
        juce::StringArray errors;
        engine.applySession (s, &errors, true);
        expectEquals (errors.size(), 0);

        Io io;
        io.setInput (0, 0.5f);
        io.setInput (1, 0.25f);

        beginTest ("routing: A to the master pair, B to its direct pair only");
        {
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.5f, 1e-6f);
            expectWithinAbsoluteError (io.last (1), 0.5f, 1e-6f);   // mono: both sides
            expectWithinAbsoluteError (io.last (2), 0.25f, 1e-6f);
            expectWithinAbsoluteError (io.last (3), 0.25f, 1e-6f);
            expectWithinAbsoluteError (io.last (4), 0.0f, 1e-6f);

            s.channels[1].output.master = true;   // B to both
            engine.setChannelOutput (s.channels[1].id, s.channels[1].output);
            render (engine, io);
            expectWithinAbsoluteError (io.last (0), 0.75f, 1e-6f);
            expectWithinAbsoluteError (io.last (2), 0.25f, 1e-6f);
            s.channels[1].output.master = false;
            engine.setChannelOutput (s.channels[1].id, s.channels[1].output);
        }

        beginTest ("mic OFF ramps to silence within 5 ms and ON comes back");
        {
            engine.setChannelOn (s.channels[0].id, false);
            render (engine, io);
            expectGreaterThan (io.first (0), 0.4f);   // the ramp starts from the open switch
            expectWithinAbsoluteError (io.last (0), 0.0f, 0.02f);   // 256 samples = 5.3 ms: gone by the end of the block
            render (engine, io);
            expectWithinAbsoluteError (io.last (0), 0.0f, 1e-6f);
            expectWithinAbsoluteError (io.first (0), 0.0f, 1e-6f);

            engine.setChannelOn (s.channels[0].id, true);
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (0), 0.5f, 1e-6f);
        }

        TestGainPlugin* chainGain = nullptr;   // channel A's plugin, kept for the OFF test below

        beginTest ("sends: post takes the chain's output, pre the raw input; OFF cuts the send; the return amount scales it");
        {
            auto* gain = new TestGainPlugin (0.5f);
            chainGain = gain;
            engine.getChannelChain (s.channels[0].id)->addPlugin (std::unique_ptr<juce::AudioPluginInstance> (gain));
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (0), 0.25f, 1e-6f);   // A through its chain

            engine.setSend (s.channels[0].id, s.fx[0].id, 0.5, false);   // post: 0.25 * 0.5 = 0.125 into the FX, returned to the master
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (0), 0.25f + 0.125f, 1e-6f);
            const auto fxMeter = engine.readFxMeter (s.fx[0].id);
            expectWithinAbsoluteError (fxMeter.left, 0.125f, 1e-6f);

            engine.setSend (s.channels[0].id, s.fx[0].id, 0.5, true);   // pre: 0.5 * 0.5 = 0.25
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (0), 0.25f + 0.25f, 1e-6f);

            engine.setFxReturn (s.fx[0].id, 0.5);
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (0), 0.25f + 0.125f, 1e-6f);

            engine.setChannelOn (s.channels[0].id, false);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.0f, 1e-6f);   // the send is cut with the mic
            engine.setChannelOn (s.channels[0].id, true);
            engine.setSend (s.channels[0].id, s.fx[0].id, 0.0, false);
            engine.setFxReturn (s.fx[0].id, 1.0);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.25f, 1e-6f);
        }

        beginTest ("a mic that is OFF skips its plugins when asked to (no CPU), keeps the mic meter alive, and runs them again when ON");
        {
            expect (chainGain != nullptr);
            expect (engine.getSkipChainWhenOff());   // the default
            render (engine, io);
            const int whileOn = chainGain->processCount;
            render (engine, io, 2);
            expectEquals (chainGain->processCount, whileOn + 2);   // on: every block

            engine.setChannelOn (s.channels[0].id, false);
            render (engine, io, 2);   // the ramp-down block still runs the chain; then the mic is fully off
            const int afterOff = chainGain->processCount;
            (void) engine.readChannelMeter (s.channels[0].id);
            render (engine, io, 3);
            expectEquals (chainGain->processCount, afterOff);   // off: not one call
            expectWithinAbsoluteError (io.last (0), 0.0f, 1e-6f);
            expectGreaterThan (engine.readChannelMeter (s.channels[0].id).left, 0.4f);   // the meter shows the mic itself (0.5)

            engine.setSkipChainWhenOff (false);
            render (engine, io, 2);
            expectEquals (chainGain->processCount, afterOff + 2);   // not asked to skip: the chain keeps running while off
            expectWithinAbsoluteError (io.last (0), 0.0f, 1e-6f);
            engine.setSkipChainWhenOff (true);

            engine.setChannelOn (s.channels[0].id, true);
            const int beforeOn = chainGain->processCount;
            render (engine, io, 2);
            expectEquals (chainGain->processCount, beforeOn + 2);   // on again: every block
            expectWithinAbsoluteError (io.last (0), 0.25f, 1e-6f);  // back through the chain (0.5 * 0.5)
        }

        beginTest ("a plugin that produces NaN is quarantined: its block passes dry, the slot is faulted, the operator hears of it once");
        {
            auto* poison = new TestGainPlugin (0.5f);
            poison->emitNaN = true;
            engine.getChannelChain (s.channels[1].id)->addPlugin (std::unique_ptr<juce::AudioPluginInstance> (poison));   // B -> direct 2-3
            render (engine, io, 2);
            expect (std::isfinite (io.last (2)) && std::isfinite (io.first (2)));
            expectWithinAbsoluteError (io.last (2), 0.25f, 1e-6f);   // B's input (0.25) untouched: the plugin's output was thrown away
            juce::StringArray faults;
            engine.forEachChain ([&faults] (PluginChain& chain) { faults.addArray (chain.takeNewFaults()); });
            expectEquals (faults.joinIntoString (","), juce::String ("TestGain"));
            faults.clear();
            engine.forEachChain ([&faults] (PluginChain& chain) { faults.addArray (chain.takeNewFaults()); });
            expect (faults.isEmpty());   // told once
            const auto meter = engine.readChannelMeter (s.channels[1].id);
            expect (std::isfinite (meter.left));
            engine.getChannelChain (s.channels[1].id)->removePlugin (0);
        }

        beginTest ("a plugin whose callback lock is busy (a save capturing its state) is passed dry, never waited for");
        {
            expect (chainGain != nullptr);
            std::atomic<bool> release { false }, holding { false };
            std::thread holder ([&]
            {
                const juce::ScopedLock sl (chainGain->getCallbackLock());
                holding.store (true);

                while (! release.load())
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
            });

            while (! holding.load())
                std::this_thread::sleep_for (std::chrono::milliseconds (1));

            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            render (engine, io, 2);
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
            expectWithinAbsoluteError (io.last (0), 0.5f, 1e-6f);   // A dry: the 0.5 gain plugin was skipped
            expectLessThan (elapsed, 200.0);                          // and nobody waited for the lock
            release.store (true);
            holder.join();
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (0), 0.25f, 1e-6f);   // back through the chain
        }

        beginTest ("a moved send or return fader ramps across the block instead of stepping");
        {
            engine.setSend (s.channels[0].id, s.fx[0].id, 1.0, false);   // post: 0.25 into the FX (bypass chain), back to the master
            render (engine, io);
            expectWithinAbsoluteError (io.first (0), 0.25f, 0.01f);              // the block starts where the last one ended (no send)
            expectWithinAbsoluteError (io.last (0), 0.25f + 0.25f, 0.01f);       // and ends at the new level
            render (engine, io);
            expectWithinAbsoluteError (io.first (0), 0.5f, 1e-6f);               // steady from then on
            engine.setFxReturn (s.fx[0].id, 0.0);
            render (engine, io);
            expectGreaterThan (io.first (0), 0.45f);                             // the return ramps down across the block
            expectWithinAbsoluteError (io.last (0), 0.25f, 0.01f);
            engine.setSend (s.channels[0].id, s.fx[0].id, 0.0, false);
            engine.setFxReturn (s.fx[0].id, 1.0);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.25f, 1e-6f);
        }

        beginTest ("the FX channel can go to a direct pair instead of the master; the master chain shapes the main outputs");
        {
            engine.setSend (s.channels[0].id, s.fx[0].id, 1.0, true);   // 0.5 into the FX
            MixOutput fxOut;
            fxOut.master = false;
            fxOut.direct = true;
            fxOut.directFirst = 4;
            engine.setFxOutput (s.fx[0].id, fxOut);
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (0), 0.25f, 1e-6f);   // no FX on the master
            expectWithinAbsoluteError (io.last (4), 0.5f, 1e-6f);
            expectWithinAbsoluteError (io.last (5), 0.5f, 1e-6f);

            auto* masterGain = new TestGainPlugin (0.5f);
            engine.getMasterChain().addPlugin (std::unique_ptr<juce::AudioPluginInstance> (masterGain));
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (0), 0.125f, 1e-6f);
            expectWithinAbsoluteError (io.last (4), 0.5f, 1e-6f);   // a direct output is not the master
            engine.getMasterChain().clear();
            engine.setSend (s.channels[0].id, s.fx[0].id, 0.0, false);
            fxOut.master = true;
            fxOut.direct = false;
            engine.setFxOutput (s.fx[0].id, fxOut);
        }

        beginTest ("an FX channel's mono switch sums its two sides at half level onto both outputs");
        {
            MixEngine fresh;
            fresh.prepare (sampleRate, blockSize);
            MixSession m;
            m.addFx ("verb");
            m.addChannel ("S");
            m.channels[0].inputFirst = 2;                   // inputs 2 (0.1) and 3 (0.2)
            m.channels[0].stereo = true;
            m.channels[0].output.master = false;            // the mic itself goes nowhere: only the FX is heard
            m.sendFor (m.channels[0], m.fx[0].id) = { m.fx[0].id, 1.0, true };
            m.fx[0].output.master = false;
            m.fx[0].output.direct = true;
            m.fx[0].output.directFirst = 4;
            fresh.applySession (m, nullptr, true);

            Io stereoIo;
            stereoIo.setInput (2, 0.1f);
            stereoIo.setInput (3, 0.2f);
            render (fresh, stereoIo, 2);
            expectWithinAbsoluteError (stereoIo.last (4), 0.1f, 1e-6f);   // stereo: each side as it came
            expectWithinAbsoluteError (stereoIo.last (5), 0.2f, 1e-6f);

            fresh.setFxMono (m.fx[0].id, true);
            render (fresh, stereoIo, 2);
            expectWithinAbsoluteError (stereoIo.last (4), 0.15f, 1e-6f);  // mono: (0.1 + 0.2) / 2 on both sides
            expectWithinAbsoluteError (stereoIo.last (5), 0.15f, 1e-6f);

            m.fx[0].mono = true;                            // and it comes back from a session
            MixEngine again;
            again.prepare (sampleRate, blockSize);
            again.applySession (m, nullptr, true);
            render (again, stereoIo, 2);
            expectWithinAbsoluteError (stereoIo.last (4), 0.15f, 1e-6f);
        }

        beginTest ("a stereo channel takes an input pair; the main output pair can move; meters report the chain's output");
        {
            io.setInput (2, 0.1f);
            io.setInput (3, 0.2f);
            engine.setChannelInput (s.channels[1].id, 2, true);
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (2), 0.1f, 1e-6f);   // B's direct pair now carries its stereo input
            expectWithinAbsoluteError (io.last (3), 0.2f, 1e-6f);

            engine.setMasterOutput (4);
            render (engine, io, 2);
            expectWithinAbsoluteError (io.last (4), 0.25f, 1e-6f);
            expectWithinAbsoluteError (io.last (0), 0.0f, 1e-6f);
            engine.setMasterOutput (0);

            engine.readChannelMeter (s.channels[0].id);
            render (engine, io);
            const auto m = engine.readChannelMeter (s.channels[0].id);
            expectWithinAbsoluteError (m.left, 0.25f, 1e-6f);   // after the 0.5 gain plugin
            const auto again = engine.readChannelMeter (s.channels[0].id);
            expectWithinAbsoluteError (again.left, 0.0f, 1e-6f);   // "since the last read"
            engine.readMasterMeter();   // drop what earlier blocks accumulated
            render (engine, io);
            const auto master = engine.readMasterMeter();
            expectWithinAbsoluteError (master.left, 0.25f, 1e-6f);
        }

        MixSession next = s;

        beginTest ("applySession keeps existing nodes (their live chains included) and drops removed ones");
        {
            auto* chainBefore = engine.getChannelChain (s.channels[0].id);
            expectEquals (chainBefore->getNumSlots(), 1);   // the gain plugin added live

            next.removeChannel (s.channels[1].id);
            next.addChannel ("C");
            next.channels[1].inputFirst = 3;
            engine.applySession (next);   // no chain restore: the live chains stay
            expect (engine.getChannelChain (s.channels[0].id) == chainBefore);
            expectEquals (engine.getChannelChain (s.channels[0].id)->getNumSlots(), 1);
            expect (engine.getChannelChain (s.channels[1].id) == nullptr);
            expect (engine.getChannelChain (next.channels[1].id) != nullptr);

            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.25f + 0.2f, 1e-6f);   // A (0.25) + C (input 3 = 0.2) on the master
            expectWithinAbsoluteError (io.last (2), 0.0f, 1e-6f);           // B's direct output is gone

            MixSession captured = next;
            engine.captureLivePluginStates (captured);
            expectEquals ((int) captured.channels[0].chain.size(), 1);
            expectEquals (captured.channels[0].chain[0].name, juce::String ("TestGain"));
        }

        beginTest ("a master plugin removed live stays removed through a structural edit (the stale model does not bring it back)");
        {
            engine.getMasterChain().addPlugin (std::unique_ptr<juce::AudioPluginInstance> (new TestGainPlugin (0.5f)));
            MixSession saved = next;
            engine.captureLivePluginStates (saved);   // the model now remembers the master plugin
            expectEquals ((int) saved.master.chain.size(), 1);

            engine.getMasterChain().removePlugin (0);   // removed live: the model is stale until the next save
            saved.addChannel ("D");                     // a structural edit with that stale master state in the model
            engine.applySession (saved);
            expectEquals (engine.getMasterChain().getNumSlots(), 0);
            next = saved;
        }

        beginTest ("opening a file rebuilds every node and chain off the graph: new chain objects, the old plugins gone");
        {
            auto* channelChainBefore = engine.getChannelChain (next.channels[0].id);
            auto* masterBefore = &engine.getMasterChain();
            expectGreaterThan (TestGainPlugin::liveInstances, 0);

            juce::StringArray loadErrors;
            engine.applySession (next, &loadErrors, true);   // like a load: the real host cannot recreate TestGain, the slot stays as "missing"
            expect (engine.getChannelChain (next.channels[0].id) != channelChainBefore);
            expect (&engine.getMasterChain() != masterBefore);
            expectEquals (engine.getChannelChain (next.channels[0].id)->getNumSlots(), 1);
            expect (engine.getChannelChain (next.channels[0].id)->getSlot (0).isMissing());
            expectEquals (engine.getMasterChain().getNumSlots(), 1);   // the saved (stale) master slot, as a load must
            expectEquals (TestGainPlugin::liveInstances, 0);            // the old instances were destroyed after the swap
            expectGreaterThan (loadErrors.size(), 0);

            render (engine, io, 3);   // the new graph runs: A (input 0 = 0.5, a missing slot passes the signal) + C (input 3 = 0.2); D has no input
            expectWithinAbsoluteError (io.last (0), 0.7f, 1e-6f);
        }

        runPanTests();
    }

    void runPanTests()
    {
        beginTest ("mono constant-power pan keeps centre at unity on both sides; stereo balance leaves the favoured side untouched");
        for (const bool stereo : { false, true })
        {
            MixEngine engine;
            MixSession s;
            s.addChannel ("Pan");
            auto& c = s.channels[0];
            c.stereo = stereo;
            c.output.direct = true;   // the same pair goes to master 1-2 and direct 3-4
            engine.applySession (s, nullptr, true);
            Io io;
            io.in.clear();
            io.setInput (0, 0.5f);
            io.setInput (1, 0.25f);
            const float rightInput = stereo ? 0.25f : 0.5f;
            struct Position { double pan; float left, right; };
            const Position mono[] { { 0.0, 1.0f, 1.0f }, { -1.0, std::sqrt (2.0f), 0.0f }, { 1.0, 0.0f, std::sqrt (2.0f) },
                                    { -0.5, 1.306562965f, 0.541196100f }, { 0.5, 0.541196100f, 1.306562965f } };
            const Position balance[] { { 0.0, 1.0f, 1.0f }, { -1.0, 1.0f, 0.0f }, { 1.0, 0.0f, 1.0f },
                                       { -0.5, 1.0f, 0.707106781f }, { 0.5, 0.707106781f, 1.0f } };
            for (const auto& p : stereo ? balance : mono)
            {
                engine.setChannelPan (c.id, p.pan);
                render (engine, io, 3);
                for (int pair : { 0, 2 })
                {
                    expectWithinAbsoluteError (io.first (pair), 0.5f * p.left, 1e-6f);
                    expectWithinAbsoluteError (io.last (pair), 0.5f * p.left, 1e-6f);
                    expectWithinAbsoluteError (io.last (pair + 1), rightInput * p.right, 1e-6f);
                }
                if (! stereo)
                    expectWithinAbsoluteError (io.last (0) * io.last (0) + io.last (1) * io.last (1), 0.5f, 1e-6f);
            }

            c.pan = -1.0;
            engine.applySession (s, nullptr, true);   // a loaded pan starts at its saved position, with no centre transient
            render (engine, io);
            expectWithinAbsoluteError (io.first (0), stereo ? 0.5f : std::sqrt (0.5f), 1e-6f);
            expectWithinAbsoluteError (io.first (1), 0.0f, 1e-6f);
            engine.setChannelPan (c.id, 9.0);   // the engine API also clamps callers
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.0f, 1e-6f);
            expectWithinAbsoluteError (io.last (1), rightInput * (stereo ? 1.0f : std::sqrt (2.0f)), 1e-6f);
        }

        beginTest ("pan ramps are continuous for short, long and oversized blocks at different rates and finish after 10 ms");
        for (const double rate : { 44100.0, 48000.0, 96000.0 })
        for (const int block : { 16, 64, 256, 1024 })
        for (const bool stereo : { false, true })
        {
            MixEngine engine;
            engine.prepare (rate, block);
            MixSession s;
            s.addChannel ("Ramp");
            s.channels[0].pan = -1.0;
            s.channels[0].stereo = stereo;
            s.channels[0].output.direct = true;
            engine.applySession (s, nullptr, true);
            const int request = block * 3 + 7;   // crosses internal chunks and has a short final chunk
            juce::AudioBuffer<float> input (2, request), output (4, request);
            for (int side = 0; side < 2; ++side)
                juce::FloatVectorOperations::fill (input.getWritePointer (side), 0.25f, request);
            const float hard = 0.25f * (stereo ? 1.0f : std::sqrt (2.0f));
            const int ramp = juce::roundToInt (rate * MixEngine::panRampSeconds);
            const float tolerance = 5e-6f;   // JUCE's float gain accumulator rounds across up to 960 ramp samples
            std::array<float, 2> previous { hard, 0.0f };
            float maxJump = 0.0f, maxError = 0.0f, pairError = 0.0f;
            engine.setChannelPan (s.channels[0].id, 1.0);
            int elapsed = 0;
            while (elapsed <= ramp + request)
            {
                engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), 4, request);
                for (int i = 0; i < request; ++i)
                {
                    const float fraction = juce::jmin (1.0f, (float) (elapsed + i) / (float) ramp);
                    const float expected[] { hard * (1.0f - fraction), hard * fraction };
                    for (int side = 0; side < 2; ++side)
                    {
                        const float sample = output.getSample (side, i);
                        maxJump = juce::jmax (maxJump, std::abs (sample - previous[(size_t) side]));
                        maxError = juce::jmax (maxError, std::abs (sample - expected[side]));
                        pairError = juce::jmax (pairError, std::abs (sample - output.getSample (side + 2, i)));
                        previous[(size_t) side] = sample;
                    }
                }
                elapsed += request;
            }
            expectLessThan (maxJump, hard / (float) ramp + tolerance);
            expectLessThan (maxError, tolerance);
            expectLessThan (pairError, 1e-7f);

            // Reverse the target while a ramp is still moving; a structural edit reuses the node and its progress.
            for (const double pan : { -1.0, 1.0, 0.0 })
            {
                s.channels[0].pan = pan;
                engine.setChannelPan (s.channels[0].id, pan);
                engine.applySession (s);
                engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), 4, 16);
                for (int side = 0; side < 2; ++side)
                {
                    expectLessThan (std::abs (output.getSample (side, 0) - previous[(size_t) side]), hard / (float) ramp + tolerance);
                    previous[(size_t) side] = output.getSample (side, 15);
                }
            }
        }

        beginTest ("pan is after the chain and switch; pre/post sends and the channel meter keep their stereo image during a pan ramp");
        for (const bool stereo : { false, true })
        for (const bool pre : { false, true })
        {
            MixEngine engine;
            MixSession s;
            s.addFx();
            s.addChannel();
            auto& c = s.channels[0];
            c.stereo = stereo;
            c.output.direct = true;
            s.fx[0].output = { false, true, 4 };
            c.sends[0].amount = 0.4;
            c.sends[0].pre = pre;
            engine.applySession (s, nullptr, true);
            engine.getChannelChain (c.id)->addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            Io io;
            io.in.clear();
            io.setInput (0, 0.5f);
            io.setInput (1, 0.25f);
            const float right = stereo ? 0.25f : 0.5f;
            render (engine, io);
            for (const double pan : { -1.0, 1.0, 0.0 })
            {
                engine.setChannelPan (c.id, pan);
                for (int b = 0; b < 3; ++b)
                {
                    engine.readChannelMeter (c.id);
                    render (engine, io);
                    float error = 0.0f;
                    for (int i = 0; i < blockSize; ++i)
                    {
                        error = juce::jmax (error, std::abs (io.out.getSample (4, i) - 0.5f * (pre ? 0.4f : 0.2f)),
                                                  std::abs (io.out.getSample (5, i) - right * (pre ? 0.4f : 0.2f)));
                    }
                    expectLessThan (error, 1e-6f);
                    const auto meter = engine.readChannelMeter (c.id);
                    expectWithinAbsoluteError (meter.left, 0.25f, 1e-6f);
                    expectWithinAbsoluteError (meter.right, right * 0.5f, 1e-6f);
                }
                if (pan < 0.0)
                {
                    expectWithinAbsoluteError (io.last (0), stereo ? 0.25f : 0.25f * std::sqrt (2.0f), 1e-6f);
                    expectWithinAbsoluteError (io.last (3), 0.0f, 1e-6f);
                }
            }
            for (const bool skip : { false, true })
            {
                engine.setSkipChainWhenOff (skip);
                engine.setChannelOn (c.id, false);
                render (engine, io, 3);
                engine.setChannelPan (c.id, 1.0);
                render (engine, io);
                expectWithinAbsoluteError (io.out.getMagnitude (0, blockSize), 0.0f, 1e-6f);   // all outputs, sends included
                engine.setChannelOn (c.id, true);
                render (engine, io);
                expectWithinAbsoluteError (io.out.getMagnitude (0, 0, blockSize), 0.0f, 1e-6f);   // no stale left pan on unmute
                render (engine, io, 2);
                expectWithinAbsoluteError (io.last (3), right * 0.5f * (stereo ? 1.0f : std::sqrt (2.0f)), 1e-6f);
                engine.setChannelPan (c.id, 0.0);
                render (engine, io, 3);
            }
        }
    }
};

static MixEngineTests mixEngineTests;

} // namespace gocue::tests
