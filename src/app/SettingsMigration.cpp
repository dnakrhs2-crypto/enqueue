#include "app/SettingsMigration.h"

namespace gocue
{

juce::Result migrateSettingsFolder (const juce::PropertiesFile::Options& options, const juce::String& oldFolderName)
{
    const auto newFile = options.getDefaultFile();
    const auto newFolder = newFile.getParentDirectory();
    const auto oldFolder = newFolder.getSiblingFile (oldFolderName);

    if (newFile.existsAsFile() || ! oldFolder.isDirectory())
        return juce::Result::ok();

    const auto created = newFolder.createDirectory();

    if (created.failed())
        return created;

    const auto oldSettingsName = oldFolderName + "." + options.filenameSuffix;   // "GoCue.settings": the one file named after the app
    juce::StringArray failed;

    for (const auto& f : oldFolder.findChildFiles (juce::File::findFiles, false))
    {
        const auto target = f.getFileName().equalsIgnoreCase (oldSettingsName) ? newFile : newFolder.getChildFile (f.getFileName());

        if (! f.copyFileTo (target))
            failed.add (f.getFileName());
    }

    if (! failed.isEmpty())
    {
        newFile.deleteFile();
        return juce::Result::fail ("could not copy " + failed.joinIntoString (", ") + " from " + oldFolder.getFullPathName());
    }

    return juce::Result::ok();
}

} // namespace gocue
