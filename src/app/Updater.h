#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace gocue
{

/** Auto-update through WinSparkle, fed by an appcast published on GitHub Releases.

    Everything is a no-op when the build has no WinSparkle (GOCUE_HAS_WINSPARKLE=0) or when
    the appcast URL / EdDSA public key were not configured at build time. */
class Updater
{
public:
    struct Callbacks
    {
        /** May the app close right now (no unsaved work)? Called from a WinSparkle thread. */
        std::function<bool()> canShutdown;
        /** Close the app so the installer can run. Called from a WinSparkle thread. */
        std::function<void()> requestShutdown;
    };

    static bool isAvailable();
    static juce::String getAppcastUrl();

    /** Call once the main window is visible. Starts the automatic (daily) background check. */
    static void initialise (const juce::String& companyName, const juce::String& appName,
                            const juce::String& version, Callbacks callbacks);

    /** Manual check with WinSparkle's own UI (menu item). */
    static void checkForUpdatesWithUI();

    /** Call before the main window goes away. */
    static void shutdown();
};

} // namespace gocue
