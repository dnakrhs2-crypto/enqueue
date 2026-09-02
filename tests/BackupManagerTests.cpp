#include "app/BackupManager.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class BackupManagerTests : public juce::UnitTest
{
public:
    BackupManagerTests() : juce::UnitTest ("BackupManager", "GoCue") {}

    void runTest() override
    {
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gocue_backup_" + juce::Uuid().toString());
        expect (root.createDirectory().wasOk());
        const auto project = root.getChildFile ("show.gocue");
        expect (project.replaceWithText ("{\"app\":\"GoCue\",\"version\":2,\"cues\":[]}"));
        const juce::Time now (2026, 8, 2, 14, 30, 0, 0, true);   // 2026-09-02 14:30:00 (month is 0-based)

        beginTest ("backup files live next to the project and carry a timestamp");
        {
            const auto dir = BackupManager::backupDirFor (project);
            expectEquals (dir.getFileName(), juce::String ("show.gocue.backups"));
            expect (dir.getParentDirectory() == root);

            const auto file = BackupManager::backupFileFor (project, now);
            expectEquals (file.getFileName(), juce::String ("show (Backup 2026-09-02_143000).gocue"));
            expect (file.getParentDirectory() == dir);

            const auto parsed = BackupManager::timestampOf (file);
            expectEquals (parsed.toMilliseconds(), now.toMilliseconds());
            expectEquals (BackupManager::timestampOf (root.getChildFile ("notes.gocue")).toMilliseconds(), (juce::int64) 0);
            expectEquals (BackupManager::timestampOf (root.getChildFile ("x (Backup nope).gocue")).toMilliseconds(), (juce::int64) 0);
        }

        beginTest ("copyToBackups copies the saved file and never overwrites an earlier backup");
        {
            juce::File made;
            expect (BackupManager::copyToBackups (project, now, &made).wasOk());
            expect (made.existsAsFile());
            expectEquals (made.loadFileAsString(), project.loadFileAsString());

            juce::File second;
            expect (BackupManager::copyToBackups (project, now, &second).wasOk());
            expect (second != made);
            expect (second.getFileName().contains (" 2.gocue"));

            expect (BackupManager::copyToBackups (root.getChildFile ("missing.gocue"), now).failed());
        }

        beginTest ("rotate keeps 20 recent, one per hour for a day, one per day beyond");
        {
            const auto dir = root.getChildFile ("rot.gocue.backups");
            expect (dir.createDirectory().wasOk());
            auto make = [&] (juce::Time t) { expect (dir.getChildFile ("rot (Backup " + t.formatted ("%Y-%m-%d_%H%M%S") + ").gocue").replaceWithText ("x")); };

            for (int i = 0; i < 25; ++i)                                   // last hour: 25 backups, 2 minutes apart
                make (now - juce::RelativeTime::minutes (i * 2));

            for (int i = 0; i < 5; ++i)                                    // 5 backups inside hour 5
                make (now - juce::RelativeTime::hours (5) - juce::RelativeTime::minutes (i * 10));

            for (int i = 0; i < 3; ++i)                                    // 3 backups on day 3
                make (now - juce::RelativeTime::days (3) - juce::RelativeTime::hours (i * 2));

            make (now - juce::RelativeTime::days (10));                    // a lone old one
            expect (dir.getChildFile ("keep-me.txt").replaceWithText ("not a backup"));

            BackupManager::rotate (dir, now);

            const auto remaining = dir.findChildFiles (juce::File::findFiles, false, "*.gocue");
            expectEquals (remaining.size(), 20 + 1 + 1 + 1);
            expect (dir.getChildFile ("keep-me.txt").existsAsFile());

            int recent = 0;
            for (const auto& f : remaining)
                if (now.toMilliseconds() - BackupManager::timestampOf (f).toMilliseconds() < 60 * 60 * 1000)
                    ++recent;

            expectEquals (recent, 20);
            expect (dir.getChildFile ("rot (Backup " + now.formatted ("%Y-%m-%d_%H%M%S") + ").gocue").existsAsFile());   // newest survives
        }

        beginTest ("copyIntoProject copies into audio/, renames on collision and leaves project files alone");
        {
            // a folder that is *not* inside the project folder (a source inside it must not be copied)
            const auto outside = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gocue_backup_src_" + juce::Uuid().toString());
            expect (outside.createDirectory().wasOk());
            const auto source = outside.getChildFile ("tone.wav");
            expect (source.replaceWithText ("audio bytes"));

            const auto copied = BackupManager::copyIntoProject (source, root);
            expect (copied == root.getChildFile ("audio").getChildFile ("tone.wav"));
            expect (copied.existsAsFile());
            expectEquals (copied.loadFileAsString(), juce::String ("audio bytes"));

            const auto again = BackupManager::copyIntoProject (source, root);
            expectEquals (again.getFileName(), juce::String ("tone (2).wav"));

            expect (BackupManager::copyIntoProject (copied, root) == copied);              // already inside
            expect (BackupManager::copyIntoProject (outside.getChildFile ("nope.wav"), root) == outside.getChildFile ("nope.wav"));
            expect (outside.deleteRecursively());
        }

        expect (root.deleteRecursively());
    }
};

static BackupManagerTests backupManagerTests;

} // namespace gocue::tests
