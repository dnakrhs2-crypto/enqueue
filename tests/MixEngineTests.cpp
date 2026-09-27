#include "MixEngine.h"
#include "AudioBackends.h"
#include "LiveMixSettings.h"
#include "TestGainPlugin.h"
#include "ui/SettingsDialog.h"
#include "ui/TopBar.h"
#include "ui/LiveMixLookAndFeel.h"
#include "../livemix/src/ui/MainComponent.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>
#include <windows.h>

namespace gocue::tests
{

using namespace gocue::livemix;

namespace
{
    struct MixDeviceRecord
    {
        juce::String input, output;
        juce::BigInteger inputs, outputs;
        bool playing = false;
        juce::AudioIODevice* device = nullptr;
        double rate = 48000.0;
        int buffer = 256;
        juce::AudioIODeviceCallback* callback = nullptr;
    };

    class MixFakeDevice : public juce::AudioIODevice
    {
    public:
        MixFakeDevice (const juce::String& type, std::shared_ptr<MixDeviceRecord> r)
            : AudioIODevice (r->output.isNotEmpty() ? r->output : r->input, type), record (std::move (r)) { record->device = this; }
        ~MixFakeDevice() override { stop(); }
        juce::StringArray channelNames (bool input) const
        {
            juce::StringArray names;
            if ((input ? record->input : record->output).isNotEmpty())
                for (int i = 0; i < (getTypeName().contains ("ASIO") ? 72 : 8); ++i) names.add (juce::String (i + 1));
            return names;
        }
        juce::StringArray getInputChannelNames() override { return channelNames (true); }
        juce::StringArray getOutputChannelNames() override { return channelNames (false); }
        juce::Array<double> getAvailableSampleRates() override { return { 48000.0, 44100.0 }; }
        juce::Array<int> getAvailableBufferSizes() override { return { 128, 256, 512, 1024 }; }
        int getDefaultBufferSize() override { return 256; }
        juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double sr, int bs) override
        {
            close();
            if (record->input == "Broken" || record->output == "Broken" || bs == 1024)
                return "deliberate fake open failure";
            record->inputs = ins;
            record->outputs = outs;
            record->inputs.setRange (getInputChannelNames().size(), 128, false);
            record->outputs.setRange (getOutputChannelNames().size(), 128, false);
            record->rate = sr;
            record->buffer = bs;
            opened = true;
            return {};
        }
        void close() override { stop(); opened = false; }
        bool isOpen() override { return opened; }
        void start (juce::AudioIODeviceCallback* cb) override
        {
            callback = cb;
            record->callback = cb;
            record->playing = cb != nullptr && opened;
            if (cb != nullptr) cb->audioDeviceAboutToStart (this);
        }
        void stop() override
        {
            if (callback != nullptr) callback->audioDeviceStopped();
            callback = nullptr;
            record->callback = nullptr;
            record->playing = false;
        }
        bool isPlaying() override { return record->playing; }
        juce::String getLastError() override { return {}; }
        int getCurrentBufferSizeSamples() override { return record->buffer; }
        double getCurrentSampleRate() override { return record->rate; }
        int getCurrentBitDepth() override { return 32; }
        juce::BigInteger getActiveInputChannels() const override { return record->inputs; }
        juce::BigInteger getActiveOutputChannels() const override { return record->outputs; }
        int getInputLatencyInSamples() override { return record->input.isEmpty() ? 0 : 48; }
        int getOutputLatencyInSamples() override { return record->output.isEmpty() ? 0 : 96; }
    private:
        std::shared_ptr<MixDeviceRecord> record;
        juce::AudioIODeviceCallback* callback = nullptr;
        bool opened = false;
    };

