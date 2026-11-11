#include "BundledPlugins.h"

#include <windows.h>

namespace gocue::tests
{
namespace
{
    using namespace livemix;

    struct Scratch
    {
        juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("livemix-bundled-" + juce::Uuid().toString());
        Scratch() { directory.createDirectory(); }
        ~Scratch() { directory.deleteRecursively(); }
    };

    bool copyBundle (const juce::File& source, const juce::File& target)
    {
        if (! source.copyDirectoryTo (target)) return false;
        if (source.getFileName() == target.getFileName()) return true;
        const auto binaries = target.getChildFile ("Contents/x86_64-win");
        return binaries.getChildFile (source.getFileName()).moveFileTo (binaries.getChildFile (target.getFileName()));
    }

    struct Fixture
    {
        Scratch scratch;
        juce::File directory = scratch.directory.getChildFile ("VST3");
        LiveMixSettings settings { scratch.directory.getChildFile ("settings") };
        PluginHost host;
        juce::File bundle = directory.getChildFile ("LiveMix Test Shell.vst3");
        Fixture() { directory.createDirectory(); }
        void stamp (const juce::String& id)
        {
            directory.getChildFile ("bundled_plugins.json").replaceWithText ("{\"schema\":1,\"id\":\"" + id + "\"}");
        }
        BundledPluginsResult sync()
        {
            return syncBundledPlugins (host, settings, directory, juce::File (LM_SCAN_WORKER_EXE));
        }
    };

    struct SettingsEnvironment
    {
        juce::String old = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_SETTINGS_DIR", {});
        explicit SettingsEnvironment (const juce::File& directory)
        { SetEnvironmentVariableW (L"LIVEMIX_SETTINGS_DIR", directory.getFullPathName().toWideCharPointer()); }
        ~SettingsEnvironment()
        { SetEnvironmentVariableW (L"LIVEMIX_SETTINGS_DIR", old.isEmpty() ? nullptr : old.toWideCharPointer()); }
    };
}

class LiveMixBundledPluginsTests : public juce::UnitTest
{
public:
    LiveMixBundledPluginsTests() : UnitTest ("LiveMix bundled plugins", "LiveMix") {}

