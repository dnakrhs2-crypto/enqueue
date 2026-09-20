#pragma once

#include "app/ProjectDocument.h"
#include "app/ShortcutService.h"
#include "app/MidiInputService.h"
#include "app/MidiTriggerRouter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::WorkspaceSettingsDialog
{

/** Single modal launchAsync dialog: 일반 / 파일 / 오디오 edit the project immediately;
    단축키 edits this PC through ShortcutService. UI scale remains in the settings menu. */
void show (ProjectDocument& document, ShortcutService&, juce::Component* centreAround, MidiInputService* = nullptr, MidiTriggerRouter* = nullptr);
void closeIfOpen();

} // namespace gocue::WorkspaceSettingsDialog
