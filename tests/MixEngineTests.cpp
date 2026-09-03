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

        beginTest ("sends: post takes the chain's output, pre the raw input; OFF cuts the send; the return amount scales it");
        {
            auto* gain = new TestGainPlugin (0.5f);
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

        beginTest ("applySession keeps existing nodes (their live chains included) and drops removed ones");
        {
            auto* chainBefore = engine.getChannelChain (s.channels[0].id);
            expectEquals (chainBefore->getNumSlots(), 1);   // the gain plugin added live

            MixSession next = s;
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
    }
};

static MixEngineTests mixEngineTests;

} // namespace gocue::tests
