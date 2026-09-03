#include "audio/AudioEngine.h"

#include <juce_audio_devices/juce_audio_devices.h>

namespace gocue::tests
{

/** An in-memory device with N outputs: no hardware, no threads. */
class FakeDevice : public juce::AudioIODevice
{
public:
    FakeDevice (const juce::String& name, const juce::String& typeName, int numOutputs)
        : juce::AudioIODevice (name, typeName), outputs (numOutputs) {}

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
        activeOutputs = outputChannels;
        activeOutputs.setRange (outputs, 256, false);   // only the channels this device has
        opened = true;
        ++openCount;
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
    int openCount = 0;
    juce::BigInteger activeOutputs;
    bool opened = false;
    juce::AudioIODeviceCallback* callback = nullptr;
};

/** One device type with one device; the type name decides whether the engine treats it as ASIO. */
class FakeType : public juce::AudioIODeviceType
{
public:
    FakeType (const juce::String& typeName, int numOutputs) : juce::AudioIODeviceType (typeName), outputs (numOutputs) {}

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

        return new FakeDevice (deviceName(), getTypeName(), outputs);
    }

    juce::String deviceName() const                               { return "Fake " + getTypeName(); }

    int outputs;
};

class OutputLimitTests : public juce::UnitTest
{
public:
    OutputLimitTests() : juce::UnitTest ("Output limit (ASIO only multichannel)", "GoCue") {}

    static FakeDevice* currentFake (AudioEngine& engine)
    {
        return dynamic_cast<FakeDevice*> (engine.getDeviceManager().getCurrentAudioDevice());
    }

    static std::unique_ptr<juce::XmlElement> savedState (const juce::String& type, const juce::String& device, const juce::String& outBits)
    {
        auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
        xml->setAttribute ("deviceType", type);
        xml->setAttribute ("audioOutputDeviceName", device);
        xml->setAttribute ("audioInputDeviceName", "");
        xml->setAttribute ("audioDeviceRate", 48000.0);
        xml->setAttribute ("audioDeviceBufferSize", 512);
        xml->setAttribute ("audioDeviceOutChans", outBits);
        return xml;
    }

    void runTest() override
    {
        beginTest ("a Windows Audio device with 8 outputs opens with 1-2 only");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("Windows Audio", 8));
            const auto error = engine.initialise (nullptr);
            expect (error.isEmpty(), error);

            auto* device = currentFake (engine);
            expect (device != nullptr);

            if (device != nullptr)
            {
                expect (! engine.currentTypeAllowsMultichannel());
                expectEquals (engine.outputLimitForCurrentType(), AudioEngine::stereoOnlyOutputs);
                expectEquals (device->getActiveOutputChannels().countNumberOfSetBits(), 2);
                expect (device->getActiveOutputChannels()[0] && device->getActiveOutputChannels()[1]);
                expectEquals (engine.getDeviceManager().getAudioDeviceSetup().outputChannels.countNumberOfSetBits(), 2);
            }
        }

        beginTest ("an ASIO device with 8 outputs keeps all of them");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("ASIO", 8));
            const auto error = engine.initialise (nullptr);
            expect (error.isEmpty(), error);

            auto* device = currentFake (engine);
            expect (device != nullptr);

            if (device != nullptr)
            {
                expect (engine.currentTypeAllowsMultichannel());
                expectEquals (engine.outputLimitForCurrentType(), AudioEngine::maxDeviceOutputs);
                expectEquals (device->getActiveOutputChannels().countNumberOfSetBits(), 8);
                expectEquals (device->openCount, 1);   // no needless reopen
            }
        }

        beginTest ("a saved 8-channel Windows Audio setup comes back as 1-2");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("Windows Audio", 8));
            const auto error = engine.initialise (savedState ("Windows Audio", "Fake Windows Audio", "11111111").get());
            expect (error.isEmpty(), error);

            auto* device = currentFake (engine);
            expect (device != nullptr);

            if (device != nullptr)
            {
                expectEquals (device->getActiveOutputChannels().countNumberOfSetBits(), 2);
                expect (device->getActiveOutputChannels()[0] && device->getActiveOutputChannels()[1]);
            }
        }

        beginTest ("a saved 8-channel ASIO setup is honoured");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("ASIO", 8));
            const auto error = engine.initialise (savedState ("ASIO", "Fake ASIO", "11111111").get());
            expect (error.isEmpty(), error);

            auto* device = currentFake (engine);
            expect (device != nullptr);

            if (device != nullptr)
                expectEquals (device->getActiveOutputChannels().countNumberOfSetBits(), 8);
        }

        beginTest ("choosing outputs 3-4 on Windows Audio is put back to 1-2 by the enforcement");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("Windows Audio", 8));
            engine.initialise (nullptr);

            auto setup = engine.getDeviceManager().getAudioDeviceSetup();
            setup.useDefaultOutputChannels = false;
            setup.outputChannels.clear();
            setup.outputChannels.setRange (2, 2, true);   // what the selector's stereo-pair click produces
            expect (engine.getDeviceManager().setAudioDeviceSetup (setup, true).isEmpty());

            auto* device = currentFake (engine);
            expect (device != nullptr);

            if (device != nullptr)
            {
                expect (device->getActiveOutputChannels()[2]);   // the manager did open 3-4...
                engine.enforceOutputLimit();                     // ...and the change callback trims it
                device = currentFake (engine);
                expect (device != nullptr);

                if (device != nullptr)
                {
                    expectEquals (device->getActiveOutputChannels().countNumberOfSetBits(), 2);
                    expect (device->getActiveOutputChannels()[0] && device->getActiveOutputChannels()[1]);
                }
            }

            // a second call finds nothing to do (no reopen loop)
            const int opens = currentFake (engine) != nullptr ? currentFake (engine)->openCount : -1;
            engine.enforceOutputLimit();
            expectEquals (currentFake (engine) != nullptr ? currentFake (engine)->openCount : -2, opens);
        }

        beginTest ("a plain stereo Windows Audio device is left alone");
        {
            AudioEngine engine;
            engine.getDeviceManager().addAudioDeviceType (std::make_unique<FakeType> ("Windows Audio", 2));
            engine.initialise (nullptr);

            auto* device = currentFake (engine);
            expect (device != nullptr);

            if (device != nullptr)
            {
                expectEquals (device->getActiveOutputChannels().countNumberOfSetBits(), 2);
                expectEquals (device->openCount, 1);
            }
        }

        beginTest ("no device: the enforcement is a no-op");
        {
            AudioEngine engine;
            engine.enforceOutputLimit();
            expect (! engine.currentTypeAllowsMultichannel());
        }
    }
};

static OutputLimitTests outputLimitTests;

} // namespace gocue::tests
