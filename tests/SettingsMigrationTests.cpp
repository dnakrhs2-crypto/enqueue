#include "app/SettingsMigration.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

/** 0.9.0 renamed GoCue to Enqueue: the first start copies the old settings folder over. */
class SettingsMigrationTests : public juce::UnitTest
{
public:
    SettingsMigrationTests() : juce::UnitTest ("Settings migration (GoCue -> Enqueue)", "Enqueue") {}

    void runTest() override
    {
        const auto stamp = juce::Uuid().toString().substring (0, 8);
        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        const auto oldName = "EnqueueTestOld-" + stamp;
        const auto newName = "EnqueueTestNew-" + stamp;
        const auto oldFolder = appData.getChildFile (oldName);
        const auto newFolder = appData.getChildFile (newName);

        juce::PropertiesFile::Options options;
        options.applicationName = "Enqueue";
        options.filenameSuffix = "settings";
        options.folderName = newName;
        options.osxLibrarySubFolder = "Application Support";
        options.commonToAllUsers = false;

        beginTest ("the old folder's files come over, renamed after the app");
        {
            expect (oldFolder.createDirectory().wasOk());
            expect (oldFolder.getChildFile (oldName + ".settings").replaceWithText ("<settings/>"));   // "GoCue.settings" in real life
            expect (oldFolder.getChildFile ("plugins.xml").replaceWithText ("<plugins/>"));
            expect (oldFolder.getChildFile ("GoCueNotes.txt").replaceWithText ("keep my name"));

            expect (migrateSettingsFolder (options, oldName).wasOk());

            expect (newFolder.getChildFile ("Enqueue.settings").existsAsFile());
            expectEquals (newFolder.getChildFile ("Enqueue.settings").loadFileAsString(), juce::String ("<settings/>"));
            expect (newFolder.getChildFile ("plugins.xml").existsAsFile());
            expect (newFolder.getChildFile ("GoCueNotes.txt").existsAsFile());   // only the settings file is renamed
            expect (oldFolder.getChildFile (oldName + ".settings").existsAsFile());   // the old folder is left alone
        }

        beginTest ("an existing new settings file is never overwritten");
        {
            expect (newFolder.getChildFile ("Enqueue.settings").replaceWithText ("<settings changed='1'/>"));
            expect (migrateSettingsFolder (options, oldName).wasOk());
            expectEquals (newFolder.getChildFile ("Enqueue.settings").loadFileAsString(), juce::String ("<settings changed='1'/>"));
        }

        beginTest ("no old folder: nothing happens");
        {
            newFolder.deleteRecursively();
            expect (migrateSettingsFolder (options, "EnqueueTestMissing-" + stamp).wasOk());
            expect (! newFolder.exists());
        }

        oldFolder.deleteRecursively();
        newFolder.deleteRecursively();
    }
};

static SettingsMigrationTests settingsMigrationTests;

} // namespace gocue::tests