    void runTest() override
    {
        beginTest ("first registration scans in the worker; the same stamp does no scans");
        Fixture f;
        expect (copyBundle (juce::File (LM_VST3_SHELL), f.bundle));
        const auto other = f.directory.getChildFile ("Second Shell.vst3");
        expect (copyBundle (juce::File (LM_VST3_SHELL), other));
        f.stamp ("one");
        auto result = f.sync();
        expect (result.allRegistered);
        expectEquals (result.scanned, 2);
        expectEquals (f.host.getKnownPlugins().getNumTypes(), 6);
        expectEquals (f.settings.getBundledPluginsStamp(), juce::String ("one"));
        const auto scanLog = f.settings.getSettingsDirectory().getChildFile ("logs/plugin-scan.log");
        const auto firstScanLog = scanLog.loadFileAsString();
        expect (firstScanLog.contains ("bundled-plugins\ttotal\t2 scanned\t"));
        // Even an unavailable worker executable is irrelevant when every bundle is already listed.
        result = syncBundledPlugins (f.host, f.settings, f.directory, f.scratch.directory.getChildFile ("unused.exe"));
        expect (result.allRegistered);
        expectEquals (result.scanned, 0);
        expectEquals (scanLog.loadFileAsString(), firstScanLog);

        beginTest ("same stamp skips a blacklisted bundle; a new stamp retries all bundles");
        auto& list = f.host.getKnownPlugins();
        for (const auto& type : list.getTypes())
            if (juce::File (type.fileOrIdentifier).isAChildOf (other)) list.removeType (type);
        list.addToBlacklist (other.getFullPathName());
        result = f.sync();
        expectEquals (result.scanned, 0);
        expect (! result.allRegistered);
        expectEquals (scanLog.loadFileAsString(), firstScanLog);
        const auto outside = f.scratch.directory.getChildFile ("outside.vst3").getFullPathName();
        list.addToBlacklist (outside);
        f.stamp ("two");
        result = f.sync();
        expectEquals (result.scanned, 2);
        expect (result.allRegistered);
        expect (! list.getBlacklistedFiles().contains (other.getFullPathName()));
        expect (list.getBlacklistedFiles().contains (outside));

        beginTest ("new stamp removes retired bundles; unchanged stamp also removes missing bundles");
        expect (other.deleteRecursively());
        f.stamp ("three");
        result = f.sync();
        expectEquals (result.scanned, 1);
        expect (result.allRegistered);
        expectEquals (list.getNumTypes(), 3);
        expect (f.bundle.deleteRecursively());
        result = f.sync();
        expectEquals (result.scanned, 0);
        expectEquals (list.getNumTypes(), 0);

        beginTest ("an absent bundled directory does not write settings or scan logs");
        Scratch absent;
        LiveMixSettings absentSettings (absent.directory.getChildFile ("settings"));
        PluginHost absentHost;
        result = syncBundledPlugins (absentHost, absentSettings, absent.directory.getChildFile ("absent"));
        expect (result.allRegistered);
        expectEquals (result.scanned, 0);
        expect (absentSettings.getPluginList() == nullptr);
        expect (! absentSettings.getSettingsDirectory().getChildFile ("logs").exists());

        beginTest ("removing the bundled directory clears its entries and blacklist; reinstalling the same stamp registers every bundle");
        Fixture removed;
        const auto secondBundle = removed.directory.getChildFile ("Second Shell.vst3");
        expect (copyBundle (juce::File (LM_VST3_SHELL), removed.bundle));
        expect (copyBundle (juce::File (LM_VST3_SHELL), secondBundle));
        removed.stamp ("same-payload");
        expect (removed.sync().allRegistered);
        auto& removedList = removed.host.getKnownPlugins();
        auto outsideType = removedList.getTypes()[0];
        outsideType.fileOrIdentifier = LM_VST3_SHELL;
        removedList.addType (outsideType);
        removedList.addToBlacklist (removed.bundle.getFullPathName());
        removedList.addToBlacklist (secondBundle.getFullPathName());
        removedList.addToBlacklist (outside);
        removed.settings.setPluginList (removed.host.createKnownPluginsXml().get());
        const auto removedLog = removed.settings.getSettingsDirectory().getChildFile ("logs/plugin-scan.log");
        const auto beforeRemovalLog = removedLog.loadFileAsString();
        expect (removed.directory.deleteRecursively());
        result = removed.sync();
        expect (result.allRegistered);
        expectEquals (result.scanned, 0);
        expectEquals (removedList.getNumTypes(), 1);
        expectEquals (removedList.getTypes()[0].fileOrIdentifier, outsideType.fileOrIdentifier);
        expectEquals (removedList.getBlacklistedFiles().size(), 1);
        expect (removedList.getBlacklistedFiles().contains (outside));
        expectEquals (removed.settings.getBundledPluginsStamp(), juce::String ("same-payload"));
        expectEquals (removedLog.loadFileAsString(), beforeRemovalLog);
        PluginHost persisted;
        persisted.loadKnownPluginsFromXml (removed.settings.getPluginList().get());
        expectEquals (persisted.getKnownPlugins().getNumTypes(), 1);
        expectEquals (persisted.getKnownPlugins().getBlacklistedFiles().size(), 1);
        expect (persisted.getKnownPlugins().getBlacklistedFiles().contains (outside));
        expect (copyBundle (juce::File (LM_VST3_SHELL), removed.bundle));
        expect (copyBundle (juce::File (LM_VST3_SHELL), secondBundle));
        removed.stamp ("same-payload");
        result = removed.sync();
        expect (result.allRegistered);
        expectEquals (result.scanned, 2);
        expectEquals (removedList.getNumTypes(), 7);
        expect (removedList.getBlacklistedFiles().contains (outside));

        beginTest ("bundled copies win both lists and a session saved with the outside path");
        Fixture copies;
        expect (copyBundle (juce::File (LM_VST3_SHELL), copies.bundle));
        copies.stamp ("copies");
        expect (copies.sync().allRegistered);
        auto& copiesList = copies.host.getKnownPlugins();
        const auto insideTypes = copiesList.getTypes();
        for (auto description : insideTypes)
        {
            description.fileOrIdentifier = LM_VST3_SHELL;
            copiesList.addType (description);
        }
        expectEquals (copies.host.getEffectTypes().size(), 3);
        expectEquals (copies.host.getAllEffectTypes().size(), 3);
        copies.host.setBundledPluginsDirectory ({});
        expectEquals (copies.host.getEffectTypes().size(), 6);
        expectEquals (copies.host.getAllEffectTypes().size(), 6);
        copies.host.setBundledPluginsDirectory (copies.directory);
        for (auto description : insideTypes)
        {
            const auto bundledFile = description.fileOrIdentifier;
            description.fileOrIdentifier = LM_VST3_SHELL;
            PluginSlotState state;
            state.descriptionXml = description.createXml()->toString();
            juce::String error;
            auto instance = copies.host.createInstance (state, 48000.0, 256, error);
            expect (instance != nullptr, error);
            if (instance != nullptr)
            {
                expectEquals (instance->getPluginDescription().fileOrIdentifier, bundledFile);
                expectEquals (instance->getPluginDescription().name, description.name);
            }
        }

        beginTest ("list reset keeps bundled descriptions and clears the blacklist");
        copiesList.addToBlacklist (outside);
        resetPluginListKeepingBundled (copies.host);
        expectEquals (copiesList.getNumTypes(), 3);
        expect (copiesList.getBlacklistedFiles().isEmpty());
        for (const auto& description : copiesList.getTypes())
            expect (copies.host.isBundledPluginFile (description.fileOrIdentifier));

        beginTest ("headless registration persists its list in an isolated settings directory and returns status");
        Scratch command;
        const auto executable = command.directory.getChildFile ("LiveMix.exe");
        expect (juce::File (LM_SCAN_WORKER_EXE).copyFileTo (executable));
        for (const auto& dll : juce::File (LM_SCAN_WORKER_EXE).getParentDirectory().findChildFiles (juce::File::findFiles, false, "*.dll"))
            expect (dll.copyFileTo (command.directory.getChildFile (dll.getFileName())));
        const auto settingsDirectory = command.directory.getChildFile ("settings");
        SettingsEnvironment environment (settingsDirectory);
        const auto run = [&]
        {
            juce::ChildProcess child;
            expect (child.start (juce::StringArray { executable.getFullPathName(), "--register-bundled-plugins" }));
            expect (child.waitForProcessToFinish (30000));
            if (child.isRunning()) child.kill();
            return (int) child.getExitCode();
        };
        expectEquals (run(), 0); // no bundled directory
        const auto commandBundle = command.directory.getChildFile ("VST3/LiveMix Test Shell.vst3");
        expect (copyBundle (juce::File (LM_VST3_SHELL), commandBundle));
        commandBundle.getSiblingFile ("bundled_plugins.json").replaceWithText ("{\"id\":\"command\"}");
        expectEquals (run(), 0);
        expectEquals (run(), 0);
        {
            LiveMixSettings saved (settingsDirectory);
            PluginHost restored;
            restored.loadKnownPluginsFromXml (saved.getPluginList().get());
            expectEquals (restored.getKnownPlugins().getNumTypes(), 3);
            expectEquals (saved.getBundledPluginsStamp(), juce::String ("command"));
        }
        expect (copyBundle (juce::File (LM_VST3_EMPTY), command.directory.getChildFile ("VST3/LiveMix Test Empty.vst3")));
        expectEquals (run(), 1);
    }
};

static LiveMixBundledPluginsTests liveMixBundledPluginsTests;
} // namespace gocue::tests