    class MixFakeType : public juce::AudioIODeviceType
    {
    public:
        explicit MixFakeType (const juce::String& name) : AudioIODeviceType (name) {}
        void scanForDevices() override {}
        juce::StringArray getDeviceNames (bool input) const override
        {
            if (getTypeName().contains ("ASIO")) return { "Good", "Broken" };
            return input ? juce::StringArray { "Capture", "Capture 2", "Broken" }
                         : juce::StringArray { "Headphones", "Broken" };
        }
        int getDefaultDeviceIndex (bool) const override { return 0; }
        bool hasSeparateInputsAndOutputs() const override { return ! getTypeName().contains ("ASIO"); }
        int getIndexOfDevice (juce::AudioIODevice* d, bool input) const override
        { return d == nullptr ? -1 : getDeviceNames (input).indexOf (d->getName()); }
        juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
        {
            auto r = std::make_shared<MixDeviceRecord>();
            r->input = input;
            r->output = output;
            if (failOutputs && output.isNotEmpty()) r->output = "Broken";
            records.push_back (r);
            return new MixFakeDevice (getTypeName(), std::move (r));
        }
        std::shared_ptr<MixDeviceRecord> playingOutput() const
        {
            for (auto& r : records)
                if (r->playing && r->output.isNotEmpty()) return r;
            return {};
        }
        std::vector<std::shared_ptr<MixDeviceRecord>> records;
        bool failOutputs = false;
    };

