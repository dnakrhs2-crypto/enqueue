#include "app/BackupManager.h"

#include <algorithm>
#include <map>
#include <vector>

namespace gocue::BackupManager
{

namespace
{
    const juce::String backupMarker (" (Backup ");
    const juce::String extension (".gocue");
    const juce::String stampFormat ("%Y-%m-%d_%H%M%S");
}

juce::File backupDirFor (const juce::File& project)
{
    return project.getSiblingFile (project.getFileName() + ".backups");
}

juce::File backupFileFor (const juce::File& project, juce::Time now)
{
    return backupDirFor (project).getChildFile (project.getFileNameWithoutExtension() + backupMarker
                                                 + now.formatted (stampFormat) + ")" + extension);
}

juce::Time timestampOf (const juce::File& backup)
{
    const auto name = backup.getFileNameWithoutExtension();
    const int start = name.lastIndexOf (backupMarker);

    if (start < 0 || ! name.endsWithChar (')'))
        return juce::Time();

    const auto stamp = name.substring (start + backupMarker.length(), name.length() - 1);   // yyyy-MM-dd_HHmmss

    if (stamp.length() != 17)
        return juce::Time();

    const int year   = stamp.substring (0, 4).getIntValue();
    const int month  = stamp.substring (5, 7).getIntValue();
    const int day    = stamp.substring (8, 10).getIntValue();
    const int hour   = stamp.substring (11, 13).getIntValue();
    const int minute = stamp.substring (13, 15).getIntValue();
    const int second = stamp.substring (15, 17).getIntValue();

    if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31)
        return juce::Time();

    return juce::Time (year, month - 1, day, hour, minute, second, 0, true);
}

juce::Result copyToBackups (const juce::File& project, juce::Time now, juce::File* backupOut)
{
    if (! project.existsAsFile())
        return juce::Result::fail ("Nothing to back up: " + project.getFullPathName());

    const auto dir = backupDirFor (project);

    if (! dir.exists())
    {
        const auto created = dir.createDirectory();

        if (created.failed())
            return created;
    }

    auto target = backupFileFor (project, now);

    for (int n = 2; target.existsAsFile() && n < 100; ++n)   // same second: never overwrite an earlier backup
        target = target.getSiblingFile (target.getFileNameWithoutExtension() + " " + juce::String (n) + extension);

    if (! project.copyFileTo (target))
        return juce::Result::fail ("Could not write " + target.getFullPathName());

    if (backupOut != nullptr)
        *backupOut = target;

    return juce::Result::ok();
}

void rotate (const juce::File& dir, juce::Time now)
{
    struct Entry { juce::File file; juce::Time time; };
    std::vector<Entry> backups;

    for (const auto& file : dir.findChildFiles (juce::File::findFiles, false, "*" + extension))
    {
        const auto time = timestampOf (file);

        if (time.toMilliseconds() > 0)
            backups.push_back ({ file, time });
    }

    std::sort (backups.begin(), backups.end(), [] (const Entry& a, const Entry& b) { return a.time > b.time; });   // newest first

    constexpr juce::int64 hour = 60 * 60 * 1000;
    constexpr juce::int64 day = 24 * hour;
    int recentKept = 0;
    std::map<juce::int64, bool> hourBuckets, dayBuckets;

    for (const auto& entry : backups)
    {
        const juce::int64 age = now.toMilliseconds() - entry.time.toMilliseconds();
        bool keep = false;

        if (age < hour)
        {
            keep = recentKept < keepRecent;
            recentKept += keep ? 1 : 0;
        }
        else if (age < day)
        {
            const auto bucket = age / hour;
            keep = ! hourBuckets[bucket];
            hourBuckets[bucket] = true;
        }
        else
        {
            const auto bucket = age / day;
            keep = ! dayBuckets[bucket];
            dayBuckets[bucket] = true;
        }

        if (! keep)
            entry.file.deleteFile();
    }
}

juce::File copyIntoProject (const juce::File& audio, const juce::File& projectDir)
{
    if (! audio.existsAsFile() || projectDir == juce::File())
        return audio;

    if (audio.isAChildOf (projectDir))
        return audio;

    const auto audioDir = projectDir.getChildFile ("audio");

    if (! audioDir.exists() && audioDir.createDirectory().failed())
        return audio;

    auto target = audioDir.getChildFile (audio.getFileName());

    for (int n = 2; target.existsAsFile() && n < 1000; ++n)
        target = audioDir.getChildFile (audio.getFileNameWithoutExtension() + " (" + juce::String (n) + ")" + audio.getFileExtension());

    return audio.copyFileTo (target) ? target : audio;
}

} // namespace gocue::BackupManager
