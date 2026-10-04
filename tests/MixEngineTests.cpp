#include "MixEngine.h"
#include "AudioBackends.h"
#include "LiveMixSettings.h"
#include "TestGainPlugin.h"
#include "ui/SettingsDialog.h"
#include "ui/DeviceFormatText.h"
#include "ui/TopBar.h"
#include "ui/LiveMixLookAndFeel.h"
#include "ui/ChannelCard.h"
#include "ui/FxDrawer.h"
#include "lm_obs_protocol.h"
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
        juce::Array<double> rates { 48000.0, 44100.0 };
        double refusedRate = 0.0;   // listed, but open() keeps running at the rate it had (an ASIO driver on an external clock)
        double failedRate = 0.0;    // listed, but open() fails at it
        int buffer = 256;
        int asioOutputs = 72;
        int bits = 32;
        int createdBits = 0, openedBits = 0;
        bool createdFloat = false, openedFloat = false;
        juce::AudioIODeviceCallback* callback = nullptr;
        std::function<void()> onOutputLifecycle;
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
                for (int i = 0; i < (getTypeName().contains ("ASIO") ? (input ? 72 : record->asioOutputs) : 8); ++i) names.add (juce::String (i + 1));
            return names;
        }
        juce::StringArray getInputChannelNames() override { return channelNames (true); }
        juce::StringArray getOutputChannelNames() override { return channelNames (false); }
        juce::Array<double> getAvailableSampleRates() override { return record->rates; }
        juce::Array<int> getAvailableBufferSizes() override { return { 128, 256, 512, 1024 }; }
        int getDefaultBufferSize() override { return 256; }
        juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double sr, int bs) override
        {
            juce::getWasapiExclusivePreferredFormat (record->openedBits, record->openedFloat);
            close();
            if (record->onOutputLifecycle) record->onOutputLifecycle();
            if (record->input == "Broken" || record->output == "Broken" || bs == 1024
                || (record->failedRate > 0.0 && juce::approximatelyEqual (sr, record->failedRate)))
                return "deliberate fake open failure";
            record->inputs = ins;
            record->outputs = outs;
            record->inputs.setRange (getInputChannelNames().size(), 128, false);
            record->outputs.setRange (getOutputChannelNames().size(), 128, false);
            if (record->refusedRate <= 0.0 || ! juce::approximatelyEqual (sr, record->refusedRate))
                record->rate = sr;
            record->buffer = bs;
            opened = true;
            return {};
        }
        void close() override { stop(); opened = false; }
        bool isOpen() override { return opened; }
        void start (juce::AudioIODeviceCallback* cb) override
        {
            if (record->onOutputLifecycle) record->onOutputLifecycle();
            callback = cb;
            record->callback = cb;
            record->playing = cb != nullptr && opened;
            if (cb != nullptr) cb->audioDeviceAboutToStart (this);
        }
        void stop() override
        {
            if (record->onOutputLifecycle) record->onOutputLifecycle();
            if (callback != nullptr) callback->audioDeviceStopped();
            callback = nullptr;
            record->callback = nullptr;
            record->playing = false;
        }
        bool isPlaying() override { return record->playing; }
        juce::String getLastError() override { return {}; }
        int getCurrentBufferSizeSamples() override { return record->buffer; }
        double getCurrentSampleRate() override { return record->rate; }
        int getCurrentBitDepth() override { return record->bits; }
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
        explicit MixFakeType (const juce::String& name, int outputs = 72) : AudioIODeviceType (name), asioOutputs (outputs) {}
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
            juce::getWasapiExclusivePreferredFormat (r->createdBits, r->createdFloat);
            r->input = input;
            r->output = output;
            r->asioOutputs = asioOutputs;
            r->rates = rates;
            r->refusedRate = refusedRate;
            r->failedRate = failedRate;
            if (refusedRate > 0.0 && ! records.empty()) r->rate = records.back()->rate;   // the driver keeps its clock across reopens
            if (input.isEmpty() && output.isNotEmpty()) r->onOutputLifecycle = onOutputLifecycle;
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
        juce::Array<double> rates { 48000.0, 44100.0 };
        double refusedRate = 0.0, failedRate = 0.0;
        bool failOutputs = false;
        int asioOutputs = 72;
        std::function<void()> onOutputLifecycle;
    };

    // The same private-method access pattern as the UI transition tests: run one maintenance tick
    // at a precise callback checkpoint without sleeping or adding a production test hook.
    struct PollMonitorMaintenance
    {
        using Type = void (MixEngine::*)();
        friend Type member (PollMonitorMaintenance);
    };
    template <typename Tag, typename Tag::Type method>
    struct MixPrivateMethod { friend typename Tag::Type member (Tag) { return method; } };
    template struct MixPrivateMethod<PollMonitorMaintenance, &MixEngine::timerCallback>;

    void removeRealMixDeviceTypes (MixEngine& engine)
    {
        auto& manager = engine.getDeviceManager();
        while (! manager.getAvailableDeviceTypes().isEmpty())
            manager.removeAudioDeviceType (manager.getAvailableDeviceTypes().getLast());
    }

    struct ScopedWasapiPreference
    {
        ScopedWasapiPreference() { juce::getWasapiExclusivePreferredFormat (bits, isFloat); }
        ~ScopedWasapiPreference() { juce::setWasapiExclusivePreferredFormat (bits, isFloat); }
        int bits = 0;
        bool isFloat = false;
    };
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
        runBitDepthChoiceTests();
        runDeviceFormatTests();
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
                    for (auto* child : main.getChildren())
                        if (auto* master = dynamic_cast<MasterCard*> (child))
                            for (int width : { 364, 388, 768, 936, 988, 1400, 1408, 1920 })
                                for (int state = 0; state <= (int) MasterCard::ObsStatus::portableObs; ++state)
                                    for (int height : { 480, 1100 })
                                    {
                                        master->setObsStatus ((MasterCard::ObsStatus) state);
                                        main.setSize (width + 32, height);
                                        main.resized();
                                        int masterWidth = width;
                                        for (auto* other : main.getChildren())
                                            if (auto* viewport = dynamic_cast<juce::Viewport*> (other); viewport != nullptr && viewport->isVisible() && width + 32 >= 800)
                                                masterWidth = width - viewport->getScrollBarThickness();
                                        expectEquals (master->getWidth(), masterWidth);
                                        expectEquals (master->getHeight(), master->getPreferredHeight (masterWidth));
                                        expectEquals (master->getBottom(), height - 38);   // l06: 8 px above the 30 px status bar
                                        expect (main.getLocalBounds().contains (master->getBounds()));
                                        for (auto* other : main.getChildren())
                                            if (auto* viewport = dynamic_cast<juce::Viewport*> (other); viewport != nullptr && viewport->isVisible())
                                            {
                                                expect (! viewport->getBounds().intersects (master->getBounds()));
                                                expectEquals (master->getY() - viewport->getBottom(), 20);
                                            }
                                    }
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

    void runBitDepthChoiceTests()
    {
        const ScopedWasapiPreference preference;
        auto expectPreference = [this] (int expectedBits, bool expectedFloat)
        {
            int bits = -1;
            bool isFloat = false;
            juce::getWasapiExclusivePreferredFormat (bits, isFloat);
            expectEquals (bits, expectedBits);
            expect (isFloat == expectedFloat);
        };
        beginTest ("bit depth preference reaches both devices before create/open and survives buffer changes and restart");
        for (const auto* choice : { "", "int16", "int24", "int32", "float32" })
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto type = std::make_unique<MixFakeType> ("Windows Audio (Exclusive Mode)");
            auto* fake = type.get();
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const juce::String format (choice);
            const int bits = format.isEmpty() ? 0 : format == "int16" ? 16 : format == "int24" ? 24 : 32;
            const bool isFloat = format == "float32";
            juce::setWasapiExclusivePreferredFormat (20, false);
            expect (engine.openDevice ({ fake->getTypeName(), "Capture", "Headphones", 256, 48000.0, format }).isEmpty());
            expectPreference (bits, isFloat);
            expectEquals (engine.getOpenDevice().sampleFormat, format);
            expect (engine.isSplitMonitor() && engine.isMonitorRunning());
            expect (engine.setBufferSize (512).isEmpty());
            expectEquals (engine.getOpenDevice().bufferSize, 512);
            expectEquals (engine.getOpenDevice().sampleFormat, format);
            expectPreference (bits, isFloat);
            expect (engine.restartDevice().isEmpty());
            expectEquals (engine.getOpenDevice().sampleFormat, format);
            expectPreference (bits, isFloat);
            for (const auto& record : fake->records)
            {
                expectEquals (record->createdBits, bits);
                expectEquals (record->openedBits, bits);
                expect (record->createdFloat == isFloat && record->openedFloat == isFloat);
            }
        }

        beginTest ("failed input and split-output opens restore the previous bit depth before rollback");
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto type = std::make_unique<MixFakeType> ("Windows Audio (Exclusive Mode)");
            auto* fake = type.get();
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const MixDevice previous { fake->getTypeName(), "Capture", "Headphones", 256, 48000.0, "int16" };
            expect (engine.openDevice (previous).isEmpty());
            for (bool failInput : { true, false })
            {
                auto wanted = previous;
                wanted.sampleFormat = "float32";
                if (failInput) wanted.input = "Broken"; else wanted.output = "Broken";
                expect (engine.openDevice (wanted).isNotEmpty());
                expectPreference (16, false);
                const auto restored = engine.getOpenDevice();
                expectEquals (restored.sampleFormat, previous.sampleFormat);
                expectEquals (restored.type, previous.type);
                expectEquals (restored.input, previous.input);
                expectEquals (restored.output, previous.output);
                expect (engine.isDeviceRunning() && engine.isMonitorRunning());
                for (const auto& record : fake->records)
                    if (record->playing)
                    {
                        expectEquals (record->createdBits, 16);
                        expectEquals (record->openedBits, 16);
                        expect (! record->createdFloat && ! record->openedFloat);
                    }
            }
        }

        beginTest ("a session choice alone reopens exclusive, but is only recorded for ASIO/shared/low latency");
        for (const auto* typeName : { "ASIO", "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" })
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto type = std::make_unique<MixFakeType> (typeName);
            auto* fake = type.get();
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            MixDevice wanted { typeName, juce::String (typeName) == "ASIO" ? "Good" : "Capture", "", 256, 48000.0, "int16" };
            expect (engine.openDevice (wanted).isEmpty());
            const auto before = fake->records.size();
            wanted.sampleFormat = "int24";
            expect (engine.openSessionDevice (wanted).isEmpty());
            expect ((fake->records.size() > before) == (juce::String (typeName) == "Windows Audio (Exclusive Mode)"));
            expectEquals (engine.getOpenDevice().sampleFormat, juce::String ("int24"));
            expectPreference (24, false);
            const auto opened = fake->records.size();
            expect (engine.openSessionDevice (wanted).isEmpty());
            expect (fake->records.size() == opened);
            expect (engine.restartDevice().isEmpty());
            expectEquals (engine.getOpenDevice().sampleFormat, juce::String ("int24"));
            expectPreference (24, false);
        }

        beginTest ("startup uses the saved bit depth and both ASIO and Windows fallbacks reset it to automatic");
        for (bool asioFallback : { true, false })
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto& manager = engine.getDeviceManager();
            manager.addAudioDeviceType (std::make_unique<MixFakeType> ("Windows Audio (Exclusive Mode)"));
            manager.addAudioDeviceType (std::make_unique<MixFakeType> (asioFallback ? "ASIO" : "Windows Audio"));
            MixDevice saved { "Windows Audio (Exclusive Mode)", "Capture", "", 256, 48000.0, "float32" };
            expect (engine.initialise (&saved).isEmpty());
            expectEquals (engine.getOpenDevice().sampleFormat, saved.sampleFormat);
            expectPreference (32, true);
            engine.shutdown();
            saved.input = "Broken";
            expect (engine.initialise (&saved).isEmpty());
            expectEquals (engine.getOpenDevice().type, juce::String (asioFallback ? "ASIO" : "Windows Audio"));
            expect (engine.getOpenDevice().sampleFormat.isEmpty());
            expectPreference (0, false);
        }
    }

    void runDeviceFormatTests()
    {
        using Format = MixEngine::DeviceFormat;
        using Kind = Format::Kind;
        using Info = juce::WasapiFormatInfo;
        beginTest ("device format mapping distinguishes Windows settings, exclusive streams and ASIO driver bits");
        {
            Info input { 32, true, 16, false, 44100.0, Info::exclusiveInt16 };
            Info output { 32, true, 24, false, 48000.0, Info::exclusiveInt16 | Info::exclusiveInt24 };
            auto format = MixEngine::describeDeviceFormat (Kind::none, input, output, "int24", 24);
            expect (format.kind == Kind::none);
            expectEquals (format.inputBits, 0);
            expectEquals (format.outputBits, 0);
            format = MixEngine::describeDeviceFormat (Kind::asio, input, output, "float32", 24);
            expect (format.kind == Kind::asio);
            expectEquals (format.inputBits, 24);
            expectEquals (format.outputBits, 24);
            expect (! format.inputFloat && ! format.outputFloat);
            expectEquals (format.inputAccepted | format.outputAccepted, 0);
            expect (! format.inputRefused && ! format.outputRefused);

            format = MixEngine::describeDeviceFormat (Kind::windowsShared, input, output, "int24");
            expectEquals (format.inputBits, 16);
            expectEquals (format.outputBits, 24);
            expect (! format.inputFloat && ! format.outputFloat);
            expectEquals (format.inputDeviceRate, 44100.0);
            expectEquals (format.outputDeviceRate, 48000.0);
            expectEquals (format.inputAccepted | format.outputAccepted, 0);
            expect (! format.inputRefused && ! format.outputRefused);
            input.deviceBits = 32;
            input.deviceIsFloat = true;
            format = MixEngine::describeDeviceFormat (Kind::windowsShared, input, output, "");
            expect (format.inputFloat && format.inputBits == 32);
            input.deviceBits = 0;
            input.deviceSampleRate = 0;
            output.streamBits = 0; // a closed direction must not report stale endpoint properties
            format = MixEngine::describeDeviceFormat (Kind::windowsShared, input, output, "");
            expectEquals (format.inputBits, 0);
            expectEquals (format.outputBits, 0);
            expectEquals (format.inputDeviceRate, 0.0);
            expectEquals (format.outputDeviceRate, 0.0);
        }

        beginTest ("exclusive accepted masks and refusal use each direction's actual stream, including a separate monitor");
        {
            Info input { 16, false, 32, true, 44100.0, Info::exclusiveInt16 };
            Info monitorOutput { 24, false, 16, false, 48000.0, Info::exclusiveInt16 | Info::exclusiveInt24 };
            auto format = MixEngine::describeDeviceFormat (Kind::windowsExclusive, input, monitorOutput, "int24");
            expectEquals (format.inputBits, 16);
            expectEquals (format.outputBits, 24);
            expectEquals (format.inputAccepted, (int) Info::exclusiveInt16);
            expectEquals (format.outputAccepted, (int) (Info::exclusiveInt16 | Info::exclusiveInt24));
            expect (format.inputRefused && ! format.outputRefused);
            expectEquals (format.inputDeviceRate, 0.0);
            expectEquals (format.outputDeviceRate, 0.0);
            format = MixEngine::describeDeviceFormat (Kind::windowsExclusive, input, monitorOutput, "");
            expect (! format.inputRefused && ! format.outputRefused);
            format = MixEngine::describeDeviceFormat (Kind::windowsExclusive, input, monitorOutput, "int32");
            expect (format.inputRefused && format.outputRefused);
            input.streamBits = monitorOutput.streamBits = 32;
            input.streamIsFloat = true;
            input.exclusiveFormats = Info::exclusiveFloat32;
            monitorOutput.exclusiveFormats = Info::exclusiveInt32;
            format = MixEngine::describeDeviceFormat (Kind::windowsExclusive, input, monitorOutput, "float32");
            expect (format.inputFloat && ! format.outputFloat);
            expect (! format.inputRefused && format.outputRefused);
            expectEquals (format.inputAccepted, (int) Info::exclusiveFloat32);
            expectEquals (format.outputAccepted, (int) Info::exclusiveInt32);
            format = MixEngine::describeDeviceFormat (Kind::windowsExclusive, input, {}, "int32");
            expect (format.inputRefused && ! format.outputRefused);
            expectEquals (format.outputBits, 0);
            expectEquals (format.outputAccepted, 0);
        }

        beginTest ("running fake formats stay unknown in shared mode, use driver bits in exclusive/ASIO and use the split output's device");
        const ScopedWasapiPreference preference;
        for (const auto* typeName : { "ASIO", "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" })
        {
            MixEngine engine;
            expect (engine.getDeviceFormat().kind == Kind::none);
            removeRealMixDeviceTypes (engine);
            auto type = std::make_unique<MixFakeType> (typeName);
            auto* fake = type.get();
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const bool asio = juce::String (typeName) == "ASIO";
            const bool exclusive = juce::String (typeName) == "Windows Audio (Exclusive Mode)";
            expect (engine.openDevice ({ typeName, asio ? "Good" : "Capture", asio ? "Good" : "Headphones", 256, 48000.0, "int32" }).isEmpty());
            for (auto& record : fake->records)
            {
                record->bits = record->input.isNotEmpty() ? 16 : 24;
                record->rate = record->input.isNotEmpty() ? 48000.0 : 44100.0;
            }
            const auto format = engine.getDeviceFormat();
            expect (format.kind == (asio ? Kind::asio : exclusive ? Kind::windowsExclusive : Kind::windowsShared));
            expectEquals (format.inputBits, asio || exclusive ? 16 : 0);
            expectEquals (format.outputBits, asio ? 16 : exclusive ? 24 : 0);
            expect (! format.inputFloat && ! format.outputFloat);
            expect (! format.inputRefused && ! format.outputRefused);
            expectEquals (format.inputAccepted | format.outputAccepted, 0);
            expectEquals (format.inputDeviceRate, 0.0);
            expectEquals (format.outputDeviceRate, 0.0);
            expectEquals (format.inputStreamRate, 48000.0);
            expectEquals (format.outputStreamRate, asio ? 48000.0 : 44100.0);
            if (! asio)
            {
                auto wanted = engine.getOpenDevice();
                wanted.output.clear();
                expect (engine.openDevice (wanted).isEmpty());
                expectEquals (engine.getDeviceFormat().outputBits, 0);
                expectEquals (engine.getDeviceFormat().outputStreamRate, 0.0);
                // Drive the same-device duplex path without relying on this PC's endpoint container ids.
                auto duplex = engine.getDeviceManager().getAudioDeviceSetup();
                duplex.outputDeviceName = "Headphones";
                duplex.useDefaultOutputChannels = false;
                duplex.outputChannels.setRange (0, 2, true);
                expect (engine.getDeviceManager().setAudioDeviceSetup (duplex, true).isEmpty());
                expect (! engine.isSplitMonitor() && engine.isMonitorRunning());
                auto running = fake->playingOutput();
                expect (running != nullptr);
                if (running != nullptr)
                {
                    running->rate = 96000.0;
                    expectEquals (engine.getDeviceFormat().inputStreamRate, 96000.0);
                    expectEquals (engine.getDeviceFormat().outputStreamRate, 96000.0);
                }
            }
            engine.shutdown();
            expect (engine.getDeviceFormat().kind == Kind::none);
            expectEquals (engine.getDeviceFormat().inputBits, 0);
            expectEquals (engine.getDeviceFormat().outputBits, 0);
            expectEquals (engine.getDeviceFormat().inputStreamRate, 0.0);
            expectEquals (engine.getDeviceFormat().outputStreamRate, 0.0);
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

class LiveMixRound9Tests : public juce::UnitTest
{
public:
    LiveMixRound9Tests() : juce::UnitTest ("LiveMix Round 9 routing and monitor restarts", "LiveMix") {}

    static void dispatchMessages()
    {
        MSG message;
        while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&message);
            DispatchMessageW (&message);
        }
    }

    static juce::Button* button (juce::Component& component, const juce::String& text)
    {
        for (auto* child : component.getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && b->getButtonText() == text) return b;
        return nullptr;
    }

    static juce::ComboBox* outputCombo (juce::Component& component)
    {
        juce::ComboBox* result = nullptr;
        for (auto* child : component.getChildren())
            if (auto* combo = dynamic_cast<juce::ComboBox*> (child)) result = combo;
        return result;
    }

    void click (juce::Button& b)
    {
        b.triggerClick(); // JUCE dispatches the real button's toggle and onClick asynchronously.
        dispatchMessages();
    }

    void runTest() override
    {
        beginTest ("N1: real channel/FX toggle clicks preserve 3-4/5-6 through Windows and back to ASIO");
        for (const auto* typeName : { "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" })
        for (bool hasOutput : { true, false })
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> ("ASIO"));
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> (typeName));
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> ("ASIO Stereo", 2));
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> ("ASIO NoOut", 0));
            MixDocument document (engine);
            auto& session = document.getSession();
            session.device = { "ASIO", "Good", "Good", 256, 48000.0 };
            session.channels[0].output = { false, true, 2 };
            session.channels[0].sends[0].amount = 0.5;
            session.fx[0].output = { false, true, 4 };
            document.applyToEngine();
            expect (engine.openDevice (session.device).isEmpty());
            ChannelCard card (document, session.channels[0].id);
            FxDrawer drawer (document);
            document.onValueChanged = [&] { card.refresh(); drawer.refresh(); };
            auto refreshDevices = [&]
            {
                auto* device = engine.getDeviceManager().getCurrentAudioDevice();
                const auto names = engine.isSplitMonitor() ? juce::StringArray { "1", "2" } : device->getOutputChannelNames();
                card.setDeviceChannels (device->getInputChannelNames(), names);
                drawer.setDeviceChannels (names);
            };
            refreshDevices();
            expect (engine.openDevice ({ typeName, "Capture", hasOutput ? "Headphones" : "", 256, 48000.0 }).isEmpty());
            refreshDevices(); // Saved device is still ASIO: the controls must use the RUNNING backend.
            for (auto* component : std::initializer_list<juce::Component*> { &card, &drawer })
            {
                const bool channel = component == &card;
                const int pair = channel ? 2 : 4;
                auto read = [&] { return channel ? session.channels[0].output : session.fx[0].output; };
                auto* master = button (*component, ko ("마스터"));
                auto* direct = button (*component, ko ("직접 출력"));
                auto* combo = outputCombo (*component);
                expect (master != nullptr && direct != nullptr && combo != nullptr);
                if (master == nullptr || direct == nullptr || combo == nullptr) continue;
                expectEquals (combo->getSelectedId(), 1);
                for (int i = 0; i < 2; ++i)
                {
                    click (*master);
                    expect (read().master == (i == 0));
                    expect (read().direct);
                    expectEquals (read().directFirst, pair);
                }
                expect (direct->isEnabled() == hasOutput);
                for (int i = 0; i < 2; ++i)
                {
                    click (*direct);
                    expect (read().direct == (! hasOutput || i == 1)); // no-output control stays disabled
                    expect (! read().master);
                    expectEquals (read().directFirst, pair);
                }
                // A notification from the one effective Windows pair also cannot become a saved pair edit.
                combo->setSelectedId (0, juce::dontSendNotification);
                combo->setSelectedId (1, juce::sendNotificationSync);
                expectEquals (read().directFirst, pair);
            }
            expect (engine.openDevice (session.device).isEmpty());
            refreshDevices();
            expectEquals (session.channels[0].output.directFirst, 2);
            expectEquals (session.fx[0].output.directFirst, 4);
            MixEngineTests::Io io;
            io.in.clear();
            io.setInput (0, 0.25f);
            MixEngineTests::render (engine, io, 3);
            for (int out = 0; out < MixEngineTests::numOuts; ++out)
                expectWithinAbsoluteError (io.last (out), out < 2 ? 0.0f : out < 4 ? 0.25f : 0.125f, 1.0e-6f);

            // An explicit selection on a running multi-pair ASIO device is the one action that changes the pair.
            outputCombo (card)->setSelectedId (5, juce::sendNotificationSync);
            outputCombo (drawer)->setSelectedId (3, juce::sendNotificationSync);
            expectEquals (session.channels[0].output.directFirst, 4);
            expectEquals (session.fx[0].output.directFirst, 2);
            expect (engine.openDevice ({ "ASIO Stereo", "Good", "Good", 256, 48000.0 }).isEmpty());
            refreshDevices();
            expectEquals (engine.getNumDeviceOutputs(), 2);
            for (auto* component : std::initializer_list<juce::Component*> { &card, &drawer })
            {
                // the saved 5-6 / 3-4 do not exist on this 2-output device (the combo shows 없음): the operator picks 1-2
                outputCombo (*component)->setSelectedId (1, juce::sendNotificationSync);
                click (*button (*component, ko ("마스터")));
                click (*button (*component, ko ("마스터")));
            }
            // an explicit pick on a running ASIO device is stored even when it is the device's only pair (Astra R10:
            // ignoring it left a direct-only channel silent on a 2-output interface)
            expectEquals (session.channels[0].output.directFirst, 0);
            expectEquals (session.fx[0].output.directFirst, 0);
            // an output-less ASIO device lists placeholder pairs: picking one stores nothing (Astra R11)
            expect (engine.openDevice ({ "ASIO NoOut", "Good", "Good", 256, 48000.0 }).isEmpty());
            refreshDevices();
            expectEquals (engine.getNumDeviceOutputs(), 0);
            outputCombo (card)->setSelectedId (3, juce::sendNotificationSync);
            outputCombo (drawer)->setSelectedId (5, juce::sendNotificationSync);
            expectEquals (session.channels[0].output.directFirst, 0);
            expectEquals (session.fx[0].output.directFirst, 0);
            engine.shutdown();
            card.setDeviceChannels ({ "1", "2" }, { "1", "2", "3", "4", "5", "6" });
            drawer.setDeviceChannels ({ "1", "2", "3", "4", "5", "6" });
            outputCombo (card)->setSelectedId (3, juce::sendNotificationSync);
            outputCombo (drawer)->setSelectedId (5, juce::sendNotificationSync);
            // no device runs: a combo change stores nothing
            expectEquals (session.channels[0].output.directFirst, 0);
            expectEquals (session.fx[0].output.directFirst, 0);
        }

        beginTest ("N2: output-only format/period rebuild keeps OBS epoch, every input block and plugin prepares");
        {
            const auto mapping = "Local\\LiveMix.ObsTest.Round9." + juce::Uuid().toString();
            MixEngine engine (mapping);
            removeRealMixDeviceTypes (engine);
            auto type = std::make_unique<MixFakeType> ("Windows Audio");
            auto* fake = type.get();
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            expect (engine.openDevice ({ "Windows Audio", "Capture", "Headphones", 256, 48000.0 }).isEmpty());
            std::shared_ptr<MixDeviceRecord> input;
            for (const auto& record : fake->records)
                if (record->device == engine.getDeviceManager().getCurrentAudioDevice()) input = record;
            expect (input != nullptr && input->callback != nullptr);
            if (input == nullptr || input->callback == nullptr) return;
            MixSession session;
            session.addChannel();
            session.addFx();
            session.channels[0].sends[0].amount = 0.5;
            session.fx[0].output = { false, false, 4 };
            engine.applySession (session);
            struct ProbePlugin : TestGainPlugin
            {
                ProbePlugin() : TestGainPlugin (1.0f) {}
                void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
                {
                    if (checkpoint) checkpoint();
                    TestGainPlugin::processBlock (buffer, midi);
                }
                std::function<void()> checkpoint;
            };
            std::array<ProbePlugin*, 3> plugins {};
            int index = 0;
            for (auto* chain : { engine.getChannelChain (session.channels[0].id), engine.getFxChain (session.fx[0].id), &engine.getMasterChain() })
            {
                auto plugin = std::make_unique<ProbePlugin>();
                plugins[(size_t) index++] = plugin.get();
                chain->addPlugin (std::move (plugin));
            }
            engine.getObsSender().setEnabled (true);
            struct Ring
            {
                HANDLE handle = nullptr;
                lm_obs_ring_header* header = nullptr;
                ~Ring() { if (header != nullptr) UnmapViewOfFile (header); if (handle != nullptr) CloseHandle (handle); }
            } ring;
            ring.handle = OpenFileMappingW (FILE_MAP_READ, FALSE, mapping.toWideCharPointer());
            if (ring.handle != nullptr) ring.header = static_cast<lm_obs_ring_header*> (MapViewOfFile (ring.handle, FILE_MAP_READ, 0, 0, 0));
            expect (ring.header != nullptr);
            if (ring.header == nullptr) return;
            std::array<float, 1024> samples;
            samples.fill (0.25f);
            const float* inputs[] { samples.data() };
            auto inputBlock = [&] { input->callback->audioDeviceIOCallbackWithContext (inputs, 1, nullptr, 0, input->buffer, {}); };
            for (int i = 0; i < 32; ++i) inputBlock();
            auto expectedEpoch = lm_obs_load_acquire (&ring.header->epoch);
            std::array<int, 3> prepares {}, releases {};
            for (size_t i = 0; i < plugins.size(); ++i)
            {
                prepares[i] = plugins[i]->prepareCount;
                releases[i] = plugins[i]->releaseCount;
            }
            int inputBlocks = 0, lifecycleCalls = 0, lostBlocks = 0, changedEpochs = 0, damagedBlocks = 0;
            auto probeInput = [&]
            {
                const auto before = lm_obs_load_acquire (&ring.header->write_frames);
                inputBlock();
                ++inputBlocks;
                if (lm_obs_load_acquire (&ring.header->write_frames) != before + input->buffer) ++lostBlocks;
                else
                {
                    const auto* pcm = reinterpret_cast<const float*> (reinterpret_cast<const char*> (ring.header) + ring.header->data_offset);
                    for (int i = 0; i < input->buffer; ++i)
                    {
                        const auto offset = (size_t) ((before + i) % LM_OBS_CAPACITY_FRAMES) * 2;
                        if (std::abs (pcm[offset] - 0.25f) > 1.0e-5f || std::abs (pcm[offset + 1] - 0.25f) > 1.0e-5f)
                        {
                            ++damagedBlocks;
                            break;
                        }
                    }
                }
                if (lm_obs_load_acquire (&ring.header->epoch) != expectedEpoch) ++changedEpochs;
            };
            fake->onOutputLifecycle = [&] { ++lifecycleCalls; probeInput(); };
            for (const auto& record : fake->records)
                if (record->input.isEmpty() && record->output.isNotEmpty()) record->onOutputLifecycle = fake->onOutputLifecycle;
            auto recover = [&]
            {
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 1500.0;
                while (! engine.isMonitorRunning() && juce::Time::getMillisecondCounterHiRes() < deadline)
                {
                    probeInput();
                    dispatchMessages();
                    juce::Thread::sleep (2);
                }
                expect (engine.isMonitorRunning());
            };
            auto checkMonitorAudio = [&]
            {
                const auto output = fake->playingOutput();
                expect (output != nullptr && output->callback != nullptr);
                if (output == nullptr || output->callback == nullptr) return;
                juce::AudioBuffer<float> audio (2, output->buffer);
                double nextInput = 0.0;
                for (int block = 0; block < 120; ++block)
                {
                    const double time = block * output->buffer / output->rate;
                    while (nextInput <= time)
                    {
                        probeInput();
                        nextInput += input->buffer / input->rate;
                    }
                    output->callback->audioDeviceIOCallbackWithContext (nullptr, 0, audio.getArrayOfWritePointers(), 2, output->buffer, {});
                }
                expectWithinAbsoluteError (audio.getSample (0, output->buffer - 1), 0.25f, 1.0e-5f);
                expectWithinAbsoluteError (audio.getSample (1, output->buffer - 1), 0.25f, 1.0e-5f);
            };
            for (const auto format : { std::pair<double, int> { 48000.0, 512 }, { 44100.0, 512 } })
            {
                const auto oldOutput = fake->playingOutput();
                const auto before = lm_obs_load_acquire (&ring.header->write_frames);
                const int blocksBefore = inputBlocks, callsBefore = lifecycleCalls;
                oldOutput->callback->audioDeviceStopped();
                oldOutput->rate = format.first;
                oldOutput->buffer = format.second;
                oldOutput->callback->audioDeviceAboutToStart (oldOutput->device);
                expect (! engine.isMonitorRunning());
                recover();
                const auto output = fake->playingOutput();
                expect (output != nullptr && output != oldOutput);
                if (output != nullptr)
                {
                    expectEquals (output->buffer, format.second);
                    expectEquals (output->rate, format.first);
                }
                expectEquals (lm_obs_load_acquire (&ring.header->epoch), expectedEpoch);
                expectGreaterThan (lifecycleCalls, callsBefore);
                expectEquals (lm_obs_load_acquire (&ring.header->write_frames), before + int64_t (inputBlocks - blocksBefore) * input->buffer);
                for (size_t i = 0; i < plugins.size(); ++i)
                {
                    expectEquals (plugins[i]->prepareCount, prepares[i]);
                    expectEquals (plugins[i]->releaseCount, releases[i]);
                }
                checkMonitorAudio();
            }
            expectEquals (lostBlocks, 0);
            expectEquals (changedEpochs, 0);
            expectEquals (damagedBlocks, 0);

            beginTest ("N2: monitor retirement waits for the graph's last reference without detaching or waiting");
            const auto retiring = fake->playingOutput();
            retiring->callback->audioDeviceStopped();
            retiring->buffer = 128;
            retiring->callback->audioDeviceAboutToStart (retiring->device);
            const auto devicesBefore = fake->records.size();
            bool checkpointReached = false;
            plugins[0]->checkpoint = [&]
            {
                if (checkpointReached) return;
                checkpointReached = true;
                (engine.*member (PollMonitorMaintenance {}))();
                expect (retiring->callback != nullptr && retiring->playing, "The in-flight graph still owns the old monitor");
                expect (fake->records.size() == devicesBefore, "A maintenance tick must defer while the callback holds a reference");
            };
            probeInput();
            plugins[0]->checkpoint = {};
            expect (checkpointReached);
            (engine.*member (PollMonitorMaintenance {}))();
            expect (retiring->callback == nullptr && ! retiring->playing);
            expect (engine.isMonitorRunning());
            expectEquals (lm_obs_load_acquire (&ring.header->epoch), expectedEpoch);
            expectEquals (lostBlocks, 0);
            expectEquals (damagedBlocks, 0);
            checkMonitorAudio();

            beginTest ("N2: an input format change still prepares DSP and bumps OBS epoch exactly once");
            expectedEpoch = lm_obs_load_acquire (&ring.header->epoch);
            input->callback->audioDeviceStopped();
            input->rate = 44100.0;
            input->buffer = 128;
            input->callback->audioDeviceAboutToStart (input->device);
            ++expectedEpoch;
            expectEquals (lm_obs_load_acquire (&ring.header->epoch), expectedEpoch);
            recover();
            expectEquals (lm_obs_load_acquire (&ring.header->epoch), expectedEpoch);
            expectEquals (lm_obs_load_acquire (&ring.header->sample_rate), int64_t (44100));
            for (size_t i = 0; i < plugins.size(); ++i)
            {
                expectEquals (plugins[i]->prepareCount, prepares[i] + 1);
                expectEquals (plugins[i]->releaseCount, releases[i] + 1);
            }
            checkMonitorAudio();
            // Clear captures before the engine and fake devices are destroyed.
            fake->onOutputLifecycle = {};
            for (const auto& record : fake->records) record->onOutputLifecycle = {};
        }
    }
};
static LiveMixRound9Tests liveMixRound9Tests;

