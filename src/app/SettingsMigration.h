#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace gocue
{

/** 0.9.0 renamed GoCue to Enqueue. When the settings file described by 'options' does not exist yet and the sibling
    folder 'oldFolderName' does (%APPDATA%\GoCue), its files are copied over, "GoCue" in their names becoming
    options.applicationName. The old folder is left untouched. */
void migrateSettingsFolder (const juce::PropertiesFile::Options& options, const juce::String& oldFolderName);

} // namespace gocue
