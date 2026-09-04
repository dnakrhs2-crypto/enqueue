#include "MixEngine.h"
#include "TestGainPlugin.h"

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
    }
};

static MixEngineTests mixEngineTests;

} // namespace gocue::tests
