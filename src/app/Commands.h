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
    addFadeCue,           // Ctrl+7 (페이드 인)
    addFadeOutCue,        // Ctrl+Shift+7
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
    feedbackChat,         // Help > 커뮤니티: the open-chat room
    about,
    youtubeDownload,      // 유튜브다운 > 유튜브 다운로드...: the site makes an mp3 out of a YouTube link, the app fetches it

    uiScale100,           // 설정 > 글씨·화면 크기: this PC's UI scale, one command per choice (the saved one is ticked)
    uiScale110,
    uiScale125,
    uiScale150
};

/** The registration path used by MainComponent, kept independent of the shortcut catalog. */
inline const juce::Array<juce::CommandID>& getAllMainCommands()
{
    static const juce::Array<juce::CommandID> ids { CommandIDs::go, CommandIDs::pauseToggle, CommandIDs::fadeOutSelected,
                    CommandIDs::panicAll, CommandIDs::hardStopAll, CommandIDs::preview,
                    CommandIDs::auditionGo, CommandIDs::auditionPreview, CommandIDs::toggleAlwaysAudition,
                    CommandIDs::loadCue, CommandIDs::loadToTime, CommandIDs::resetCue, CommandIDs::resetAll,
                    CommandIDs::addCue, CommandIDs::addFadeCue, CommandIDs::addFadeOutCue, CommandIDs::addDevampCue, CommandIDs::addGroupCue, CommandIDs::groupSelectedCues,
                    CommandIDs::ungroupSelected, CommandIDs::collapseAllGroups, CommandIDs::expandAllGroups,
                    CommandIDs::addControlCue, CommandIDs::addWaitCue, CommandIDs::addMemoCue, CommandIDs::addMicCue, CommandIDs::toggleSequenceRecording,
                    CommandIDs::addCueList, CommandIDs::addCart, CommandIDs::nextContainer, CommandIDs::previousContainer,
                    CommandIDs::renameContainer, CommandIDs::removeContainer,
                    CommandIDs::revertFade, CommandIDs::fetchFadeLevels, CommandIDs::removeCue, CommandIDs::duplicateCue,
                    CommandIDs::moveCueUp, CommandIDs::moveCueDown, CommandIDs::selectAll,
                    CommandIDs::copyCues, CommandIDs::cutCues, CommandIDs::pasteCues, CommandIDs::pasteCueProperties,
                    CommandIDs::find, CommandIDs::findNext,
                    CommandIDs::renumber, CommandIDs::deleteNumbers, CommandIDs::findMissingFiles,
                    CommandIDs::saveCueTemplate, CommandIDs::clearCueTemplate,
                    CommandIDs::newProject, CommandIDs::openProject,
                    CommandIDs::saveProject, CommandIDs::saveProjectAs,
                    CommandIDs::undo, CommandIDs::redo, CommandIDs::toggleShowMode, CommandIDs::toggleActiveCues, CommandIDs::toggleInspector,
                    CommandIDs::audioSettings, CommandIDs::audioPatches, CommandIDs::pluginManager, CommandIDs::masterInserts,
                    CommandIDs::workspaceSettings,
                    CommandIDs::checkForUpdates, CommandIDs::showManual, CommandIDs::feedbackChat, CommandIDs::about,
                    CommandIDs::youtubeDownload,
                    CommandIDs::uiScale100, CommandIDs::uiScale110, CommandIDs::uiScale125, CommandIDs::uiScale150 };
    return ids;
}

} // namespace gocue::CommandIDs
