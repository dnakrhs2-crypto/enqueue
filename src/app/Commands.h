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
    loadCue,              // L
    loadToTime,           // Ctrl+T
    resetCue,
    resetAll,

    addCue,
    removeCue,
    duplicateCue,
    moveCueUp,
    moveCueDown,
    selectAll,            // Ctrl+A
    copyCues,             // Ctrl+C
    cutCues,              // Ctrl+X
    pasteCues,            // Ctrl+V
    pasteCueProperties,   // Ctrl+Shift+V
    find,                 // Ctrl+F
    findNext,             // F3
    renumber,             // Ctrl+R
    deleteNumbers,
    findMissingFiles,
    saveCueTemplate,
    clearCueTemplate,

    newProject,
    openProject,
    saveProject,
    saveProjectAs,

    undo,
    redo,

    toggleShowMode,       // Ctrl+Shift+M
    toggleActiveCues,     // Ctrl+L

    audioSettings,
    pluginManager,
    masterInserts,
    workspaceSettings,

    checkForUpdates,
    about
};

} // namespace gocue::CommandIDs
