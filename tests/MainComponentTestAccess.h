#pragma once

#include "ui/MainComponent.h"

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
    static PanicKeyHook& panicKeyHook (MainComponent& main) { return *main.panicHook; }
    static MidiInputService::Callbacks midiCallbacks (MainComponent& main) { return main.midiRouter->inputCallbacks(); }
};
}