class LiveMixDeviceUiTests : public juce::UnitTest
{
public:
    LiveMixDeviceUiTests() : juce::UnitTest ("LiveMix device settings UI", "LiveMix") {}

    static void dispatchFor (int milliseconds)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + milliseconds;
        do
        {
            MSG message {};
            for (int i = 0; i < 100 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++i)
            {
                TranslateMessage (&message);
                DispatchMessageW (&message);
            }
            juce::Thread::sleep (5);
        } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    }
    void runTest() override
    {
        runFormatTextTests();
        beginTest ("device settings follow the running backend and apply input, monitor, rate and buffer immediately");
        const auto directory = juce::File::createTempFile ("-device-ui");
        expect (directory.deleteFile());
        expect (directory.createDirectory().wasOk());
        runSelectionRefreshTests (directory.getChildFile ("selection-refresh"));
        runAsioRateTests (directory.getChildFile ("asio-rate"));
        runSettingsWindowTests (directory.getChildFile ("settings-window"));
        {
            LiveMixLookAndFeel lookAndFeel;
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            MixFakeType* asioType = nullptr;
            MixFakeType* exclusiveType = nullptr;
            for (const auto* type : { "ASIO", "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" })
            {
                auto fake = std::make_unique<MixFakeType> (type);
                if (juce::String (type) == "ASIO") asioType = fake.get();
                if (juce::String (type) == "Windows Audio (Exclusive Mode)") exclusiveType = fake.get();
                engine.getDeviceManager().addAudioDeviceType (std::move (fake));
            }
            auto failMonitor = [&]
            {
                const auto monitor = exclusiveType->playingOutput();
                expect (monitor != nullptr && monitor->callback != nullptr);
                if (monitor != nullptr && monitor->callback != nullptr)
                {
                    exclusiveType->failOutputs = true;
                    monitor->callback->audioDeviceError ("deliberate output restart failure");
                    (engine.*member (PollMonitorMaintenance {}))();
                }
                expect (engine.isDeviceRunning() && ! engine.isMonitorRunning());
                expectEquals (engine.getOpenDevice().output, juce::String ("Headphones"));
                expectEquals (engine.getDeviceFormat().outputBits, 0);
            };
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
                auto* bitDepth = combo ("device-bitdepth");
                auto* bitDepthText = dynamic_cast<juce::Label*> (content->findChildWithID ("device-bitdepth-detail"));
                auto* soundSettings = dynamic_cast<juce::TextButton*> (content->findChildWithID ("windows-sound-settings"));
                expect (bitDepth != nullptr && bitDepthText != nullptr && soundSettings != nullptr);
                expect (type != nullptr && input != nullptr && output != nullptr && rate != nullptr && buffer != nullptr);
                if (type != nullptr && input != nullptr && output != nullptr && rate != nullptr && buffer != nullptr)
                {
                    expectEquals (type->getNumItems(), 4);
                    expectEquals (input->getText(), juce::String ("Good"));
                    expect (! output->isVisible() && rate->isVisible() && buffer->isVisible());
                    for (int mode = 1; mode <= 4; ++mode)
                    {
                        if (mode > 1) type->setSelectedId (mode, juce::sendNotificationSync);
                        const auto actual = engine.getOpenDevice();
                        expectEquals (type->getText(), AudioBackends::label (actual.type));
                        expectEquals (input->getText(), actual.input);
                        expectEquals (rate->getSelectedId(), (int) actual.sampleRate);
                        expectEquals (buffer->getSelectedId(), actual.bufferSize);
                        expect (output->isVisible() == (mode != 1));
                        expect (rate->isVisible());   // ASIO too (0.12.1)
                        expect (buffer->isVisible() == (mode != 2));
                        if (bitDepth != nullptr && bitDepthText != nullptr && soundSettings != nullptr)
                        {
                            expect (bitDepth->isVisible() == (mode == 4));
                            expect (bitDepthText->isVisible());
                            expect (soundSettings->isVisible() == (mode == 2 || mode == 3)); // never click: launches control.exe
                            if (mode == 1) expect (bitDepthText->getText().contains (ko ("32비트 · ASIO 드라이버가 정합니다")));
                            if (mode == 2 || mode == 3)
                                expect (bitDepthText->getText().contains (ko ("입력 알 수 없음, 출력 알 수 없음")));
                            if (mode == 4)
                            {
                                expectEquals (bitDepth->getNumItems(), 5);
                                expectEquals (bitDepth->getSelectedId(), 1);
                                expect (bitDepth->isItemEnabled (3)); // the fake's unknown capabilities cannot rule out a choice
                                bitDepth->setSelectedId (3, juce::sendNotificationSync);
                                expectEquals (engine.getOpenDevice().sampleFormat, juce::String ("int24"));
                                int bits = 0;
                                bool isFloat = true;
                                juce::getWasapiExclusivePreferredFormat (bits, isFloat);
                                expectEquals (bits, 24);
                                expect (! isFloat);
                                expectEquals (bitDepthText->getText(), ko ("지금: 입력 32비트, 출력 32비트"));
                            }
                        }
                        for (auto* child : content->getChildren())
                            if (child->isVisible())
                            {
                                expect (content->getLocalBounds().contains (child->getBounds()));
                                for (auto* other : content->getChildren())
                                    if (other != child && other->isVisible())
                                        expect (! child->getBounds().intersects (other->getBounds()), child->getComponentID() + " overlaps " + other->getComponentID());
                            }
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
                    expectEquals (changes, 8);
                    type->setSelectedId (1, juce::sendNotificationSync);
                    expectEquals (engine.getOpenDevice().type, juce::String ("ASIO"));
                    expectEquals (input->getText(), juce::String ("Good"));
                    expect (! output->isVisible());
                    expectEquals (engine.getOpenDevice().sampleFormat, juce::String ("int24"));
                    if (bitDepthText != nullptr && asioType != nullptr)
                    {
                        for (const auto& record : asioType->records) if (record->playing) record->bits = 24;
                        input->addItem ("timer must keep this item", 999);
                        const int inputItems = input->getNumItems();
                        const int selected = input->getSelectedId();
                        dispatchFor (650);
                        expect (bitDepthText->getText().contains (ko ("24비트 · ASIO 드라이버가 정합니다")));
                        expectEquals (input->getNumItems(), inputItems);
                        expectEquals (input->getSelectedId(), selected);
                    }
                    type->setSelectedId (4, juce::sendNotificationSync);
                    if (bitDepth != nullptr) expectEquals (bitDepth->getSelectedId(), 3);
                    failMonitor();
                    dispatchFor (650);
                    if (bitDepthText != nullptr) expectEquals (bitDepthText->getText(), ko ("지금: 입력 32비트"));
                    exclusiveType->failOutputs = false;
                    expect (engine.restartDevice().isEmpty());
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
            beginTest ("top bar status includes actual input bits, omits unknown bits and gives both directions in its tooltip");
            MixEngine::DeviceFormat format;
            format.kind = MixEngine::DeviceFormat::Kind::windowsShared;
            format.inputBits = 24;
            format.outputBits = 32;
            format.outputFloat = true;
            format.inputDeviceRate = 44100.0;
            format.outputDeviceRate = 48000.0;
            expectEquals (TopBar::buildStatusText (48000.0, 256, 10.7, true, format), ko ("48.0 kHz · 24비트 · 256 샘플  10.7 ms"));
            expectEquals (TopBar::buildStatusText (48000.0, 256, 10.7, true, format, false), ko ("48.0 kHz · 24비트 · 256  10.7 ms"));
            expectEquals (TopBar::buildStatusText (48000.0, 256, 10.7, false, format), ko ("오디오 멈춤"));
            expectEquals (TopBar::buildStatusText (44100.0, 128, 5.8, true, {}), ko ("44.1 kHz · 128 샘플  5.8 ms"));
            bar.setStatus (48000.0, 256, 10.7, 0.2, true, format);
            auto* status = dynamic_cast<juce::Label*> (bar.findChildWithID ("device-status"));
            expect (status != nullptr);
            if (status != nullptr)
            {
                expect (status->getTooltip().contains (ko ("입력 24비트 · 44.1 kHz, 출력 32비트 부동소수점 · 48 kHz")));
                expect (status->getTooltip().contains (ko ("(윈도우 설정)")));
                for (const auto kind : { MixEngine::DeviceFormat::Kind::asio, MixEngine::DeviceFormat::Kind::windowsExclusive })
                {
                    format.kind = kind;
                    bar.setStatus (48000.0, 256, 10.7, 0.2, true, format);
                    expect (status->getTooltip().contains (kind == MixEngine::DeviceFormat::Kind::asio ? ko ("(ASIO 드라이버)") : ko ("(독점)")));
                }
            }
            beginTest ("top bar measures status text in all layouts down to 420 px without shrinking the device below 120 px");
            for (int sessionState = 0; sessionState < 3; ++sessionState)
            {
                if (sessionState == 1) expect (document.save (directory.getChildFile ("Top bar session.livemix")).wasOk());
                if (sessionState == 2) document.markDirty (false);
                bar.refresh();
                auto* state = dynamic_cast<juce::Label*> (bar.findChildWithID ("session-state"));
                expect (state != nullptr);
                if (state != nullptr) expectEquals (state->getText(), sessionState == 0 ? ko ("아직 파일 없음") : sessionState == 1 ? ko ("저장됨") : ko ("저장 안 됨"));
                for (const int width : { 420, 480, 580, 699, 700, 900, 1219, 1220, 1280, 1366, 1400, 1439, 1440, 1920 })
                    for (bool muted : { false, true })
                        for (bool large : { false, true })
                        {
                            bar.setMuteGroups (muted, muted);
                            expect (bar.modeFor (width) == (width >= 1220 ? TopBar::Mode::wide : width >= 700 ? TopBar::Mode::compact : TopBar::Mode::narrow));
                            bar.setSize (width, bar.preferredHeight (width));
                            bar.setStatus (large ? 384000.0 : 48000.0, large ? 8192 : 256, large ? 432.1 : 10.7, 0.2, true, format);
                            const auto full = TopBar::buildStatusText (large ? 384000.0 : 48000.0, large ? 8192 : 256, large ? 432.1 : 10.7, true, format);
                            const auto shortText = TopBar::buildStatusText (large ? 384000.0 : 48000.0, large ? 8192 : 256, large ? 432.1 : 10.7, true, format, false);
                            const auto minimal = TopBar::buildStatusText (large ? 384000.0 : 48000.0, large ? 8192 : 256, large ? 432.1 : 10.7, true, {}, false);
                            if (status != nullptr)
                            {
                                expect (status->getText() == full || status->getText() == shortText || status->getText() == minimal);
                                if (width == 1920) expectEquals (status->getText(), full);
                                expect (status->getTooltip().startsWith (full));
                                expectGreaterOrEqual (status->getWidth(), labelWidthForText (*status, status->getText()),
                                                      "status fit at " + juce::String (width) + ": " + status->getText());
                                expectEquals (status->getMinimumHorizontalScale(), 1.0f);
                            }
                            for (auto* child : bar.getChildren())
                            {
                                if (! child->isVisible() || child->getBounds().isEmpty()) continue;
                                expect (bar.getLocalBounds().contains (child->getBounds()), "top bar child outside at " + juce::String (width));
                                if (auto* box = dynamic_cast<juce::ComboBox*> (child)) expectGreaterOrEqual (box->getWidth(), 120);
                                if (auto* label = dynamic_cast<juce::Label*> (child))
                                {
                                    if (label->getTooltip() == ko ("열린 세션. 세션 버튼에서 저장·열기"))
                                        expectGreaterOrEqual (label->getWidth(), 160, "session name at " + juce::String (width));
                                    if (label->getText() == ko ("아직 파일 없음") || label->getText() == ko ("저장 안 됨") || label->getText() == ko ("저장됨"))
                                        expectGreaterOrEqual (label->getWidth(), labelWidthForText (*label, label->getText()), "session state at " + juce::String (width));
                                }
                                for (auto* other : bar.getChildren())
                                    if (other != child && other->isVisible() && ! other->getBounds().isEmpty())
                                        expect (! child->getBounds().intersects (other->getBounds()), "top bar overlap at " + juce::String (width));
                            }
                            const auto folder = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_UI_SCREENSHOT_DIR", {});
                            if (folder.isNotEmpty())
                            {
                                auto shotDirectory = sessionState == 0 ? juce::File (folder) : juce::File (folder).getChildFile (sessionState == 1 ? "saved" : "dirty");
                                if (large) shotDirectory = shotDirectory.getChildFile ("extreme");
                                expect (shotDirectory.createDirectory().wasOk());
                                juce::FileOutputStream image (shotDirectory.getChildFile ("topbar-" + juce::String (width) + (muted ? "-both" : "-none") + ".png"));
                                expect (image.openedOk());
                                if (image.openedOk())
                                {
                                    expect (image.setPosition (0));
                                    expect (image.truncate().wasOk());
                                    expect (juce::PNGImageFormat().writeImageToStream (bar.createComponentSnapshot (bar.getLocalBounds()), image));
                                }
                            }
                        }
            }
            beginTest ("a stopped split monitor is omitted from status detail even while its saved endpoint is retained");
            failMonitor();
            bar.setStatus (engine.getSampleRate(), engine.getBlockSize(), engine.getLatencyMs(), 0.0, true, engine.getDeviceFormat());
            if (status != nullptr)
            {
                expect (status->getTooltip().contains (ko ("입력 32비트")));
                expect (! status->getTooltip().contains (ko ("출력")));
            }
            bar.setLookAndFeel (nullptr);
        }
        expect (directory.deleteRecursively());
    }

    void runSelectionRefreshTests (const juce::File& directory)
    {
        beginTest ("saved int24 survives one-sided capability labels, the 500 ms refresh and a buffer reopen");
        const ScopedWasapiPreference preference;
        for (bool inputOnly : { false, true })
        {
            const MixDevice saved { "Windows Audio (Exclusive Mode)", "Capture", "Headphones", 256, 48000.0, "int24" };
            { LiveMixSettings initial (directory); initial.setLastDevice (saved); }
            LiveMixSettings settings (directory);
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> (saved.type));
            expect (settings.getLastDevice().has_value());
            expect (engine.openDevice (*settings.getLastDevice()).isEmpty());
            std::pair<int, int> accepted {};
            int changes = 0;
            for (int reopen = 0; reopen < 2; ++reopen)
            {
                SettingsDialog::show (engine, settings, nullptr, [&]
                {
                    ++changes;
                    settings.setLastDevice (engine.getOpenDevice());
                }, {}, {}, {}, {}, [&] { return accepted; });
                juce::Component* content = nullptr;
                auto& desktop = juce::Desktop::getInstance();
                for (int i = 0; i < desktop.getNumComponents(); ++i)
                    if (auto* dialog = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)); dialog != nullptr && dialog->getName() == ko ("설정"))
                        if (auto* viewport = dynamic_cast<juce::Viewport*> (dialog->getContentComponent())) content = viewport->getViewedComponent();
                expect (content != nullptr);
                if (content != nullptr)
                {
                    auto* depth = dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-bitdepth"));
                    auto* buffer = dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-buffer"));
                    expect (depth != nullptr && buffer != nullptr);
                    if (depth != nullptr && buffer != nullptr)
                    {
                        expectEquals (depth->getSelectedId(), 3);
                        accepted = inputOnly ? std::make_pair (juce::WasapiFormatInfo::exclusiveInt24, juce::WasapiFormatInfo::exclusiveInt16)
                                             : std::make_pair (juce::WasapiFormatInfo::exclusiveInt16, juce::WasapiFormatInfo::exclusiveInt24);
                        dispatchFor (650);
                        expectEquals (changes, reopen);
                        expectEquals (depth->getSelectedId(), 3);
                        expectEquals (depth->getText(), inputOnly ? ko ("24비트 — 입력만") : ko ("24비트 — 출력만"));
                        expect (! depth->isItemEnabled (4));
                        buffer->setSelectedId (reopen == 0 ? 512 : 256, juce::sendNotificationSync);
                        expectEquals (changes, reopen + 1);
                        expectEquals (engine.getOpenDevice().bufferSize, reopen == 0 ? 512 : 256);
                        expectEquals (engine.getOpenDevice().sampleFormat, juce::String ("int24"));
                        expectEquals (settings.getLastDevice()->sampleFormat, juce::String ("int24"));
                        expectEquals (depth->getSelectedId(), 3);
                    }
                }
                SettingsDialog::closeIfOpen();
            }
        }
    }

    static juce::Component* openSettingsContent (MixEngine& engine, LiveMixSettings& settings, std::function<void()> deviceChanged)
    {
        SettingsDialog::show (engine, settings, nullptr, std::move (deviceChanged), {}, {}, {}, {});
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* dialog = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)); dialog != nullptr && dialog->getName() == ko ("설정"))
                if (auto* viewport = dynamic_cast<juce::Viewport*> (dialog->getContentComponent())) return viewport->getViewedComponent();
        return nullptr;
    }

    // 10/4 gom: the settings opened glued to the top of the screen, as tall as the screen with the title bar off it -
    // they open centred and short (640 px, or 70% of a small screen) and the wheel scrolls the rest
    void runSettingsWindowTests (const juce::File& directory)
    {
        beginTest ("settings opening place: 100% and 125% screens, the app near an edge, a monitor above, short settings, tiny and narrow screens");
        {
            const juce::BorderSize<int> frame (31, 8, 8, 8);
            const juce::Rectangle<int> fullHd (0, 0, 1920, 1032), scaled (0, 0, 1536, 816), upper (1920, -1012, 1920, 1080),
                                       tiny (0, 0, 800, 300), narrow (0, 0, 500, 1000);
            const auto place = [&] (int contentHeight, juce::Point<int> centre, juce::Rectangle<int> screen)
            {
                const auto p = SettingsDialog::placement (568, contentHeight, centre, screen, frame);
                const auto framed = frame.addedTo (p.bounds);
                expect (screen.reduced (12).contains (framed), "frame on screen: " + framed.toString() + " in " + screen.toString());
                // the resize limits let the opening size stand: JUCE would otherwise force it back off screen
                expect (p.minWidth <= p.bounds.getWidth() && p.bounds.getWidth() <= p.maxWidth
                        && p.minHeight <= p.bounds.getHeight() && p.bounds.getHeight() <= p.maxHeight, "limits admit " + p.bounds.toString());
                return p;
            };
            const auto p = place (1170, fullHd.getCentre(), fullHd);
            expectEquals (p.bounds.getWidth(), 568);
            expectEquals (p.bounds.getHeight(), 640);
            expect (frame.addedTo (p.bounds).getCentre() == fullHd.getCentre(), "centred: " + frame.addedTo (p.bounds).toString());
            expect (p.minWidth == 568 && p.maxWidth == 568 && p.minHeight == 320 && p.maxHeight == 1170);   // dragged taller: up to all the settings
            expectEquals (place (1170, scaled.getCentre(), scaled).bounds.getHeight(), 571);         // 70% of a 125% screen's 816
            expectEquals (place (500, fullHd.getCentre(), fullHd).bounds.getHeight(), 500);         // short settings: no empty space
            expectEquals (frame.addedTo (place (1170, { 960, 100 }, fullHd).bounds).getY(), 12);     // the app high up: down until the title bar shows
            expectEquals (frame.addedTo (place (1170, { 960, 1000 }, fullHd).bounds).getBottom(), 1020);
            const auto above = place (1170, upper.getCentre(), upper);                                // a monitor above the main one
            expectEquals (above.bounds.getHeight(), 640);
            expect (frame.addedTo (above.bounds).getCentre() == upper.getCentre(), "centred above: " + frame.addedTo (above.bounds).toString());
            expectEquals (place (1170, tiny.getCentre(), tiny).bounds.getHeight(), 300 - 24 - 39);    // as tall as fits, under the usual 320
            expectEquals (place (1170, narrow.getCentre(), narrow).bounds.getWidth(), 500 - 24 - 16); // narrower than the settings: they scroll sideways
        }

        beginTest ("settings resize limits hold the inside: borders a monitor's scale rounds wider or narrower never squeeze or widen it");
        {
            SettingsDialog::ClientLimits limits;
            limits.setClientLimits (568, 568, 320, 1170);
            const juce::Rectangle<int> everywhere (-10000, -10000, 20000, 20000);
            for (int side = 5; side <= 12; ++side)   // the limits made with 8 px side borders once squeezed 9 px ones to 566
            {
                const juce::BorderSize<int> frame (31, side, side, side);
                limits.frameNow = [frame] { return frame; };
                const auto before = frame.addedTo (juce::Rectangle<int> (100, 100, 568, 640));
                for (const int width : { 400, 568, 900 })   // dragged narrower, left alone, dragged wider
                {
                    auto r = frame.addedTo (juce::Rectangle<int> (100, 100, width, 640));
                    limits.checkBounds (r, before, everywhere, false, false, false, true);
                    expectEquals (frame.subtractedFrom (r).getWidth(), 568, "borders " + juce::String (side) + ", " + juce::String (width) + " wide");
                }
                for (const int height : { 100, 2000 })
                {
                    auto r = frame.addedTo (juce::Rectangle<int> (100, 100, 568, height));
                    limits.checkBounds (r, before, everywhere, false, false, true, false);
                    expectEquals (frame.subtractedFrom (r).getHeight(), height < 640 ? 320 : 1170);
                }
            }
        }

        beginTest ("settings open centred and no taller than 640 px or 70% of the screen, the title bar on screen; the wheel scrolls the rest");
        expect (directory.createDirectory().wasOk());
        MixEngine engine;
        removeRealMixDeviceTypes (engine);
        engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> ("ASIO"));
        expect (engine.openDevice ({ "ASIO", "Good", "Good", 256, 48000.0 }).isEmpty());
        LiveMixSettings settings (directory);
        SettingsDialog::show (engine, settings, nullptr, {}, {}, {}, {}, {});
        auto& desktop = juce::Desktop::getInstance();
        juce::DialogWindow* window = nullptr;
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* dialog = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)); dialog != nullptr && dialog->getName() == ko ("설정"))
                window = dialog;
        auto* viewport = window != nullptr ? dynamic_cast<juce::Viewport*> (window->getContentComponent()) : nullptr;
        auto* content = viewport != nullptr ? viewport->getViewedComponent() : nullptr;
        const auto* display = desktop.getDisplays().getPrimaryDisplay();
        expect (window != nullptr && window->getPeer() != nullptr && viewport != nullptr && content != nullptr && display != nullptr);
        if (window != nullptr && window->getPeer() != nullptr && viewport != nullptr && content != nullptr && display != nullptr)
        {
            const auto screen = display->userBounds.toNearestInt();
            const auto bounds = window->getScreenBounds();
            const auto frame = window->getPeer()->getFrameSize();
            const auto framed = frame.addedTo (bounds);
            const int fullWidth = content->getWidth() + viewport->getScrollBarThickness();
            const auto expected = SettingsDialog::placement (fullWidth, content->getHeight(), screen.getCentre(), screen, frame);
            expect (bounds == expected.bounds, "opened at " + bounds.toString() + ", placed at " + expected.bounds.toString());
            expect (bounds.getHeight() <= 640 && content->getHeight() > bounds.getHeight(), "short, the settings scroll: " + bounds.toString());
            if (screen.getHeight() >= 1032)
                expectEquals (bounds.getHeight(), 640);   // 1080p at 100% and taller screens
            expect (screen.reduced (12).contains (framed), "title bar and borders on screen: " + framed.toString() + " in " + screen.toString());
            expect (std::abs (framed.getCentreX() - screen.getCentreX()) <= 1 && std::abs (framed.getCentreY() - screen.getCentreY()) <= 1,
                    "centred: " + framed.toString() + " in " + screen.toString());

            // the wheel over a box (which takes no wheel) scrolls the settings and leaves the box alone, down to the end
            auto* type = dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-type"));
            expect (type != nullptr);
            expect (viewport->getVerticalScrollBar().isVisible(), "a scroll bar down the side");
            if (expected.bounds.getWidth() == fullWidth)   // a screen as wide as the settings shows them whole: limits of the
            {                                              // bare width once squeezed the window 16 px narrower than its settings
                expectEquals (window->getWidth(), fullWidth);
                expect (! viewport->getHorizontalScrollBar().isVisible(), "no sideways scroll bar: view " + juce::String (viewport->getViewWidth())
                        + " for " + juce::String (content->getWidth()) + " px of settings");
            }
            if (type != nullptr)
            {
                const int selected = type->getSelectedId();
                const auto now = juce::Time::getCurrentTime();
                const juce::MouseEvent event (desktop.getMainMouseSource(), { 20.0f, 10.0f }, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                              type, type, now, { 20.0f, 10.0f }, now, 0, false);
                juce::MouseWheelDetails wheel {};
                wheel.deltaY = -60.0f / 256.0f;   // one notch down, as JUCE reads a WM_MOUSEWHEEL of 120
                type->mouseWheelMove (event, wheel);
                expect (viewport->getViewPositionY() > 0, "one notch scrolled " + juce::String (viewport->getViewPositionY()) + " px");
                expectEquals (type->getSelectedId(), selected);
                for (int i = 0; i < 100 && viewport->getViewPositionY() < content->getHeight() - viewport->getViewHeight(); ++i)
                    type->mouseWheelMove (event, wheel);
                expectEquals (viewport->getViewPositionY(), content->getHeight() - viewport->getViewHeight());
            }
        }
        SettingsDialog::closeIfOpen();
    }

    void runAsioRateTests (const juce::File& directory)
    {
        beginTest ("ASIO rate box: only the driver's rates, the chosen one opens and survives a buffer change, a refused one is said");
        expect (directory.createDirectory().wasOk());
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto fake = std::make_unique<MixFakeType> ("ASIO");
            auto* asio = fake.get();
            // no 44.1 kHz: the Windows pair is not offered for ASIO; 8k and 384k are outside the 44.1 - 192 kHz shown
            asio->rates = { 96000.0, 48000.0, 8000.0, 88200.0, 384000.0, 192000.0 };
            asio->refusedRate = 88200.0;
            asio->failedRate = 192000.0;
            engine.getDeviceManager().addAudioDeviceType (std::move (fake));
            expect (engine.openDevice ({ "ASIO", "Good", "Good", 256, 0.0 }).isEmpty());
            expectEquals (juce::roundToInt (engine.getOpenDevice().sampleRate), 48000);
            LiveMixSettings settings (directory);
            int changes = 0;
            auto* content = openSettingsContent (engine, settings, [&]
            {
                ++changes;
                settings.setLastDevice (engine.getOpenDevice());
            });
            expect (content != nullptr);
            auto* rate = content != nullptr ? dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-rate")) : nullptr;
            auto* buffer = content != nullptr ? dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-buffer")) : nullptr;
            expect (rate != nullptr && buffer != nullptr);
            if (rate != nullptr && buffer != nullptr)
            {
                expect (rate->isVisible());
                expectEquals (rate->getNumItems(), 4);
                expectEquals (rate->getItemId (0), 48000);
                expectEquals (rate->getItemId (1), 88200);
                expectEquals (rate->getItemId (2), 96000);
                expectEquals (rate->getItemId (3), 192000);
                expectEquals (rate->getSelectedId(), 48000);

                rate->setSelectedId (96000, juce::sendNotificationSync);
                expectEquals (juce::roundToInt (engine.getOpenDevice().sampleRate), 96000);
                expectEquals (rate->getSelectedId(), 96000);
                expectEquals (changes, 1);
                expect (settings.getLastDevice().has_value() && juce::roundToInt (settings.getLastDevice()->sampleRate) == 96000);
                dispatchFor (50);
                expect (dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent()) == nullptr);

                buffer->setSelectedId (512, juce::sendNotificationSync);
                expectEquals (engine.getOpenDevice().bufferSize, 512);
                expectEquals (juce::roundToInt (engine.getOpenDevice().sampleRate), 96000);   // a buffer change keeps the chosen rate
                expectEquals (rate->getSelectedId(), 96000);

                rate->setSelectedId (88200, juce::sendNotificationSync);
                expectEquals (juce::roundToInt (engine.getOpenDevice().sampleRate), 96000);   // refused: it runs on at 96 kHz
                expectEquals (rate->getSelectedId(), 96000);
                expect (juce::roundToInt (settings.getLastDevice()->sampleRate) == 96000);
                dispatchFor (50);
                auto* alert = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
                expect (alert != nullptr);
                if (alert != nullptr)
                {
                    expectEquals (alert->getName(), ko ("샘플레이트를 바꾸지 못했습니다"));
                    alert->exitModalState (0);
                }
                dispatchFor (50);

                // a failed open rolls back and says so once: no rate notice on top of the error
                rate->setSelectedId (192000, juce::sendNotificationSync);
                expectEquals (juce::roundToInt (engine.getOpenDevice().sampleRate), 96000);
                expectEquals (rate->getSelectedId(), 96000);
                dispatchFor (50);
                alert = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
                expect (alert != nullptr);
                if (alert != nullptr)
                {
                    expectEquals (alert->getName(), ko ("오디오 장치를 열지 못했습니다"));
                    alert->exitModalState (0);
                }
                dispatchFor (50);
                expect (dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent()) == nullptr);
            }
            SettingsDialog::closeIfOpen();

            // running outside the range (set elsewhere): that rate is still shown, the others outside stay hidden
            expect (engine.openDevice ({ "ASIO", "Good", "Good", 256, 8000.0 }).isEmpty());
            expectEquals (juce::roundToInt (engine.getOpenDevice().sampleRate), 8000);
            content = openSettingsContent (engine, settings, {});
            rate = content != nullptr ? dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-rate")) : nullptr;
            expect (rate != nullptr);
            if (rate != nullptr)
            {
                expectEquals (rate->getNumItems(), 5);
                expectEquals (rate->getItemId (0), 8000);
                expectEquals (rate->getItemId (3), 96000);
                expectEquals (rate->getItemId (4), 192000);
                expectEquals (rate->getSelectedId(), 8000);
            }
            SettingsDialog::closeIfOpen();
        }

        beginTest ("ASIO with no device open shows no rate rather than a made-up 48 kHz; Windows audio still offers 44.1 / 48 kHz");
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<MixFakeType> ("ASIO"));
            LiveMixSettings settings (directory);
            auto* content = openSettingsContent (engine, settings, {});
            auto* rate = content != nullptr ? dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-rate")) : nullptr;
            expect (rate != nullptr);
            if (rate != nullptr)
            {
                expect (engine.getOpenDevice().input.isEmpty());
                expect (rate->isVisible());
                expectEquals (rate->getNumItems(), 0);
                expectEquals (rate->getSelectedId(), 0);
            }
            SettingsDialog::closeIfOpen();

            auto windows = std::make_unique<MixFakeType> ("Windows Audio");
            windows->rates = { 96000.0 };
            engine.getDeviceManager().addAudioDeviceType (std::move (windows));
            expect (engine.openDevice ({ "Windows Audio", "Capture", "Headphones", 0, 96000.0 }).isEmpty());
            content = openSettingsContent (engine, settings, {});
            rate = content != nullptr ? dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-rate")) : nullptr;
            expect (rate != nullptr);
            if (rate != nullptr)
            {
                expectEquals (rate->getNumItems(), 3);
                expectEquals (rate->getItemId (0), 44100);
                expectEquals (rate->getItemId (1), 48000);
                expectEquals (rate->getSelectedId(), 96000);
            }
            SettingsDialog::closeIfOpen();
        }

        beginTest ("ASIO: the dialog follows a device changed under it, says when a reset soon undoes a chosen rate, a closed device offers none");
        {
            MixEngine engine;
            removeRealMixDeviceTypes (engine);
            auto fake = std::make_unique<MixFakeType> ("ASIO");
            auto* asio = fake.get();
            fake->rates = { 48000.0, 96000.0 };
            engine.getDeviceManager().addAudioDeviceType (std::move (fake));
            // a driver reset (JUCE reopens by itself, never through openDevice) that leaves the device at 96 kHz
            auto resetTo96 = [asio] { for (auto& record : asio->records) if (record->playing) record->rate = 96000.0; };
            expect (engine.openDevice ({ "ASIO", "Good", "Good", 256, 48000.0 }).isEmpty());
            LiveMixSettings settings (directory);
            auto* content = openSettingsContent (engine, settings, {});
            auto* rate = content != nullptr ? dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-rate")) : nullptr;
            auto* buffer = content != nullptr ? dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-buffer")) : nullptr;
            expect (rate != nullptr && buffer != nullptr);
            if (rate != nullptr && buffer != nullptr)
            {
                auto noAlert = [] { return dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent()) == nullptr; };
                expectEquals (rate->getSelectedId(), 48000);

                // changed under the dialog (a session opened from Explorer, a driver reset): shown by the 500 ms refresh
                expect (engine.openDevice ({ "ASIO", "Good", "Good", 512, 96000.0 }).isEmpty());
                dispatchFor (650);
                expectEquals (rate->getSelectedId(), 96000);
                expectEquals (buffer->getSelectedId(), 512);
                expect (noAlert());

                // chosen, then undone by a reset moments later: said like a refused rate
                rate->setSelectedId (48000, juce::sendNotificationSync);
                expectEquals (juce::roundToInt (engine.getOpenDevice().sampleRate), 48000);
                expect (noAlert());
                resetTo96();
                dispatchFor (650);
                auto* alert = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
                expect (alert != nullptr);
                if (alert != nullptr)
                {
                    expectEquals (alert->getName(), ko ("샘플레이트를 바꾸지 못했습니다"));
                    alert->exitModalState (0);
                }
                dispatchFor (50);
                expectEquals (rate->getSelectedId(), 96000);

                // a reopen the app asks for right after (a session opened) is shown, not blamed on the choice
                rate->setSelectedId (48000, juce::sendNotificationSync);
                expectEquals (juce::roundToInt (engine.getOpenDevice().sampleRate), 48000);
                expect (engine.openDevice ({ "ASIO", "Good", "Good", 512, 96000.0 }).isEmpty());
                dispatchFor (650);
                expect (noAlert());
                expectEquals (rate->getSelectedId(), 96000);

                // seconds later a reset is only shown too
                rate->setSelectedId (48000, juce::sendNotificationSync);
                dispatchFor (3300);
                expect (noAlert());
                resetTo96();
                dispatchFor (650);
                expect (noAlert());
                expectEquals (rate->getSelectedId(), 96000);
            }
            SettingsDialog::closeIfOpen();

            // a device object left closed (a failed driver reset) offers no rate to pick
            if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
                device->close();
            content = openSettingsContent (engine, settings, {});
            rate = content != nullptr ? dynamic_cast<juce::ComboBox*> (content->findChildWithID ("device-rate")) : nullptr;
            expect (rate != nullptr);
            if (rate != nullptr)
            {
                expectEquals (rate->getNumItems(), 0);
                expectEquals (rate->getSelectedId(), 0);
            }
            SettingsDialog::closeIfOpen();
        }
    }

    void runFormatTextTests()
    {
        using Kind = MixEngine::DeviceFormat::Kind;
        beginTest ("Korean bit depth text explains ASIO control, Windows conversion directions and exclusive refusal");
        MixDevice device { "Windows Audio", "Capture", "Headphones", 256, 48000.0, "int24" };
        MixEngine::DeviceFormat format;
        expectEquals (DeviceFormatText::settings (format, device, false).detail, ko ("장치가 열려 있지 않습니다"));
        format.kind = Kind::asio;
        format.inputBits = format.outputBits = 24;
        expectEquals (DeviceFormatText::settings (format, device, false).detail, ko ("24비트 · ASIO 드라이버가 정합니다"));
        expectEquals (DeviceFormatText::settings (format, device, true).detail,
                      ko ("24비트 · ASIO 드라이버가 정합니다 (바꿀 수 있는 장치는 ASIO 제어판에서)"));
        format.kind = Kind::windowsShared;
        format.inputBits = 16;
        format.inputDeviceRate = 44100.0;
        format.outputDeviceRate = 96000.0;
        format.inputStreamRate = format.outputStreamRate = 48000.0;
        auto text = DeviceFormatText::settings (format, device, false);
        expectEquals (text.detail, ko ("입력 16비트 · 44.1 kHz, 출력 24비트 · 96 kHz (윈도우 소리 설정의 '기본 형식')"));
        expect (text.hint.contains (ko ("녹음/재생 탭 → 장치 더블클릭 → 고급 → 기본 형식에서 바꿉니다.")));
        expect (text.hint.contains (ko ("입력: 윈도우가 44.1 kHz → 48 kHz로 변환 중")));
        expect (text.hint.contains (ko ("출력: 윈도우가 48 kHz → 96 kHz로 변환 중")));
        format.outputStreamRate = 44100.0;
        text = DeviceFormatText::settings (format, device, false);
        expect (text.hint.contains (ko ("출력: 윈도우가 44.1 kHz → 96 kHz로 변환 중")));
        expect (! text.hint.contains (ko ("출력: 윈도우가 48 kHz")));
        format.outputDeviceRate = 44100.0;
        text = DeviceFormatText::settings (format, device, false);
        expect (! text.hint.contains (ko ("출력: 윈도우가")), "LiveMix's split monitor resampling is not Windows conversion");
        format.inputStreamRate = 96000.0;
        text = DeviceFormatText::settings (format, device, false);
        expect (text.hint.contains (ko ("입력: 윈도우가 44.1 kHz → 96 kHz로 변환 중")));
        format.inputStreamRate = format.outputStreamRate = 48000.0;
        format.inputDeviceRate = format.outputDeviceRate = 48000.0;
        text = DeviceFormatText::settings (format, device, false);
        expect (! text.hint.contains (ko ("변환 중")));
        format.inputBits = 0;
        format.inputDeviceRate = 0;
        device.output.clear();
        text = DeviceFormatText::settings (format, device, false);
        expectEquals (text.detail, ko ("입력 알 수 없음 (윈도우 소리 설정의 '기본 형식')"));
        expect (! text.hint.contains (ko ("변환 중")));
        device.output = "Headphones";
        format.kind = Kind::windowsExclusive;
        format.inputBits = 16;
        format.inputRefused = true;
        text = DeviceFormatText::settings (format, device, false);
        expectEquals (text.detail, ko ("지금: 입력 16비트, 출력 24비트"));
        expectEquals (text.warning, ko ("입력 장치가 24비트를 받지 않아 16비트로 열었습니다."));
        device.sampleFormat = "float32";
        format.outputRefused = true;
        text = DeviceFormatText::settings (format, device, false);
        expect (text.warning.contains (ko ("출력 장치가 32비트 부동소수점을 받지 않아 24비트로 열었습니다.")));
        expectEquals (DeviceFormatText::bitDepth (32, true), ko ("32비트 부동소수점"));

        beginTest ("exclusive choices name one-sided support, disable unsupported formats and leave unknown capabilities selectable");
        format.inputAccepted = juce::WasapiFormatInfo::exclusiveInt16;
        format.outputAccepted = juce::WasapiFormatInfo::exclusiveInt16 | juce::WasapiFormatInfo::exclusiveInt24;
        expect (DeviceFormatText::exclusiveItem (1, format, true, true).enabled);
        expectEquals (DeviceFormatText::exclusiveItem (1, format, true, true).text, ko ("자동 (장치가 받는 가장 높은 형식)"));
        expectEquals (DeviceFormatText::exclusiveItem (2, format, true, true).text, ko ("16비트"));
        expectEquals (DeviceFormatText::exclusiveItem (3, format, true, true).text, ko ("24비트 — 출력만"));
        expect (DeviceFormatText::exclusiveItem (3, format, true, true).enabled);
        expectEquals (DeviceFormatText::exclusiveItem (4, format, true, true).text, ko ("32비트 — 이 장치 지원 안 함"));
        expect (! DeviceFormatText::exclusiveItem (4, format, true, true).enabled);
        expect (! DeviceFormatText::exclusiveItem (5, format, true, true).enabled);
        format.inputAccepted = juce::WasapiFormatInfo::exclusiveInt24;
        expectEquals (DeviceFormatText::exclusiveItem (3, format, true, false).text, ko ("24비트 — 입력만"));
        expect (! DeviceFormatText::exclusiveItem (2, format, true, false).enabled);
        format.inputAccepted = format.outputAccepted = 0;
        for (int id = 1; id <= 5; ++id) expect (DeviceFormatText::exclusiveItem (id, format, true, true).enabled);
    }
};
static LiveMixDeviceUiTests liveMixDeviceUiTests;

} // namespace gocue::tests
