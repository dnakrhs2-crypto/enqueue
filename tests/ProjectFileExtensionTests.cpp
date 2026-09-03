#include "app/BackupManager.h"
#include "model/ProjectSerializer.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

/** 0.9.0: projects are .enqueue, the .gocue files of the GoCue days keep working. */
class ProjectFileExtensionTests : public juce::UnitTest
{
public:
    ProjectFileExtensionTests() : juce::UnitTest ("Project file extensions (.enqueue / .gocue)", "Enqueue") {}

    void runTest() override
    {
        beginTest ("new files are .enqueue; .enqueue and .gocue open, anything else does not");
        {
            expectEquals (juce::String (ProjectSerializer::fileExtension), juce::String (".enqueue"));
            expect (juce::File ("C:\\shows\\a.enqueue").hasFileExtension (ProjectSerializer::openableExtensions));
            expect (juce::File ("C:\\shows\\b.gocue").hasFileExtension (ProjectSerializer::openableExtensions));
            expect (juce::File ("C:\\shows\\B.GOCUE").hasFileExtension (ProjectSerializer::openableExtensions));
            expect (! juce::File ("C:\\shows\\c.txt").hasFileExtension (ProjectSerializer::openableExtensions));
            expect (! juce::File ("C:\\shows\\d.enqueue.backups").hasFileExtension (ProjectSerializer::openableExtensions));
        }

        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("enqueue_ext_" + juce::Uuid().toString());
        expect (root.createDirectory().wasOk());
        const juce::Time now (2026, 8, 3, 14, 30, 0);   // 3 September 2026 14:30:00

        beginTest ("backups follow the project's own extension");
        {
            const auto fresh = BackupManager::backupFileFor (root.getChildFile ("show.enqueue"), now);
            expectEquals (fresh.getFileName(), juce::String ("show (Backup 2026-09-03_143000).enqueue"));
            expectEquals (fresh.getParentDirectory().getFileName(), juce::String ("show.enqueue.backups"));

            const auto old = BackupManager::backupFileFor (root.getChildFile ("old.gocue"), now);
            expectEquals (old.getFileName(), juce::String ("old (Backup 2026-09-03_143000).gocue"));
            expectEquals (old.getParentDirectory().getFileName(), juce::String ("old.gocue.backups"));
        }

        beginTest ("rotate counts backups of both eras together");
        {
            const auto dir = root.getChildFile ("mix.enqueue.backups");
            expect (dir.createDirectory().wasOk());
            auto make = [&] (juce::Time t, const juce::String& ext)
            {
                expect (dir.getChildFile ("mix (Backup " + t.formatted ("%Y-%m-%d_%H%M%S") + ")" + ext).replaceWithText ("x"));
            };

            for (int i = 0; i < 10; ++i)
                make (now - juce::RelativeTime::minutes (i * 2), ".enqueue");        // the 10 newest

            for (int i = 0; i < 10; ++i)
                make (now - juce::RelativeTime::hours (2) - juce::RelativeTime::minutes (i * 2), ".gocue");   // 10 older ones

            BackupManager::rotate (dir, now);

            const auto enqueue = dir.findChildFiles (juce::File::findFiles, false, "*.enqueue");
            const auto gocue = dir.findChildFiles (juce::File::findFiles, false, "*.gocue");
            expectEquals (enqueue.size(), 10);   // all of the newest survive...
            expectEquals (gocue.size(), 5);      // ...and the older era is trimmed to fill 15
        }

        root.deleteRecursively();
    }
};

static ProjectFileExtensionTests projectFileExtensionTests;

} // namespace gocue::tests
