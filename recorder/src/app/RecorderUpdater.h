#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace gocue::recorder
{
// Host lifecycle/menu wiring belongs to round 28; Main.cpp/UI are outside rounds 32+33.
class RecorderUpdater
{
public:
    struct Callbacks
    {
        // WinSparkle worker thread: round 28 supplies a thread-safe state snapshot.
        // Empty = allow. Do not read UI/document objects directly from this callback.
        std::function<bool()> canShutdown;
        // Called on the JUCE message thread, after canShutdown is rechecked.
        std::function<void()> requestShutdown;
        std::function<void()> shutdownBlocked; // message thread, explanatory banner
    };
    static bool isAvailable();
    static void initialise(Callbacks callbacks = {});
    static void shutdown(); // before destroying objects referenced by callbacks
    static void checkForUpdatesWithUI();
    static void checkQuietly(); // host calls only at an idle moment
    static juce::String aboutText();
    static void showAboutDialog();
};
}
