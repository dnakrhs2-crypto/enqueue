#include "MixEngine.h"
#include "LiveMixSettings.h"
#include "MixDocument.h"
#include "ui/DeviceFormatText.h"
#include "ui/SettingsDialog.h"
#include "ui/TopBar.h"
#include "ui/LiveMixLookAndFeel.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

using namespace gocue::livemix;

/** Opt-in (LIVEMIX_REAL_DEVICE_TEST=1): this PC's real Windows audio endpoints through MixEngine - what every Windows
    device type reports for every bit-depth choice - and, with LIVEMIX_UI_SCREENSHOT_DIR, the settings dialog and the
    top bar drawn from them. LIVEMIX_REAL_INPUT / LIVEMIX_REAL_OUTPUT pick the endpoints by a part of their names.
    Without the variable it does nothing: the suite never depends on the PC's hardware. */
class LiveMixRealDeviceTests : public juce::UnitTest
{
public:
    LiveMixRealDeviceTests() : juce::UnitTest ("LiveMix real Windows devices (opt-in)", "LiveMix") {}

    void runTest() override
    {
        beginTest ("real endpoints report their formats and take an exclusive choice exactly when the driver accepts it");
        auto env = [] (const char* name, const char* fallback) { return juce::SystemStats::getEnvironmentVariable (name, fallback); };

        if (env ("LIVEMIX_REAL_DEVICE_TEST", "") != "1")
        {
            expect (true);
            return;
        }

        const auto inputPart = env ("LIVEMIX_REAL_INPUT", "StreamLine"), outputPart = env ("LIVEMIX_REAL_OUTPUT", "S/PDIF");
        const juce::File shots (env ("LIVEMIX_UI_SCREENSHOT_DIR", ""));
        const auto directory = juce::File::createTempFile ("-lm-real-devices");
        expect (directory.deleteFile());
        expect (directory.createDirectory().wasOk());

        {
            LiveMixLookAndFeel lookAndFeel;
            MixEngine engine ("Local\\LiveMix.ObsAudio.realdevicetest");   // never the real OBS ring
            LiveMixSettings settings (directory);

            for (const auto* typeName : { "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" })
            {
                juce::AudioIODeviceType* type = nullptr;

                for (auto* t : engine.getDeviceManager().getAvailableDeviceTypes())
                    if (t->getTypeName() == typeName)
                        type = t;

                expect (type != nullptr, juce::String ("missing type ") + typeName);

                if (type == nullptr)
                    continue;

                type->scanForDevices();

                auto pick = [] (const juce::StringArray& names, const juce::String& part)
                {
                    for (const auto& name : names)
                        if (name.containsIgnoreCase (part))
                            return name;

                    return juce::String();
                };

                const auto input = pick (type->getDeviceNames (true), inputPart);
                const auto output = pick (type->getDeviceNames (false), outputPart);
                expect (input.isNotEmpty() && output.isNotEmpty(), "endpoints not found");

                if (input.isEmpty() || output.isEmpty())
                    continue;

                const bool exclusive = juce::String (typeName).contains ("Exclusive");
                const auto choices = exclusive ? juce::StringArray { "", "int16", "int24", "int32", "float32" }
                                               : juce::StringArray { "", "int24" };

                for (const auto& choice : choices)
                {
                    MixDevice wanted { typeName, input, output, 480, 48000.0 };
                    wanted.sampleFormat = choice;
                    const auto error = engine.openDevice (wanted);
                    const auto f = engine.getDeviceFormat();

                    logMessage (juce::String (typeName) + " choice=" + (choice.isEmpty() ? juce::String ("auto") : choice)
                                + (error.isEmpty() ? juce::String() : " ERROR " + error)
                                + " | in " + juce::String (f.inputBits) + (f.inputFloat ? "f" : "i") + " @" + juce::String (f.inputDeviceRate, 0)
                                + " accepts=" + juce::String (f.inputAccepted) + (f.inputRefused ? " REFUSED" : "")
                                + " | out " + juce::String (f.outputBits) + (f.outputFloat ? "f" : "i") + " @" + juce::String (f.outputDeviceRate, 0)
                                + " accepts=" + juce::String (f.outputAccepted) + (f.outputRefused ? " REFUSED" : "")
                                + " | split=" + juce::String (engine.isSplitMonitor() ? 1 : 0)
                                + " | status=\"" + TopBar::buildStatusText (engine.getSampleRate(), engine.getBlockSize(), engine.getLatencyMs(), true, f) + "\"");

                    expect (error.isEmpty(), error);

                    if (error.isNotEmpty())
                        continue;

                    expectEquals (engine.getOpenDevice().sampleFormat, choice);

                    if (exclusive)
                    {
                        expect (f.kind == MixEngine::DeviceFormat::Kind::windowsExclusive);

                        auto checkDirection = [&] (int bits, bool isFloat, int accepted, bool refused, const juce::String& what)
                        {
                            expect (bits > 0, what + ": no stream format");

                            if (choice.isEmpty())
                            {
                                expect (! refused, what + ": automatic can never be refused");
                                return;
                            }

                            const int wantBits = choice == "int16" ? 16 : choice == "int24" ? 24 : 32;
                            const bool wantFloat = choice == "float32";
                            const int bit = choice == "int16" ? juce::WasapiFormatInfo::exclusiveInt16
                                          : choice == "int24" ? juce::WasapiFormatInfo::exclusiveInt24
                                          : choice == "int32" ? juce::WasapiFormatInfo::exclusiveInt32
                                                              : juce::WasapiFormatInfo::exclusiveFloat32;
                            const bool driverAccepts = (accepted & bit) != 0;
                            expect (driverAccepts == ! refused, what + ": refused flag disagrees with the driver's list");

                            if (driverAccepts)
                            {
                                expectEquals (bits, wantBits, what);
                                expect (isFloat == wantFloat, what + ": float flag");
                            }
                        };

                        checkDirection (f.inputBits, f.inputFloat, f.inputAccepted, f.inputRefused, "input");
                        checkDirection (f.outputBits, f.outputFloat, f.outputAccepted, f.outputRefused, "output");
                    }
                    else
                    {
                        expect (f.kind == MixEngine::DeviceFormat::Kind::windowsShared);
                        expect (f.inputBits > 0 && f.outputBits > 0, "the Windows format setting was not readable");
                        expect (! f.inputRefused && ! f.outputRefused);
                    }
                }

                if (shots.getFullPathName().isNotEmpty())
                {
                    if (exclusive)   // 24-bit: on this PC the capture card refuses it and the S/PDIF output takes it
                    {
                        MixDevice wanted { typeName, input, output, 480, 48000.0 };
                        wanted.sampleFormat = "int24";
                        expect (engine.openDevice (wanted).isEmpty());
                    }

                    expect (shots.createDirectory().wasOk());
                    const auto tag = exclusive ? juce::String ("exclusive") : juce::String (typeName).contains ("Low") ? juce::String ("lowlatency") : juce::String ("shared");

                    auto save = [&] (juce::Component& component, const juce::String& name)
                    {
                        juce::FileOutputStream image (shots.getChildFile (name));

                        if (image.openedOk())
                        {
                            expect (image.setPosition (0));
                            expect (image.truncate().wasOk());
                            expect (juce::PNGImageFormat().writeImageToStream (component.createComponentSnapshot (component.getLocalBounds()), image));
                        }
                    };

                    SettingsDialog::show (engine, settings, nullptr, {}, {}, {}, {}, {});
                    auto& desktop = juce::Desktop::getInstance();

                    for (int i = 0; i < desktop.getNumComponents(); ++i)
                        if (auto* dialog = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)); dialog != nullptr && dialog->getName() == ko ("설정"))
                            if (auto* viewport = dynamic_cast<juce::Viewport*> (dialog->getContentComponent()))
                                if (auto* content = viewport->getViewedComponent())
                                {
                                    content->setLookAndFeel (&lookAndFeel);
                                    save (*content, "real-settings-" + tag + ".png");
                                    content->setLookAndFeel (nullptr);
                                }

                    SettingsDialog::closeIfOpen();

                    MixDocument document (engine);
                    TopBar bar (document);
                    bar.setLookAndFeel (&lookAndFeel);
                    bar.setSize (1400, bar.preferredHeight (1400));
                    bar.setStatus (engine.getSampleRate(), engine.getBlockSize(), engine.getLatencyMs(), engine.getDspLoad(), true, engine.getDeviceFormat());
                    save (bar, "real-topbar-" + tag + ".png");
                    bar.setLookAndFeel (nullptr);
                }
            }

            engine.shutdown();
        }

        expect (directory.deleteRecursively());
    }
};

static LiveMixRealDeviceTests liveMixRealDeviceTests;

} // namespace gocue::tests
