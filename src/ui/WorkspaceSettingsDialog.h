#pragma once

#include "app/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::WorkspaceSettingsDialog
{

/** Project settings (QLab "Workspace Settings"): 일반 / 파일 tabs. Non-modal, single instance;
    every change is written to the document at once. */
void show (ProjectDocument& document, juce::Component* centreAround);
void closeIfOpen();

} // namespace gocue::WorkspaceSettingsDialog