    void removeRealMixDeviceTypes (MixEngine& engine)
    {
        auto& manager = engine.getDeviceManager();
        while (! manager.getAvailableDeviceTypes().isEmpty())
            manager.removeAudioDeviceType (manager.getAvailableDeviceTypes().getLast());
    }
}

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
        runDeviceTests();
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

    void runDeviceTests()
    {
        beginTest ("input-only rendering still advances channel/master meters and loudness");
        {
            MixEngine engine;
            MixSession s;
            s.addChannel();
            engine.applySession (s);
            juce::AudioBuffer<float> input (1, 256);
            int sample = 0;
            for (int b = 0; b < 200; ++b)
            {
                for (int i = 0; i < 256; ++i)
                    input.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * sample++ / 48.0));
                engine.renderBlock (input.getArrayOfReadPointers(), 1, nullptr, 0, 256);
                engine.getLoudnessMeter().poll();
            }
            expectGreaterThan (engine.readChannelMeter (s.channels[0].id).left, 0.49f);
            expectGreaterThan (engine.readMasterMeter().right, 0.49f);
            expectGreaterOrEqual (engine.getLoudnessMeter().getStats().elapsedSeconds(), 1.0);
            expect (engine.getLoudnessMeter().getStats().integrated().valid);
        }

        beginTest ("failed device changes restore type, both endpoints, channel masks and monitor");
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto& manager = engine.getDeviceManager();
            manager.addAudioDeviceType (std::make_unique<MixFakeType> ("ASIO"));
            auto windows = std::make_unique<MixFakeType> ("Windows Audio");
            auto* windowsType = windows.get();
            manager.addAudioDeviceType (std::move (windows));
            expect (engine.openDevice ({ "ASIO", "Good", "Good", 256, 48000.0 }).isEmpty());
            expect (engine.isDeviceRunning());
            expectEquals (engine.getNumDeviceInputs(), 64);
            expectEquals (engine.getNumDeviceOutputs(), 64);
            auto previous = manager.getAudioDeviceSetup();
            previous.inputChannels.clear();
            previous.inputChannels.setBit (0);
            previous.inputChannels.setBit (3);
            previous.outputChannels.clear();
            previous.outputChannels.setRange (2, 2, true);
            expect (manager.setAudioDeviceSetup (previous, true).isEmpty());
            previous = manager.getAudioDeviceSetup();
            expect (engine.openDevice ({ "ASIO", "Broken", "Broken", 256, 48000.0 }).isNotEmpty());
            expect (engine.isDeviceRunning() && manager.getCurrentAudioDevice()->isPlaying());
            expect (manager.getAudioDeviceSetup() == previous);
            expectEquals (engine.getOpenDevice().input, juce::String ("Good"));

            expect (engine.openDevice ({ "Windows Audio", "Capture", "Broken", 256, 48000.0 }).isNotEmpty());
            expectEquals (engine.getOpenDevice().type, juce::String ("ASIO"));
            expect (manager.getAudioDeviceSetup() == previous);
            expect (engine.isDeviceRunning() && ! engine.isSplitMonitor());

            const MixDevice split { "Windows Audio", "Capture", "Headphones", 256, 48000.0 };
            expect (engine.openDevice (split).isEmpty());
            expect (engine.isDeviceRunning() && engine.isSplitMonitor());
            expect (manager.getAudioDeviceSetup().outputDeviceName.isEmpty());
            expectEquals (manager.getCurrentAudioDevice()->getActiveOutputChannels().countNumberOfSetBits(), 0);
            expectEquals (engine.getNumDeviceOutputs(), 2);
            auto monitor = windowsType->playingOutput();
            expect (monitor != nullptr);
            if (monitor != nullptr)
            {
                expect (monitor->input.isEmpty());
                expectEquals (monitor->outputs.toInteger(), 3);
            }
            expectWithinAbsoluteError (engine.getLatencyMs(), 1.0 + 2.0 + 3.0 * 256.0 / 48.0 + 3.0, 0.001);
            if (monitor != nullptr && monitor->callback != nullptr)
            {
                MixSession session;
                session.addChannel();
                session.channels[0].output = { true, true, 4 };
                session.master.outputFirst = 4;
                engine.applySession (session, nullptr, true);
                Io io;
                io.in.clear();
                io.setInput (0, 0.25f);
                // The graph has zero physical outputs. Its master AND direct pair reach the second device.
                for (int b = 0; b < 100; ++b)
                {
                    engine.renderBlock (io.in.getArrayOfReadPointers(), numIns, nullptr, 0, blockSize);
                    monitor->callback->audioDeviceIOCallbackWithContext (nullptr, 0, io.out.getArrayOfWritePointers(), 2, blockSize, {});
                }
                expectWithinAbsoluteError (io.last (0), 0.5f, 1.0e-5f);
                expectWithinAbsoluteError (io.last (1), 0.5f, 1.0e-5f);
            }
            previous = manager.getAudioDeviceSetup();
            expect (engine.openDevice ({ "Windows Audio", "Capture 2", "Broken", 512, 44100.0 }).isNotEmpty());
            expect (engine.isDeviceRunning() && engine.isSplitMonitor());
            expect (manager.getAudioDeviceSetup() == previous);
            expectEquals (engine.getOpenDevice().output, juce::String ("Headphones"));
            monitor = windowsType->playingOutput();
            expect (monitor != nullptr);
            if (monitor != nullptr)
            {
                expectEquals (monitor->buffer, 256);
                expectWithinAbsoluteError (monitor->rate, 48000.0, 0.01);
            }
            const auto openedBefore = windowsType->records.size();
            expect (engine.setBufferSize (512).isEmpty());
            expectEquals (engine.getOpenDevice().bufferSize, 256);
            expect (openedBefore == windowsType->records.size());
            expect (engine.restartDevice().isEmpty());
            expect (engine.isSplitMonitor() && engine.isDeviceRunning());
            expect (windowsType->playingOutput() != nullptr);

            expect (engine.openDevice ({ "Windows Audio", "Capture", "", 256, 48000.0 }).isEmpty());
            expect (! engine.isSplitMonitor() && engine.isDeviceRunning());
            expectEquals (engine.getNumDeviceOutputs(), 0);
            expect (windowsType->playingOutput() == nullptr);
            expect (engine.getOpenDevice().output.isEmpty());
            expect (engine.openDevice ({ "Windows Audio", "", "Headphones", 256, 48000.0 }).isNotEmpty());
            expect (engine.isDeviceRunning() && engine.getOpenDevice().output.isEmpty());
            for (const auto& record : windowsType->records)
                expect (record->input.isEmpty() || record->output.isEmpty(), "a split type switch must never open a default duplex device");
        }

        beginTest ("low-latency and exclusive split monitors keep working through buffer changes, failures and restart");
        for (const auto* typeName : { "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" })
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> (typeName));
            expect (engine.openDevice ({ typeName, "Capture", "Headphones", 256, 48000.0 }).isEmpty());
            expect (engine.setBufferSize (512).isEmpty());
            expectEquals (engine.getOpenDevice().bufferSize, 512);
            expect (engine.isSplitMonitor() && engine.isDeviceRunning());
            expect (engine.setBufferSize (1024).isNotEmpty());
            expectEquals (engine.getOpenDevice().bufferSize, 512);
            expect (engine.isSplitMonitor() && engine.isDeviceRunning());
            expect (engine.restartDevice().isEmpty());
            expectEquals (engine.getOpenDevice().type, juce::String (typeName));
            expectEquals (engine.getOpenDevice().bufferSize, 512);
        }

        beginTest ("split monitor is rebuilt after input rate and output period restarts, retaining 1 kHz pitch");
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto type = std::make_unique<MixFakeType> ("Windows Audio");
            auto* fake = type.get();
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            expect (engine.openDevice ({ "Windows Audio", "Capture", "Headphones", 256, 48000.0 }).isEmpty());
            MixSession s;
            s.addChannel();
            engine.applySession (s);
            auto originalOutput = fake->playingOutput();
            auto* inputDevice = engine.getDeviceManager().getCurrentAudioDevice();
            std::shared_ptr<MixDeviceRecord> inputRecord;
            for (const auto& record : fake->records)
                if (record->device == inputDevice)
                {
                    inputRecord = record;
                    record->callback->audioDeviceStopped();
                    record->rate = 44100.0;
                    record->callback->audioDeviceAboutToStart (inputDevice);
                    break;
                }
            expect (! engine.isMonitorRunning());
            auto pump = [&]
            {
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 1500.0;
                while (! engine.isMonitorRunning() && juce::Time::getMillisecondCounterHiRes() < deadline)
                {
                    MSG message;
                    while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage (&message);
                        DispatchMessageW (&message);
                    }
                    juce::Thread::sleep (2);
                }
                expect (engine.isMonitorRunning());
            };
            pump();
            auto outputDevice = fake->playingOutput();
            expect (outputDevice != nullptr && outputDevice != originalOutput);
            if (outputDevice != nullptr && outputDevice->callback != nullptr)
            {
                // Schedule the actual engine and monitor callbacks on their two independent nominal clocks.
                std::array<float, 256> input;
                const float* inputs[] { input.data() };
                juce::AudioBuffer<float> output (2, outputDevice->buffer);
                double nextInput = 0.0, firstCrossing = -1.0, lastCrossing = -1.0;
                int inputSample = 0, crossings = 0, outputSample = 0;
                float previous = 0.0f;
                for (int block = 0; block < 1200; ++block)
                {
                    const double time = block * outputDevice->buffer / outputDevice->rate;
                    while (nextInput <= time)
                    {
                        for (auto& x : input) x = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * inputSample++ / 44100.0);
                        inputRecord->callback->audioDeviceIOCallbackWithContext (inputs, 1, nullptr, 0, (int) input.size(), {});
                        nextInput += input.size() / 44100.0;
                    }
                    outputDevice->callback->audioDeviceIOCallbackWithContext (nullptr, 0, output.getArrayOfWritePointers(), 2, output.getNumSamples(), {});
                    for (int i = 0; i < output.getNumSamples(); ++i, ++outputSample)
                    {
                        const float current = output.getSample (0, i);
                        if (time > 1.0 && previous < 0.0f && current >= 0.0f)
                        {
                            const double crossing = outputSample - current / (current - previous);
                            if (firstCrossing < 0.0) firstCrossing = crossing;
                            lastCrossing = crossing;
                            ++crossings;
                        }
                        previous = current;
                    }
                }
                expectGreaterThan (crossings, 1000);
                const double hz = (crossings - 1) * outputDevice->rate / (lastCrossing - firstCrossing);
                expectWithinAbsoluteError (1200.0 * std::log2 (hz / 1000.0), 0.0, 1.0);
                outputDevice->callback->audioDeviceStopped();
                outputDevice->buffer = 512;
                outputDevice->rate = 44100.0;
                outputDevice->callback->audioDeviceAboutToStart (outputDevice->device);
                expect (! engine.isMonitorRunning());
                pump();
                const auto rebuilt = fake->playingOutput();
                expect (rebuilt != nullptr && rebuilt != outputDevice);
                if (rebuilt != nullptr)
                {
                    expectEquals (rebuilt->buffer, 512);
                    expectWithinAbsoluteError (rebuilt->rate, 44100.0, 0.01);
                    struct IsolatedFolder
                    {
                        juce::File file = juce::File::createTempFile ("-monitor-status");
                        ~IsolatedFolder() { file.deleteRecursively(); }
                    } isolated;
                    const auto folder = isolated.file;
                    LiveMixSettings settings (folder);
                    MixDocument document (engine);
                    document.applyToEngine();
                    ObsPluginActions actions;
                    actions.roots = [folder] { return ObsPluginInstaller::Roots { folder, folder, folder, [] { return false; }, {} }; };
                    actions.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
                    gocue::livemix::MainComponent main (document, settings, actions);
                    fake->failOutputs = true;
                    rebuilt->callback->audioDeviceStopped();
                    rebuilt->buffer = 128;
                    rebuilt->callback->audioDeviceAboutToStart (rebuilt->device);
                    bool stoppedStatus = false;
                    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
                    while (juce::Time::getMillisecondCounterHiRes() < deadline)
                    {
                        MSG message;
                        while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
                        {
                            TranslateMessage (&message);
                            DispatchMessageW (&message);
                        }
                        for (auto* child : main.getChildren())
                            if (auto* label = dynamic_cast<juce::Label*> (child))
                                stoppedStatus |= label->getText() == ko ("모니터 출력 멈춤 - 설정에서 출력 장치를 확인하세요");
                        juce::Thread::sleep (2);
                    }
                    expect (engine.isDeviceRunning() && ! engine.isMonitorRunning());
                    expect (stoppedStatus);
                    fake->failOutputs = false;
                    expect (engine.restartDevice().isEmpty());
                    expect (engine.isMonitorRunning());
                }
            }
        }

        beginTest ("ASIO routing survives Windows opens and a session applied before its ASIO device opens");
        for (int scenario : { 0, 1, 2 }) // device round trip, session load, live routing edits
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> ("ASIO"));
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> ("Windows Audio"));
            MixDocument document (engine);
            auto& s = document.getSession();
            s.device = { "ASIO", "Good", "Good", 256, 48000.0 };
            s.master.outputFirst = 2;
            s.channels[0].output = { true, true, 4 };
            s.channels[0].sends[0].amount = 0.5;
            s.fx[0].output = { false, true, 4 };
            expect (engine.openDevice (s.device).isEmpty());
            document.applyToEngine();
            const juce::TemporaryFile sessionFile (".livemix");
            expect (document.save (sessionFile.getFile()).wasOk());
            expect (engine.openDevice ({ "Windows Audio", "Capture", "", 256, 48000.0 }).isEmpty());
            if (scenario == 1) expect (document.load (sessionFile.getFile()).wasOk());
            // Live parameter edits must preserve requests too, while rendering remains limited to 1-2.
            if (scenario == 2)
            {
                engine.setMasterOutput (2);
                engine.setChannelOutput (s.channels[0].id, s.channels[0].output);
                engine.setFxOutput (s.fx[0].id, s.fx[0].output);
            }
            Io io;
            io.in.clear();
            io.setInput (0, 0.25f);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.625f, 1.0e-6f);
            expect (engine.openSessionDevice (s.device).isEmpty());
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (0), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (io.last (2), 0.25f, 1.0e-6f);
            expectWithinAbsoluteError (io.last (3), 0.25f, 1.0e-6f);
            expectWithinAbsoluteError (io.last (4), 0.375f, 1.0e-6f);
            expectWithinAbsoluteError (io.last (5), 0.375f, 1.0e-6f);
            expectEquals (s.master.outputFirst, 2);
            expectEquals (s.channels[0].output.directFirst, 4);
            expectEquals (s.fx[0].output.directFirst, 4);
        }

        beginTest ("Windows rendering limits master, channel direct and FX direct to outputs 1-2");
        for (const bool windows : { false, true })
        {
            MixEngine engine;
            MixSession s;
            if (windows) s.device.type = "Windows Audio";
            s.addChannel();
            s.addFx();
            s.master.outputFirst = 4;
            s.channels[0].output = { true, true, 4 };
            s.channels[0].sends[0].amount = 0.5;
            s.fx[0].output = { false, true, 4 };
            engine.applySession (s, nullptr, true);
            Io io;
            io.in.clear();
            io.setInput (0, 0.25f);
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (windows ? 0 : 4), 0.625f, 1.0e-6f);
            expectWithinAbsoluteError (io.last (windows ? 4 : 0), 0.0f, 1.0e-6f);
            engine.setMasterOutput (4);
            engine.setChannelOutput (s.channels[0].id, { true, true, 4 });
            engine.setFxOutput (s.fx[0].id, { false, true, 4 });
            render (engine, io, 3);
            expectWithinAbsoluteError (io.last (windows ? 0 : 4), 0.625f, 1.0e-6f);
            expectWithinAbsoluteError (io.last (windows ? 4 : 0), 0.0f, 1.0e-6f);
        }

        beginTest ("startup fallback, type ordering and buffer rollback use only the fake device manager");
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto& manager = engine.getDeviceManager();
            for (const auto* typeName : { "DirectSound", "Windows Audio (Exclusive Mode)", "Windows Audio (Low Latency Mode)", "Windows Audio", "ASIO" })
                manager.addAudioDeviceType (std::make_unique<MixFakeType> (typeName));
            expectEquals (AudioBackends::availableTypes (manager).joinIntoString ("|"),
                          juce::String ("ASIO|Windows Audio|Windows Audio (Low Latency Mode)|Windows Audio (Exclusive Mode)"));
            expectEquals (AudioBackends::label ("Windows Audio"), juce::String::fromUTF8 ("윈도우 오디오"));
            expect (! AudioBackends::sameContainer ("missing capture", "missing render"));
            const MixDevice missing { "ASIO", "Broken", "Broken", 256, 48000.0 };
            expect (engine.initialise (&missing).isEmpty());
            expectEquals (engine.getOpenDevice().input, juce::String ("Good"));
            expect (engine.setBufferSize (512).isEmpty());
            expectEquals (engine.getOpenDevice().bufferSize, 512);
            expect (engine.setBufferSize (1024).isNotEmpty());
            expectEquals (engine.getOpenDevice().bufferSize, 512);
            expect (engine.isDeviceRunning());
            engine.shutdown();
            for (auto* type : manager.getAvailableDeviceTypes())
                if (type->getTypeName() == "ASIO") { manager.removeAudioDeviceType (type); break; }
            expect (engine.initialise (nullptr).isEmpty());
            expectEquals (engine.getOpenDevice().type, juce::String ("Windows Audio"));
            expectEquals (engine.getOpenDevice().input, juce::String ("Capture"));
            expectEquals (engine.getOpenDevice().output, juce::String ("Headphones"));
        }

        beginTest ("lastDevice JSON round trip and one-time ASIO XML migration use an isolated settings folder");
        {
            const auto directory = juce::File::createTempFile ("-mix-device-settings");
            directory.deleteFile();
            directory.createDirectory();
            juce::PropertiesFile::Options options;
            options.applicationName = "LiveMix";
            options.filenameSuffix = "settings";
            options.folderName = directory.getFullPathName();
            options.storageFormat = juce::PropertiesFile::storeAsXML;
            {
                juce::PropertiesFile file (options);
                juce::XmlElement legacy ("DEVICESETUP");
                legacy.setAttribute ("audioInputDeviceName", "Old ASIO");
                legacy.setAttribute ("audioOutputDeviceName", "Old ASIO");
                legacy.setAttribute ("audioDeviceRate", 44100.0);
                legacy.setAttribute ("audioDeviceBufferSize", 512);
                file.setValue ("audioDeviceState", &legacy);
                file.saveIfNeeded();
            }
            {
                LiveMixSettings settings (directory);
                const auto saved = settings.getLastDevice();
                expect (saved.has_value());
                if (saved)
                {
                    expectEquals (saved->type, juce::String ("ASIO"));
                    expectEquals (saved->input, juce::String ("Old ASIO"));
                    expectEquals (saved->output, saved->input);
                    expectEquals (saved->bufferSize, 512);
                    expectWithinAbsoluteError (saved->sampleRate, 44100.0, 0.01);
                }
                settings.setLastDevice ({ "Windows Audio", juce::String::fromUTF8 ("마이크"), "", 480, 48000.0 });
                settings.saveIfNeeded();
            }
            {
                LiveMixSettings settings (directory);
                const auto saved = settings.getLastDevice();
                expect (saved.has_value());
                if (saved)
                {
                    expectEquals (saved->type, juce::String ("Windows Audio"));
                    expectEquals (saved->input, juce::String::fromUTF8 ("마이크"));
                    expect (saved->output.isEmpty());
                    expectEquals (saved->bufferSize, 480);
                }
            }
            directory.deleteRecursively();
        }
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

