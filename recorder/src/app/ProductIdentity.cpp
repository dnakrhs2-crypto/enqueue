#include "ProductIdentity.h"

namespace gocue::recorder
{
juce::String ProductIdentity::displayName() { return RECORDER_DISPLAY_NAME; }
juce::String ProductIdentity::internalId() { return RECORDER_INTERNAL_ID; }
juce::String ProductIdentity::version() { return RECORDER_VERSION; }
juce::String ProductIdentity::appId() { return RECORDER_APP_ID; }
juce::String ProductIdentity::fileType() { return RECORDER_FILE_TYPE; }
juce::String ProductIdentity::projectExtension() { return RECORDER_PROJECT_EXTENSION; }
juce::String ProductIdentity::projectFileName() { return "project" + projectExtension(); }
juce::String ProductIdentity::settingsFileName() { return juce::String(RECORDER_SETTINGS_FOLDER) + ".settings"; }
juce::String ProductIdentity::updateRegistryKey() { return "Software\\" RECORDER_COMPANY "\\" RECORDER_SETTINGS_FOLDER "\\WinSparkle"; }
juce::File ProductIdentity::settingsDirectory(const juce::File& testRoot)
{
    return testRoot != juce::File() ? testRoot
        : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(RECORDER_SETTINGS_FOLDER);
}
const std::vector<ProductIdentity::MigrationAlias>& ProductIdentity::migrationAliases()
{
    // Add the previous public identifiers here before a rename; never regenerate AppId/project UUIDs.
    static const std::vector<MigrationAlias> aliases;
    return aliases;
}
}
