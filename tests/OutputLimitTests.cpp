#include "audio/AudioEngine.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <vector>

namespace gocue::tests
{

/** An in-memory device with N outputs: no hardware, no threads. Records every open request; can refuse wide ones. */
class FakeDevice : public juce::AudioIODevice
{
public:
    FakeDevice (const juce::String& name, const juce::String& typeName, int numOutputs, int rejectAboveChannels,
                std::vector<int>* history)
        : juce::AudioIODevice (name, typeName), outputs (numOutputs), rejectAbove (rejectAboveChannels), openHistory (history) {}

    juce::StringArray getOutputChannelNames() override
    {
        juce::StringArray names;

        for (int i = 0; i < outputs; ++i)
            names.add ("Out " + juce::String (i + 1));

        return names;
    }

    juce::StringArray getInputChannelNames() override            { return {}; }
    juce::Array<double> getAvailableSampleRates() override        { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override           { return { 512 }; }
    int getDefaultBufferSize() override                           { return 512; }

    juce::String open (const juce::BigInteger&, const juce::BigInteger& outputChannels, double, int) override
    {
        auto requested = outputChannels;
        requested.setRange (outputs, 256, false);   // only the channels this device has
        const int count = requested.countNumberOfSetBits();

        if (openHistory != nullptr)
            openHistory->push_back (count);

        if (rejectAbove > 0 && count > rejectAbove)
            return "the device does not support " + juce::String (count) + " channels";

        activeOutputs = requested;
        opened = true;
        return {};
    }

    void close() override                                         { opened = false; }
    bool isOpen() override                                        { return opened; }

    void start (juce::AudioIODeviceCallback* cb) override
    {
        callback = cb;

        if (callback != nullptr)
            callback->audioDeviceAboutToStart (this);
    }

    void stop() override
    {
        if (callback != nullptr)
            callback->audioDeviceStopped();

        callback = nullptr;
    }

    bool isPlaying() override                                     { return callback != nullptr; }
    juce::String getLastError() override                          { return {}; }
    int getCurrentBufferSizeSamples() override                    { return 512; }
    double getCurrentSampleRate() override                        { return 48000.0; }
    int getCurrentBitDepth() override                             { return 32; }
    juce::BigInteger getActiveOutputChannels() const override     { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override      { return {}; }
    int getOutputLatencyInSamples() override                      { return 0; }
    int getInputLatencyInSamples() override                       { return 0; }

    int outputs;
    int rejectAbove;
    std::vector<int>* openHistory;
    juce::BigInteger activeOutputs;
    bool opened = false;
    juce::AudioIODeviceCallback* callback = nullptr;
};

/** One device type with one device; the type name decides whether the engine treats it as ASIO. */
class FakeType : public juce::AudioIODeviceType
{
public:
    FakeType (const juce::String& typeName, int numOutputs, int rejectAboveChannels = 0)
        : juce::AudioIODeviceType (typeName), outputs (numOutputs), rejectAbove (rejectAboveChannels) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool) const override        { return { deviceName() }; }
    int getDefaultDeviceIndex (bool) const override               { return 0; }
    bool hasSeparateInputsAndOutputs() const override             { return false; }

    int getIndexOfDevice (juce::AudioIODevice* d, bool) const override
    {
        return d != nullptr && d->getName() == deviceName() ? 0 : -1;
    }

    juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String& inputName) override
    {
        if (outputName != deviceName() && inputName != deviceName())
            return nullptr;

        return new FakeDevice (deviceName(), getTypeName(), outputs, rejectAbove, &openHistory);
    }

    juce::String deviceName() const                               { return "Fake " + getTypeName(); }

    int outputs;
    int rejectAbove;                // devices created from now on refuse more channels than this (0 = never)
    std::vector<int> openHistory;   // channel count of every open request, in order (outlives the devices)
};

class OutputLimitTests : public juce::UnitTest
{
public:
    OutputLimitTests() : juce::UnitTest ("Output limit (ASIO only multichannel)", "Enqueue") {}

    static FakeDevice* currentFake (AudioEngine& engine)
    {
        return dynamic_cast<FakeDevice*> (engine.getDeviceManager().getCurrentAudioDevice());
    }

