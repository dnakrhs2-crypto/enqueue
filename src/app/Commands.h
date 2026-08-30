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

    audioSettings,
    pluginManager,
    masterInserts
};

} // namespace gocue::CommandIDs
