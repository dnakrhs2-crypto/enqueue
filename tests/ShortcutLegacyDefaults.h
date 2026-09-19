// Defaults from MainComponent::getCommandInfo in 0.10.6.
// Independent regression fixture: Windows Esc was supplied by the hook.
#pragma once

#include "app/Commands.h"

namespace gocue::shortcut_test
{
inline juce::ApplicationCommandInfo legacyCommandInfo (juce::CommandID id)
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    juce::ApplicationCommandInfo result (id);
    switch (id)
    {
        case CommandIDs::go:
            result.addDefaultKeypress (KeyPress::spaceKey, ModifierKeys::noModifiers);
            result.flags |= juce::ApplicationCommandInfo::wantsKeyUpDownCallbacks;
            break;
        case CommandIDs::pauseToggle:
            result.addDefaultKeypress ('P', ModifierKeys::noModifiers);
            break;
        case CommandIDs::fadeOutSelected:
            result.addDefaultKeypress ('F', ModifierKeys::noModifiers);
            break;
        case CommandIDs::panicAll:
           #if ! JUCE_WINDOWS
            result.addDefaultKeypress (KeyPress::escapeKey, ModifierKeys::noModifiers);
            result.flags |= juce::ApplicationCommandInfo::wantsKeyUpDownCallbacks;
           #endif
            break;
        case CommandIDs::preview:
            result.addDefaultKeypress ('V', ModifierKeys::noModifiers);
            break;
        case CommandIDs::auditionGo:
            result.addDefaultKeypress (KeyPress::spaceKey, ModifierKeys::altModifier);
            break;
        case CommandIDs::auditionPreview:
            result.addDefaultKeypress ('V', ModifierKeys::altModifier);
            break;
        case CommandIDs::loadCue:
            result.addDefaultKeypress ('L', ModifierKeys::noModifiers);
            break;
        case CommandIDs::loadToTime:
            result.addDefaultKeypress ('T', ModifierKeys::commandModifier);
            break;
        case CommandIDs::addCue:
            result.addDefaultKeypress (KeyPress::insertKey, ModifierKeys::noModifiers);
            break;
        case CommandIDs::addFadeCue:
            result.addDefaultKeypress ('7', ModifierKeys::commandModifier);
            break;
        case CommandIDs::addFadeOutCue:
            result.addDefaultKeypress ('7', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::addDevampCue:
            result.addDefaultKeypress ('8', ModifierKeys::commandModifier);
            break;
        case CommandIDs::addGroupCue:
            result.addDefaultKeypress ('0', ModifierKeys::commandModifier);
            break;
        case CommandIDs::groupSelectedCues:
            result.addDefaultKeypress ('G', ModifierKeys::commandModifier);
            break;
        case CommandIDs::ungroupSelected:
            result.addDefaultKeypress ('G', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::addControlCue:
            result.addDefaultKeypress ('9', ModifierKeys::commandModifier);
            break;
        case CommandIDs::addMicCue:
            result.addDefaultKeypress ('6', ModifierKeys::commandModifier);
            break;
        case CommandIDs::nextContainer:
            result.addDefaultKeypress (juce::KeyPress::pageDownKey, ModifierKeys::commandModifier);
            break;
        case CommandIDs::previousContainer:
            result.addDefaultKeypress (juce::KeyPress::pageUpKey, ModifierKeys::commandModifier);
            break;
        case CommandIDs::toggleSequenceRecording:
            result.addDefaultKeypress ('E', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::revertFade:
            result.addDefaultKeypress ('R', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::fetchFadeLevels:
            result.addDefaultKeypress ('T', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::removeCue:
            result.addDefaultKeypress (KeyPress::deleteKey, ModifierKeys::noModifiers);
            break;
        case CommandIDs::duplicateCue:
            result.addDefaultKeypress ('D', ModifierKeys::commandModifier);
            break;
        case CommandIDs::moveCueUp:
            result.addDefaultKeypress (KeyPress::upKey, ModifierKeys::commandModifier);
            break;
        case CommandIDs::moveCueDown:
            result.addDefaultKeypress (KeyPress::downKey, ModifierKeys::commandModifier);
            break;
        case CommandIDs::selectAll:
            result.addDefaultKeypress ('A', ModifierKeys::commandModifier);
            break;
        case CommandIDs::copyCues:
            result.addDefaultKeypress ('C', ModifierKeys::commandModifier);
            break;
        case CommandIDs::cutCues:
            result.addDefaultKeypress ('X', ModifierKeys::commandModifier);
            break;
        case CommandIDs::pasteCues:
            result.addDefaultKeypress ('V', ModifierKeys::commandModifier);
            break;
        case CommandIDs::pasteCueProperties:
            result.addDefaultKeypress ('V', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::find:
            result.addDefaultKeypress ('F', ModifierKeys::commandModifier);
            break;
        case CommandIDs::findNext:
            result.addDefaultKeypress (KeyPress::F3Key, ModifierKeys::noModifiers);
            break;
        case CommandIDs::renumber:
            result.addDefaultKeypress ('R', ModifierKeys::commandModifier);
            break;
        case CommandIDs::newProject:
            result.addDefaultKeypress ('N', ModifierKeys::commandModifier);
            break;
        case CommandIDs::openProject:
            result.addDefaultKeypress ('O', ModifierKeys::commandModifier);
            break;
        case CommandIDs::saveProject:
            result.addDefaultKeypress ('S', ModifierKeys::commandModifier);
            break;
        case CommandIDs::saveProjectAs:
            result.addDefaultKeypress ('S', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::workspaceSettings:
            result.addDefaultKeypress (',', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::undo:
            result.addDefaultKeypress ('Z', ModifierKeys::commandModifier);
            break;
        case CommandIDs::redo:
            result.addDefaultKeypress ('Y', ModifierKeys::commandModifier);
            result.addDefaultKeypress ('Z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::toggleShowMode:
            result.addDefaultKeypress ('M', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::toggleActiveCues:
            result.addDefaultKeypress ('L', ModifierKeys::commandModifier);
            break;
        case CommandIDs::toggleInspector:
            result.addDefaultKeypress ('I', ModifierKeys::commandModifier);
            break;
        case CommandIDs::audioSettings:
            result.addDefaultKeypress (',', ModifierKeys::commandModifier);
            break;
        case CommandIDs::audioPatches:
            result.addDefaultKeypress ('P', ModifierKeys::commandModifier | ModifierKeys::shiftModifier);
            break;
        case CommandIDs::pluginManager:
            result.addDefaultKeypress ('P', ModifierKeys::commandModifier);
            break;
        case CommandIDs::masterInserts:
            result.addDefaultKeypress ('M', ModifierKeys::commandModifier);
            break;
        case CommandIDs::showManual:
            result.addDefaultKeypress (juce::KeyPress::F1Key, ModifierKeys::commandModifier);
            break;
        case juce::StandardApplicationCommandIDs::quit:
            result.addDefaultKeypress ('Q', ModifierKeys::commandModifier);
            break;
        default: break;
    }
    return result;
}
} // namespace gocue::shortcut_test
