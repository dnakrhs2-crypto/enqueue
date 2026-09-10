#pragma once
#include <juce_core/juce_core.h>

// CMake reads these same definitions for the executable's resources.
#define RECORDER_DISPLAY_NAME "Recorder (가칭)"
#define RECORDER_INTERNAL_ID "gocue.recorder"
#define RECORDER_SETTINGS_FOLDER "Recorder"
#define RECORDER_PROJECT_EXTENSION ".recorder"
#define RECORDER_FILE_TYPE "Recorder.Project"
#define RECORDER_APP_ID "{9BD0A267-2A38-47C1-9623-F1B76E2587D4}"
#define RECORDER_VERSION "0.1.3"
#define RECORDER_COMPANY "Gomtwigim"
// Confirmed by the CEO on 2026-09-10 (in-app updates): GitHub releases feed + gomtwigim.com page, like Enqueue/LiveMix.
#define RECORDER_PACKAGE_STEM "Recorder"
#define RECORDER_RELEASE_REPO "dnakrhs2-crypto/recorder"
#define RECORDER_RELEASE_REMOTE "recorder"
#define RECORDER_TAG_PREFIX "recorder-v"
#define RECORDER_SITE_DIR "recorder"
#define RECORDER_SITE_URL "https://xn--jb0byyo90f.com/recorder/"
#define RECORDER_APPCAST_URL "https://github.com/dnakrhs2-crypto/recorder/releases/latest/download/appcast.xml"
#define RECORDER_RELEASE_BASE_URL "https://github.com/dnakrhs2-crypto/recorder/releases/download/"
#define RECORDER_SOURCES_STEM "Recorder-sources"
#define RECORDER_PUBLICATION_CONFIRMED "1"

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
    static juce::String appcastUrl();
    static juce::String correspondingSourceUrl();
    static bool publicationConfirmed();
    static juce::File settingsDirectory(const juce::File& testRoot = {});
    struct MigrationAlias { juce::String displayName, settingsFolder, projectExtension, fileType; };
    static const std::vector<MigrationAlias>& migrationAliases();
};
}
