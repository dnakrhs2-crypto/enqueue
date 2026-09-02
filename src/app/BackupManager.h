#pragma once

#include <juce_core/juce_core.h>

namespace gocue::BackupManager
{

/** "<project>.gocue.backups" next to the project file. */
juce::File backupDirFor (const juce::File& project);

/** The backup file for 'now': "<dir>/<name> (Backup yyyy-MM-dd_HHmmss).gocue". */
juce::File backupFileFor (const juce::File& project, juce::Time now);

/** Timestamp parsed from a backup file name, or an invalid (0) Time when the name is not a backup. */
juce::Time timestampOf (const juce::File& backup);

/** Copies the saved project file into the backup folder (used right before it is overwritten by a save).
    Returns the failure or ok; 'backupOut' receives the new file. */
juce::Result copyToBackups (const juce::File& project, juce::Time now, juce::File* backupOut = nullptr);

/** Thins out old backups: the 20 newest from the last hour, one per hour for the last day, one per day
    beyond that. Files that are not backups are left alone. */
void rotate (const juce::File& dir, juce::Time now);

/** Copies an audio file into "<projectDir>/audio" (renaming on collision: "name (2).wav"). Returns the
    destination, the source itself when it already lives inside the project folder, or the source when
    the copy failed. */
juce::File copyIntoProject (const juce::File& audio, const juce::File& projectDir);

constexpr int keepRecent = 20;

} // namespace gocue::BackupManager
