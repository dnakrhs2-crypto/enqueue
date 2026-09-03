#include "app/SettingsMigration.h"

namespace gocue
{

void migrateSettingsFolder (const juce::PropertiesFile::Options& options, const juce::String& oldFolderName)
{
    const auto newFile = options.getDefaultFile();
    const auto newFolder = newFile.getParentDirectory();
    const auto oldFolder = newFolder.getSiblingFile (oldFolderName);

    if (newFile.existsAsFile() || ! oldFolder.isDirectory())
        return;

    newFolder.createDirectory();

    for (const auto& f : oldFolder.findChildFiles (juce::File::findFiles, false))
        f.copyFileTo (newFolder.getChildFile (f.getFileName().replace ("GoCue", options.applicationName)));
}

} // namespace gocue