    static int activeOutputs (AudioEngine& engine)
    {
        auto* device = currentFake (engine);
        return device != nullptr ? device->getActiveOutputChannels().countNumberOfSetBits() : -1;
    }

    static bool firstPairOnly (AudioEngine& engine)
    {
        auto* device = currentFake (engine);
        return device != nullptr && device->getActiveOutputChannels().countNumberOfSetBits() == 2
            && device->getActiveOutputChannels()[0] && device->getActiveOutputChannels()[1];
    }

    static std::unique_ptr<juce::XmlElement> savedState (const juce::String& type, const juce::String& device, const juce::String& outBits)
    {
        auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
        xml->setAttribute ("deviceType", type);
        xml->setAttribute ("audioOutputDeviceName", device);
        xml->setAttribute ("audioInputDeviceName", "");
        xml->setAttribute ("audioDeviceRate", 48000.0);
        xml->setAttribute ("audioDeviceBufferSize", 512);

        if (outBits.isNotEmpty())
            xml->setAttribute ("audioDeviceOutChans", outBits);

        return xml;
    }

    static juce::String history (const std::vector<int>& h)
    {
        juce::String s;

        for (auto n : h)
            s << n << ' ';

        return s.trim();
    }

    /** A 48 kHz sine file (the fake device's rate). */
    juce::File writeSine (const juce::File& dir, const juce::String& fileName, double seconds, float amplitude, int channels)
    {
        const double rate = 48000.0;
        const auto file = dir.getChildFile (fileName);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr);

