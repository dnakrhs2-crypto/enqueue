#pragma once

#include "ui/MainComponent.h"

#include <array>

namespace gocue::tests
{
// Share the existing friend definition across test translation units. Keeping
// one definition avoids ODR violations and requires no product-code test hooks.
struct ReopenLastProjectTestAccess
{
    static bool saveAs (MainComponent& main, const juce::File& file) { return main.writeProjectToFile (file); }
    static void rememberSession (MainComponent& main, const juce::File& file) { main.rememberLastSessionProject (file); }
    static bool pendingAutoStart (const MainComponent& main) { return main.pendingStartOnOpenCue.isNotEmpty(); }
    static ShortcutRouter& keyboard (MainComponent& main) { return *main.shortcutRouter; }

    static CueController& controller (MainComponent& main) { return main.controller; }
    static ProjectDocument& document (MainComponent& main) { return main.document; }
    static ActiveCuesPanel& activePanel (MainComponent& main) { return main.activeCues; }
    static ActiveCuesWindow* bigView (MainComponent& main) { return main.activeCuesWindow.get(); }
    static void installBigView (MainComponent& main, std::unique_ptr<ActiveCuesWindow> window) { main.activeCuesWindow = std::move (window); }
    static void refreshPlayback (MainComponent& main) { main.timerCallback(); }
    static void useJucePanicFallback (MainComponent& main)
    {
        main.panicHook = std::make_unique<PanicKeyHook> (*main.shortcuts,
            [&main] (double time, bool hard) { main.panicFromAnywhere (time, hard); });
    }
    static PanicKeyHook& panicKeyHook (MainComponent& main) { return *main.panicHook; }
    static MidiInputService::Callbacks midiCallbacks (MainComponent& main) { return main.midiRouter->inputCallbacks(); }
    /** The 자동 레벨 dialog's counts: audio cues to match, matched, still waiting to be measured. */
    static std::array<int, 3> loudnessMatchCounts (const MainComponent& main)
    {
        return { main.matchAudioCues, main.matchMatched, main.matchWaiting };
    }
};
}
