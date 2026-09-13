#pragma once

#include "app/AppSettings.h"
#include "app/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::WorkspaceSettingsDialog
{

/** Project settings (QLab "Workspace Settings"): 일반 / 파일 / 오디오 tabs. Non-modal, single instance; every change
    is written to the document at once. 글씨·화면 크기 on the 일반 tab is this PC's (AppSettings), not the project's:
    'applyUiScale' puts a choice into effect and returns the percent actually applied. */
void show (ProjectDocument& document, AppSettings& appSettings, std::function<int (int percent)> applyUiScale,
           juce::Component* centreAround);
void closeIfOpen();

} // namespace gocue::WorkspaceSettingsDialog