class LiveMixDeviceUiTests : public juce::UnitTest
{
public:
    LiveMixDeviceUiTests() : juce::UnitTest ("LiveMix device settings UI", "LiveMix") {}
    void runTest() override
    {
        beginTest ("device settings follow the running backend and apply input, monitor, rate and buffer immediately");
        const auto directory = juce::File::createTempFile ("-device-ui");
        expect (directory.deleteFile());
        expect (directory.createDirectory().wasOk());
        {
            LiveMixLookAndFeel lookAndFeel;
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            for (const auto* type : { "ASIO", "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" })
                engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> (type));
            expect (engine.openDevice ({ "ASIO", "Good", "Good", 256, 48000.0 }).isEmpty());
            LiveMixSettings settings (directory);
            int changes = 0;
            SettingsDialog::show (engine, settings, nullptr, [&] { ++changes; }, {}, {}, {}, {});
            juce::Component* content = nullptr;
            auto& desktop = juce::Desktop::getInstance();
            for (int i = 0; i < desktop.getNumComponents(); ++i)
                if (auto* dialog = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)); dialog != nullptr && dialog->getName() == ko ("설정"))
                    if (auto* viewport = dynamic_cast<juce::Viewport*> (dialog->getContentComponent())) content = viewport->getViewedComponent();
            expect (content != nullptr);
            if (content != nullptr)
            {
                content->setLookAndFeel (&lookAndFeel);
                auto combo = [&] (const char* id) { return dynamic_cast<juce::ComboBox*> (content->findChildWithID (id)); };
                auto* type = combo ("device-type");
                auto* input = combo ("device-input");
                auto* output = combo ("device-output");
                auto* rate = combo ("device-rate");
                auto* buffer = combo ("device-buffer");
                expect (type != nullptr && input != nullptr && output != nullptr && rate != nullptr && buffer != nullptr);
                if (type != nullptr && input != nullptr && output != nullptr && rate != nullptr && buffer != nullptr)
                {
                    expectEquals (type->getNumItems(), 4);
                    expectEquals (input->getText(), juce::String ("Good"));
                    expect (! output->isVisible() && ! rate->isVisible() && buffer->isVisible());
                    for (int mode = 1; mode <= 4; ++mode)
                    {
                        if (mode > 1) type->setSelectedId (mode, juce::sendNotificationSync);
                        const auto actual = engine.getOpenDevice();
                        expectEquals (type->getText(), AudioBackends::label (actual.type));
                        expectEquals (input->getText(), actual.input);
                        expectEquals (rate->getSelectedId(), (int) actual.sampleRate);
                        expectEquals (buffer->getSelectedId(), actual.bufferSize);
                        expect (output->isVisible() == (mode != 1));
                        expect (rate->isVisible() == (mode != 1));
                        expect (buffer->isVisible() == (mode != 2));
                        for (auto* child : content->getChildren())
                            if (child->isVisible()) expect (content->getLocalBounds().contains (child->getBounds()));
                        const auto folder = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_UI_SCREENSHOT_DIR", {});
                        if (folder.isNotEmpty())
                        {
                            const juce::File shots (folder);
                            expect (shots.createDirectory().wasOk());
                            juce::FileOutputStream image (shots.getChildFile ("settings-" + juce::String (mode) + ".png"));
                            if (image.openedOk())
                            {
                                expect (image.setPosition (0));
                                expect (image.truncate().wasOk());
                                expect (juce::PNGImageFormat().writeImageToStream (content->createComponentSnapshot (content->getLocalBounds()), image));
                            }
                        }
                    }
                    input->setSelectedId (2, juce::sendNotificationSync);
                    expectEquals (engine.getOpenDevice().input, juce::String ("Capture 2"));
                    expectEquals (engine.getOpenDevice().output, juce::String ("Headphones"));
                    output->setSelectedId (1, juce::sendNotificationSync);
                    expect (engine.getOpenDevice().output.isEmpty());
                    expectEquals (output->getText(), ko ("없음 (OBS로만 보내기)"));
                    rate->setSelectedId (44100, juce::sendNotificationSync);
                    expectEquals ((int) engine.getOpenDevice().sampleRate, 44100);
                    buffer->setSelectedId (512, juce::sendNotificationSync);
                    expectEquals (engine.getOpenDevice().bufferSize, 512);
                    expectEquals (changes, 7);
                    type->setSelectedId (1, juce::sendNotificationSync);
                    expectEquals (engine.getOpenDevice().type, juce::String ("ASIO"));
                    expectEquals (input->getText(), juce::String ("Good"));
                    expect (! output->isVisible());
                }
                content->setLookAndFeel (nullptr);
            }
            SettingsDialog::closeIfOpen();

            beginTest ("top bar uses a short backend caption and lists the supplied capture endpoints");
            MixDocument document (engine);
            TopBar bar (document);
            bar.setLookAndFeel (&lookAndFeel);
            bar.setSize (700, bar.preferredHeight (700));
            bar.setDevices ({ "Capture", "Capture 2" }, "Capture 2", "Windows Audio (Low Latency Mode)");
            bool captionFound = false, selected = false;
            for (auto* child : bar.getChildren())
            {
                if (auto* label = dynamic_cast<juce::Label*> (child))
                    captionFound = captionFound || (label->getText() == ko ("윈도우") && label->getTooltip() == ko ("윈도우 오디오 (저지연)"));
                if (auto* box = dynamic_cast<juce::ComboBox*> (child)) selected = box->getNumItems() == 2 && box->getText() == "Capture 2";
            }
            expect (captionFound && selected);
            bar.setLookAndFeel (nullptr);
        }
        expect (directory.deleteRecursively());
    }
};
static LiveMixDeviceUiTests liveMixDeviceUiTests;

} // namespace gocue::tests
