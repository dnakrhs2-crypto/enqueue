#include <juce_audio_devices/juce_audio_devices.h>

namespace gocue::tests
{

/** Not a test of GoCue: a probe of this machine's audio devices, run only with GOCUE_PROBE_DEVICES=1.
    Prints, per device type, every output device with its channel count, and tries to open the WASAPI
    devices with all their channels (the exclusive-mode multichannel check). */
class DeviceProbeTests : public juce::UnitTest
{
public:
    DeviceProbeTests() : juce::UnitTest ("DeviceProbe", "GoCue") {}

    void runTest() override
    {
        beginTest ("device probe");

        if (juce::SystemStats::getEnvironmentVariable ("GOCUE_PROBE_DEVICES", "").isEmpty())
        {
            logMessage ("skipped (set GOCUE_PROBE_DEVICES=1)");
            return;
        }

        juce::AudioDeviceManager manager;

        for (auto* type : manager.getAvailableDeviceTypes())
        {
            const auto typeName = type->getTypeName();

            if (! typeName.contains ("Windows Audio"))
                continue;

            type->scanForDevices();

            for (const auto& name : type->getDeviceNames (false))
            {
                std::unique_ptr<juce::AudioIODevice> device (type->createDevice (name, {}));

                if (device == nullptr)
                {
                    logMessage ("[" + typeName + "] " + name + ": createDevice failed");
                    continue;
                }

                const int outs = device->getOutputChannelNames().size();
                juce::String line = "[" + typeName + "] " + name + ": outputs=" + juce::String (outs)
                                    + " rates=" + juce::String (device->getAvailableSampleRates().size());

                if (outs > 0)
                {
                    juce::BigInteger outputs;
                    outputs.setRange (0, outs, true);
                    const auto error = device->open ({}, outputs, device->getCurrentSampleRate() > 0 ? device->getCurrentSampleRate() : 48000.0,
                                                     device->getDefaultBufferSize());
                    line << " open(" << outs << "ch)=" << (error.isEmpty() ? juce::String ("ok, active outputs=") + juce::String (device->getActiveOutputChannels().countNumberOfSetBits())
                                                                          : "ERROR " + error);
                    device->close();
                }

                logMessage (line);
            }
        }

        expect (true);
    }
};

static DeviceProbeTests deviceProbeTests;

} // namespace gocue::tests
