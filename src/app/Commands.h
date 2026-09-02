#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::CommandIDs
{

enum : juce::CommandID
{
    go = 0x2001,
    stopSelected,
    stopAll,
    fadeOutSelected,
    fadeOutAll,

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

    checkForUpdates,
    about
};

} // namespace gocue::CommandIDs
