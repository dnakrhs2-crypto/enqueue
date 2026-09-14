#pragma once

#include "app/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::WorkspaceSettingsDialog
{

/** Project settings (QLab "Workspace Settings", 설정 > 프로젝트 설정): 일반 / 파일 / 오디오 tabs. Non-modal, single
    instance; every change is written to the document at once. This PC's 글씨·화면 크기 is not here: it lives in the
    설정 menu (AppSettings), since it is not the project's. */
void show (ProjectDocument& document, juce::Component* centreAround);
void closeIfOpen();

} // namespace gocue::WorkspaceSettingsDialog
