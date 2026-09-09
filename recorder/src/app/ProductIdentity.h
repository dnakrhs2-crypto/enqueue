#pragma once
#include <juce_core/juce_core.h>

// CMake reads these same definitions for the executable's resources.
#define RECORDER_DISPLAY_NAME "Recorder"
#define RECORDER_INTERNAL_ID "gocue.recorder"
#define RECORDER_SETTINGS_FOLDER "Recorder"
#define RECORDER_PROJECT_EXTENSION ".recorder"
#define RECORDER_FILE_TYPE "Recorder.Project"
#define RECORDER_APP_ID "{9BD0A267-2A38-47C1-9623-F1B76E2587D4}"
#define RECORDER_VERSION "0.1.0"
#define RECORDER_COMPANY "Gomtwigim"

namespace gocue::recorder
{
struct ProductIdentity
{
    static juce::String displayName();
    static juce::String internalId();
    static juce::String version();
    static juce::String appId(); // stable after publication, independent of display name
    static juce::String fileType();
    static juce::String projectExtension();
    static juce::String projectFileName();
    static juce::String settingsFileName();
    static juce::String updateRegistryKey();
    static juce::File settingsDirectory(const juce::File& testRoot = {});
    struct MigrationAlias { juce::String displayName, settingsFolder, projectExtension, fileType; };
    static const std::vector<MigrationAlias>& migrationAliases();
};
}
