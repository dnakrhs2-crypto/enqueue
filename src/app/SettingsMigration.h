#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace gocue
{

/** 0.9.0 renamed GoCue to Enqueue. When the settings file described by 'options' does not exist yet and the sibling
    folder 'oldFolderName' does (%APPDATA%\GoCue), its files are copied over: "<oldFolderName>.<suffix>" becomes the
    new settings file, every other file keeps its name. The old folder is left untouched. When a copy fails the new
    settings file is removed again, so the next start tries once more instead of settling for empty settings. */
juce::Result migrateSettingsFolder (const juce::PropertiesFile::Options& options, const juce::String& oldFolderName);

} // namespace gocue
