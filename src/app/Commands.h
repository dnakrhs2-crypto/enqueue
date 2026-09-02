#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::CommandIDs
{

enum : juce::CommandID
{
    go = 0x2001,          // Space: GO (or resume paused cues)
    pauseToggle,          // P
    fadeOutSelected,      // F
    panicAll,             // Esc: fade everything out over the panic time (twice = hard stop)
    hardStopAll,
    preview,              // V
    resetCue,
    resetAll,

    addCue,
    removeCue,
    duplicateCue,
    moveCueUp,
    moveCueDown,

    newProject,
    openProject,
    saveProject,
    saveProjectAs,

    undo,
    redo,

    audioSettings,
    pluginManager,
    masterInserts,
    workspaceSettings,

    checkForUpdates,
    about
};

} // namespace gocue::CommandIDs