        if (stream == nullptr)
            return {};

        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (rate)
                                                       .withNumChannels (channels)
                                                       .withBitsPerSample (16));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int numSamples = (int) (seconds * rate);
        juce::AudioBuffer<float> buffer (channels, numSamples);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / rate));

        expect (writer->writeFromAudioSampleBuffer (buffer, 0, numSamples));
        return file;
    }

    void runTest() override
    {
        beginTest ("a Windows Audio device with 8 outputs opens with 1-2 only");
        {
            AudioEngine engine;
            auto type = std::make_unique<FakeType> ("Windows Audio", 8);
            auto& opens = type->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const auto error = engine.initialise (nullptr);
            expect (error.isEmpty(), error);

            expect (! engine.currentTypeAllowsMultichannel());
            expectEquals (engine.outputLimitForCurrentType(), AudioEngine::stereoOnlyOutputs);
            expect (firstPairOnly (engine));
            expectEquals (engine.getDeviceManager().getAudioDeviceSetup().outputChannels.countNumberOfSetBits(), 2);
            expectEquals (engine.getNumDeviceOutputs(), 2);
            expectEquals (engine.getOutputChannelLimit(), 2);
            expectEquals (history (opens), juce::String ("8 2"));   // no saved state: the wide default open, then the trim
        }

        beginTest ("an ASIO device with 8 outputs keeps all of them");
        {
            AudioEngine engine;
            auto type = std::make_unique<FakeType> ("ASIO", 8);
            auto& opens = type->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const auto error = engine.initialise (nullptr);
            expect (error.isEmpty(), error);

            expect (engine.currentTypeAllowsMultichannel());
            expectEquals (engine.outputLimitForCurrentType(), AudioEngine::maxDeviceOutputs);
            expectEquals (activeOutputs (engine), 8);
            expectEquals (engine.getNumDeviceOutputs(), 8);
            expectEquals (engine.getOutputChannelLimit(), AudioEngine::maxDeviceOutputs);
            expectEquals (history (opens), juce::String ("8"));   // no needless reopen
        }

        beginTest ("a saved 8-channel Windows Audio setup opens once, directly with 1-2");
        {
            AudioEngine engine;
            auto type = std::make_unique<FakeType> ("Windows Audio", 8);
            auto& opens = type->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const auto error = engine.initialise (savedState ("Windows Audio", "Fake Windows Audio", "11111111").get());
            expect (error.isEmpty(), error);

            expect (firstPairOnly (engine));
            expectEquals (history (opens), juce::String ("2"));   // the saved state is normalised before the first open
        }

        beginTest ("a saved Windows Audio setup without explicit channels also opens directly with 1-2");
        {
            AudioEngine engine;
            auto type = std::make_unique<FakeType> ("Windows Audio (Exclusive Mode)", 8);
            auto& opens = type->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const auto error = engine.initialise (savedState ("Windows Audio (Exclusive Mode)", "Fake Windows Audio (Exclusive Mode)", {}).get());
            expect (error.isEmpty(), error);

            expect (firstPairOnly (engine));
            expectEquals (history (opens), juce::String ("2"));
        }

        beginTest ("a saved 8-channel ASIO setup is honoured");
        {
            AudioEngine engine;
            auto type = std::make_unique<FakeType> ("ASIO", 8);
            auto& opens = type->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const auto error = engine.initialise (savedState ("ASIO", "Fake ASIO", "11111111").get());
            expect (error.isEmpty(), error);

            expectEquals (activeOutputs (engine), 8);
            expectEquals (history (opens), juce::String ("8"));
        }

        beginTest ("a saved ASIO setup with an explicit 1-2 stays at 1-2 (the user's choice)");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("ASIO", 8));
            const auto error = engine.initialise (savedState ("ASIO", "Fake ASIO", "11").get());
            expect (error.isEmpty(), error);
            expect (firstPairOnly (engine));
        }

        beginTest ("a Windows Audio device that refuses more than two channels still opens (stereo retry)");
        {
            AudioEngine engine;
            auto type = std::make_unique<FakeType> ("Windows Audio (Exclusive Mode)", 8, 2);
            auto& opens = type->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            const auto error = engine.initialise (nullptr);
            expect (error.isEmpty(), error);

            expect (firstPairOnly (engine));
            expect (! opens.empty() && opens.back() == 2, history (opens));
            expect (opens.size() >= 2, history (opens));   // the wide attempt(s) failed first
        }

        beginTest ("an ASIO device that refuses the wide request reports the error (no stereo fallback there)");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("ASIO", 8, 2));
            const auto error = engine.initialise (nullptr);
            expect (error.isNotEmpty());
            expect (currentFake (engine) == nullptr);
        }

        beginTest ("choosing outputs 3-4 on Windows Audio is put back to 1-2 by the enforcement");
        {
            AudioEngine engine;
            auto type = std::make_unique<FakeType> ("Windows Audio", 8);
            auto& opens = type->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            engine.initialise (nullptr);

            auto setup = engine.getDeviceManager().getAudioDeviceSetup();
            setup.useDefaultOutputChannels = false;
            setup.outputChannels.clear();
            setup.outputChannels.setRange (2, 2, true);
            expect (engine.getDeviceManager().setAudioDeviceSetup (setup, true).isEmpty());

            auto* device = currentFake (engine);
            expect (device != nullptr && device->getActiveOutputChannels()[2]);   // the manager did open 3-4...
            expect (engine.enforceOutputLimit().isEmpty());                          // ...and the change callback trims it
            expect (firstPairOnly (engine));

            // a second call finds nothing to do (no reopen loop)
            const auto before = opens.size();
            expect (engine.enforceOutputLimit().isEmpty());
            expectEquals ((int) opens.size(), (int) before);
        }

        beginTest ("switching ASIO -> Windows Audio trims, and back to ASIO widens again");
        {
            AudioEngine engine;
            auto asio = std::make_unique<FakeType> ("ASIO", 8);
            auto wasapi = std::make_unique<FakeType> ("Windows Audio", 8);
            auto& asioOpens = asio->openHistory;
            auto& wasapiOpens = wasapi->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (asio));      // first type with devices = the default
            engine.getDeviceManager().addAudioDeviceType (std::move (wasapi));
            expect (engine.initialise (nullptr).isEmpty());
            expectEquals (activeOutputs (engine), 8);

            // the settings dialog's type combo does exactly this
            engine.getDeviceManager().setCurrentAudioDeviceType ("Windows Audio", true);
            auto* device = currentFake (engine);
            expect (device != nullptr && device->getTypeName() == "Windows Audio");

            // before the (asynchronous) trim lands the device runs wide, but the engine already mixes to 1-2 only
            expectEquals (activeOutputs (engine), 8);
            expectEquals (engine.getNumDeviceOutputs(), 2);
            expectEquals (engine.getOutputChannelLimit(), 2);

            expect (engine.enforceOutputLimit().isEmpty());
            expect (firstPairOnly (engine));
            expectEquals (history (wasapiOpens), juce::String ("8 2"));

            // back to ASIO: JUCE now defaults to the last explicit count (2), the policy restores every channel
            engine.getDeviceManager().setCurrentAudioDeviceType ("ASIO", true);
            device = currentFake (engine);
            expect (device != nullptr && device->getTypeName() == "ASIO");
            expect (engine.enforceOutputLimit().isEmpty());
            expectEquals (activeOutputs (engine), 8);
            expectEquals (engine.getNumDeviceOutputs(), 8);
            expectEquals (engine.getOutputChannelLimit(), AudioEngine::maxDeviceOutputs);
            expectEquals (history (asioOpens), juce::String ("8 2 8"));

            // a further call changes nothing
            const auto before = asioOpens.size();
            expect (engine.enforceOutputLimit().isEmpty());
            expectEquals ((int) asioOpens.size(), (int) before);
        }

        beginTest ("switching to a non-ASIO type whose wide open fails still ends on a stereo device; 'none' stays none");
        {
            AudioEngine engine;
            auto asio = std::make_unique<FakeType> ("ASIO", 8);
            auto exclusive = std::make_unique<FakeType> ("Windows Audio (Exclusive Mode)", 8, 2);
            auto& exclusiveOpens = exclusive->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (asio));
            engine.getDeviceManager().addAudioDeviceType (std::move (exclusive));
            expect (engine.initialise (nullptr).isEmpty());
            expectEquals (activeOutputs (engine), 8);

            engine.getDeviceManager().setCurrentAudioDeviceType ("Windows Audio (Exclusive Mode)", true);   // JUCE drops the open error
            expect (currentFake (engine) == nullptr);

            expect (engine.enforceOutputLimit().isEmpty());   // the change callback: a stereo retry on the type's default device
            auto* device = currentFake (engine);
            expect (device != nullptr && device->getTypeName() == "Windows Audio (Exclusive Mode)");
            expect (firstPairOnly (engine));
            expectEquals (history (exclusiveOpens), juce::String ("8 2"));

            // the "none" choice within a type is respected: no device, no retry
            auto none = engine.getDeviceManager().getAudioDeviceSetup();
            none.outputDeviceName.clear();
            none.inputDeviceName.clear();
            expect (engine.getDeviceManager().setAudioDeviceSetup (none, true).isEmpty());
            expect (currentFake (engine) == nullptr);
            expect (engine.enforceOutputLimit().isEmpty());
            expect (currentFake (engine) == nullptr);
            expectEquals (history (exclusiveOpens), juce::String ("8 2"));
        }

        beginTest ("an ASIO widening the driver refuses rolls back to the channels that were running");
        {
            AudioEngine engine;
            auto asio = std::make_unique<FakeType> ("ASIO", 8);
            auto wasapi = std::make_unique<FakeType> ("Windows Audio", 8);
            auto* asioType = asio.get();
            auto& asioOpens = asio->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (asio));
            engine.getDeviceManager().addAudioDeviceType (std::move (wasapi));
            expect (engine.initialise (nullptr).isEmpty());

            engine.getDeviceManager().setCurrentAudioDeviceType ("Windows Audio", true);
            expect (engine.enforceOutputLimit().isEmpty());   // JUCE's default count is 2 from here on

            asioType->rejectAbove = 2;                          // from now on the ASIO "driver" opens two channels at most
            engine.getDeviceManager().setCurrentAudioDeviceType ("ASIO", true);
            expectEquals (activeOutputs (engine), 2);
            expect (engine.enforceOutputLimit().isEmpty());     // the widening fails, the rollback restores 1-2
            expect (firstPairOnly (engine));
            expect (currentFake (engine) != nullptr && currentFake (engine)->getTypeName() == "ASIO");
            expectEquals (history (asioOpens), juce::String ("8 2 8 2"));

            const auto before = asioOpens.size();               // explicit now: not tried again
            expect (engine.enforceOutputLimit().isEmpty());
            expectEquals ((int) asioOpens.size(), (int) before);
        }

        beginTest ("a wide non-ASIO device gets silence beyond 1-2 from the callback (64 channels: scratch path)");
        {
            AudioEngine engine;
            auto asio = std::make_unique<FakeType> ("ASIO", 2);
            auto wide = std::make_unique<FakeType> ("Windows Audio", 64);
            engine.getDeviceManager().addAudioDeviceType (std::move (asio));
            engine.getDeviceManager().addAudioDeviceType (std::move (wide));
            expect (engine.initialise (nullptr).isEmpty());
            engine.getDeviceManager().setCurrentAudioDeviceType ("Windows Audio", true);   // runs with 64 channels until trimmed

            auto* device = currentFake (engine);
            expect (device != nullptr && device->callback != nullptr && activeOutputs (engine) == 64);

            if (device != nullptr && device->callback != nullptr)
            {
                const int numSamples = 512;
                juce::AudioBuffer<float> out (64, numSamples);

                for (int ch = 0; ch < 64; ++ch)
                    juce::FloatVectorOperations::fill (out.getWritePointer (ch), 1.0f, numSamples);

                juce::AudioIODeviceCallbackContext context;
                device->callback->audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 64, numSamples, context);

                float maxBeyond = 0.0f;

                for (int ch = 2; ch < 64; ++ch)
                    maxBeyond = juce::jmax (maxBeyond, out.getMagnitude (ch, 0, numSamples));

                expectEquals (maxBeyond, 0.0f);
                expectEquals (out.getMagnitude (0, 0, numSamples), 0.0f);   // nothing plays: 1-2 are rendered silence, not the fill
            }
        }

        beginTest ("a non-ASIO device left on physical 3-4 stays silent while a cue runs, and plays after the trim");
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("gocue_outputlimit_" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());
            const auto tone = writeSine (dir, "tone.wav", 2.0, 0.5f, 2);

            {
                AudioEngine engine (0);   // synchronous reads: deterministic
                engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("Windows Audio", 8));
                expect (engine.initialise (nullptr).isEmpty());

                // a set the policy never produces (a hand-edited state): physical 3-4 only, put in behind the enforcement
                auto setup = engine.getDeviceManager().getAudioDeviceSetup();
                setup.useDefaultOutputChannels = false;
                setup.outputChannels.clear();
                setup.outputChannels.setRange (2, 2, true);
                expect (engine.getDeviceManager().setAudioDeviceSetup (setup, true).isEmpty());

                auto* device = currentFake (engine);
                expect (device != nullptr && device->callback != nullptr && device->getActiveOutputChannels()[2]);

                Cue cue;
                cue.name = "tone";
                cue.file = tone;
                juce::String error;
                expect (engine.play (cue, &error), error);

                if (device != nullptr && device->callback != nullptr)
                {
                    const int numSamples = 512;
                    juce::AudioBuffer<float> out (2, numSamples);   // JUCE packs the two active channels to index 0-1
                    juce::AudioIODeviceCallbackContext context;

                    auto drive = [&]
                    {
                        for (int ch = 0; ch < 2; ++ch)
                            juce::FloatVectorOperations::fill (out.getWritePointer (ch), 1.0f, numSamples);

                        device->callback->audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 2, numSamples, context);
                    };

                    for (int i = 0; i < 10; ++i)
                        drive();

                    expectEquals (out.getMagnitude (0, 0, numSamples), 0.0f);   // nothing reaches physical 3-4...
                    expectEquals (out.getMagnitude (1, 0, numSamples), 0.0f);
                    const auto playing = engine.getPlayingCues();
                    expect (! playing.empty() && playing[0].positionSeconds > 0.05);   // ...but the cue keeps running

                    expect (engine.enforceOutputLimit().isEmpty());   // the change callback's trim: the device restarts on 1-2
                    device = currentFake (engine);
                    expect (device != nullptr && device->callback != nullptr && firstPairOnly (engine));

                    if (device != nullptr && device->callback != nullptr)
                    {
                        for (int i = 0; i < 4; ++i)
                            drive();

                        expectGreaterThan (out.getRMSLevel (0, 0, numSamples), 0.2f);   // 0.5 amp sine: rms 0.35
                        expectGreaterThan (out.getRMSLevel (1, 0, numSamples), 0.2f);
                    }
                }
            }

            dir.deleteRecursively();
        }

        beginTest ("the saved state after a trim reopens once at 1-2 on the next start");
        {
            std::unique_ptr<juce::XmlElement> saved;

            {
                AudioEngine engine;
                engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("Windows Audio", 8));
                engine.initialise (nullptr);

                // the settings dialog commits a wide default setup as chosen, the trim follows
                auto setup = engine.getDeviceManager().getAudioDeviceSetup();
                setup.useDefaultOutputChannels = true;
                setup.outputChannels.clear();
                setup.outputChannels.setRange (0, 8, true);
                expect (engine.getDeviceManager().setAudioDeviceSetup (setup, true).isEmpty());
                expect (engine.enforceOutputLimit().isEmpty());
                expect (firstPairOnly (engine));
                saved = engine.getDeviceManager().createStateXml();
            }

            expect (saved != nullptr);

            if (saved != nullptr)
            {
                AudioEngine engine;
                auto type = std::make_unique<FakeType> ("Windows Audio", 8);
                auto& opens = type->openHistory;
                engine.getDeviceManager().addAudioDeviceType (std::move (type));
                expect (engine.initialise (saved.get()).isEmpty());
                expect (firstPairOnly (engine));
                expectEquals (history (opens), juce::String ("2"));
            }
        }

        beginTest ("a plain stereo Windows Audio device is left alone");
        {
            AudioEngine engine;
            auto type = std::make_unique<FakeType> ("Windows Audio", 2);
            auto& opens = type->openHistory;
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            engine.initialise (nullptr);

            expect (firstPairOnly (engine));
            expectEquals (history (opens), juce::String ("2"));
        }

        beginTest ("no device: the enforcement is a no-op");
        {
            AudioEngine engine;
            expect (engine.enforceOutputLimit().isEmpty());
            expect (! engine.currentTypeAllowsMultichannel());
        }

        beginTest ("normaliseDeviceState: non-ASIO gets an explicit 1-2, ASIO is untouched, an untyped state goes by its device");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("ASIO", 8));
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("Windows Audio", 8));

            const auto wasapi = engine.normaliseDeviceState (savedState ("Windows Audio", "X", "11111111").get());
            expect (wasapi != nullptr && wasapi->getStringAttribute ("audioDeviceOutChans") == "11");

            const auto exclusive = engine.normaliseDeviceState (savedState ("Windows Audio (Exclusive Mode)", "X", {}).get());
            expect (exclusive != nullptr && exclusive->getStringAttribute ("audioDeviceOutChans") == "11");

            const auto asio = engine.normaliseDeviceState (savedState ("ASIO", "X", "11111111").get());
            expect (asio != nullptr && asio->getStringAttribute ("audioDeviceOutChans") == "11111111");

            const auto untypedWasapi = engine.normaliseDeviceState (savedState ({}, "Fake Windows Audio", "1111").get());
            expect (untypedWasapi != nullptr && untypedWasapi->getStringAttribute ("audioDeviceOutChans") == "11");

            const auto untypedAsio = engine.normaliseDeviceState (savedState ({}, "Fake ASIO", "1111").get());
            expect (untypedAsio != nullptr && untypedAsio->getStringAttribute ("audioDeviceOutChans") == "1111");

            const auto unknown = engine.normaliseDeviceState (savedState ({}, "X", "1111").get());
            expect (unknown != nullptr && unknown->getStringAttribute ("audioDeviceOutChans") == "1111");

            expect (engine.normaliseDeviceState (nullptr) == nullptr);
        }
    }
};

static OutputLimitTests outputLimitTests;

} // namespace gocue::tests
