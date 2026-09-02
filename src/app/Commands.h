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
    auditionGo,           // Alt+Space
    auditionPreview,      // Alt+V
    toggleAlwaysAudition, // 재생 메뉴: 항상 오디션
    loadCue,              // L
    loadToTime,           // Ctrl+T
    resetCue,
    resetAll,

    addCue,
    addFadeCue,           // Ctrl+7
    addDevampCue,         // Ctrl+8
    addGroupCue,          // Ctrl+0
    groupSelectedCues,    // Ctrl+G
    ungroupSelected,      // Ctrl+Shift+G
    collapseAllGroups,
    expandAllGroups,
    addControlCue,        // Ctrl+9
    addWaitCue,
    addMemoCue,
    toggleSequenceRecording,   // Ctrl+Shift+E
    addMicCue,                 // Ctrl+M
    addCueList,                // 큐 리스트 추가
    addCart,                   // 카트 추가
    nextContainer,             // Ctrl+PageDown
    previousContainer,         // Ctrl+PageUp
    renameContainer,
    removeContainer,
    revertFade,           // Ctrl+Shift+R
    fetchFadeLevels,      // Ctrl+Shift+T
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
    toggleInspector,      // Ctrl+I

    audioSettings,
    audioPatches,         // 오디오 패치 편집기
    pluginManager,
    masterInserts,
    workspaceSettings,

    checkForUpdates,
    showManual,           // Ctrl+F1
    about
};

} // namespace gocue::CommandIDs
