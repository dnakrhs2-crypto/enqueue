#include "LiveMixSettings.h"

namespace gocue::tests
{
class LiveMixRecordingSettingsTests : public juce::UnitTest
{
public:
    LiveMixRecordingSettingsTests() : UnitTest ("LiveMix Recorder settings integration", "LiveMix") {}
    void runTest() override
    {
        using namespace livemix;
        const auto directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("LiveMixR8Settings-" + juce::Uuid().toString());
        directory.createDirectory();
        beginTest ("recording defaults and the four new keys preserve existing preferences");
        juce::String install;
        {
            LiveMixSettings settings (directory);
            const auto defaults = settings.getRecordingOptions();
            expect (defaults.format == recording::Format::wav); expectEquals ((int) defaults.mp3Kbps, 320);
            expect (defaults.directoryUtf8.empty());
            settings.setCollabNick ("old nick"); settings.setCollabPort (51234); settings.setCollabRate (44100);
            settings.setCollabMonitorVolume (0.35f); settings.setCollabMonitorOn (false);
            settings.setCollabRoute (1); settings.setCollabRouteFirst (4); settings.setWindowState ("window");
            install = settings.getCollabInstallId();
            settings.setRecordingOptions ({ recording::Format::mp3, 128, directory.getFullPathName().toStdString() });
            settings.setCollabPersonVolume (juce::String::fromUTF8 ("손님"), 0.6f); settings.saveIfNeeded();
        }
        {
            LiveMixSettings settings (directory);
            const auto options = settings.getRecordingOptions();
            expect (options.format == recording::Format::mp3); expectEquals ((int) options.mp3Kbps, 128);
            expectEquals (juce::String (options.directoryUtf8), directory.getFullPathName());
            expectEquals (settings.getCollabPersonVolume (juce::String::fromUTF8 ("손님")), 0.6f);
            expectEquals (settings.getCollabPersonVolume ("new person"), 0.35f);
            expectEquals (settings.getCollabNick(), juce::String ("old nick")); expectEquals (settings.getCollabPort(), 51234);
            expectEquals (settings.getCollabRate(), 44100); expectEquals (settings.getCollabInstallId(), install);
            expectEquals (settings.getCollabMonitorVolume(), 0.35f); expect (! settings.getCollabMonitorOn());
            expectEquals (settings.getCollabRoute(), 1); expectEquals (settings.getCollabRouteFirst(), 4);
            expectEquals (settings.getWindowState(), juce::String ("window"));
            for (auto kbps : { 128u, 256u, 320u })
            {
                settings.setRecordingOptions ({ recording::Format::wav, kbps, {} });
                expectEquals ((int) settings.getRecordingOptions().mp3Kbps, (int) kbps);
                expect (settings.getRecordingOptions().format == recording::Format::wav);
            }
            settings.saveIfNeeded();
        }
        beginTest ("nickname volume LRU retains the 32 most recently used names");
        {
            LiveMixSettings settings (directory);
            for (int i = 0; i < 32; ++i) settings.setCollabPersonVolume ("name" + juce::String (i), 0.2f);
            settings.setCollabPersonVolume ("name0", settings.getCollabPersonVolume ("name0"));
            settings.setCollabPersonVolume ("name32", 0.8f);
            expectEquals (settings.getCollabPersonVolume ("name0"), 0.2f);
            expectEquals (settings.getCollabPersonVolume ("name1"), 0.35f);
            expectEquals (settings.getCollabPersonVolume ("name32"), 0.8f);
            settings.setCollabPersonVolume ("name32", 2.0f); expectEquals (settings.getCollabPersonVolume ("name32"), 1.0f);
            settings.setCollabPersonVolume ("name32", -1.0f); expectEquals (settings.getCollabPersonVolume ("name32"), 0.0f);
            settings.saveIfNeeded();
        }
        auto xml = juce::XmlDocument::parse (directory.getChildFile ("LiveMix.settings"));
        expect (xml != nullptr);
        if (xml)
        {
            for (const auto* key : { "recordingFormat", "recordingMp3Kbps", "recordingDirectory", "collabPersonVolumes" })
            {
                auto* entry = xml->getChildByAttribute ("name", key); expect (entry != nullptr, key);
                if (entry && juce::String (key) == "collabPersonVolumes")
                    expectEquals (juce::JSON::parse (entry->getStringAttribute ("val")).size(), 32);
            }
        }
        directory.deleteRecursively(); // the unique temporary directory created above
    }
};
static LiveMixRecordingSettingsTests liveMixRecordingSettingsTests;
}
