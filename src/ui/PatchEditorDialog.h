#pragma once

#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include "ui/PluginWindows.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::PatchEditorDialog
{

/** Audio patch editor (QLab "Audio Patch Editor"): patch list, cue outputs (names, stereo pairs, inserts),
    routing matrix to the device outputs with the patch main level, device output inserts.
    Non-modal, single instance; every change is written to the document and the engine at once. */
void show (ProjectDocument& document, AudioEngine& engine, PluginWindowManager& windows,
           std::function<void()> onOpenPluginManager, juce::Component* centreAround);
void closeIfOpen();
/** Forward chain changes so open insert strips stay in sync. */
void chainChanged (PluginChain* chain);
/** Call after the document's patches were replaced from outside (project open / new). */
void patchesChanged();

} // namespace gocue::PatchEditorDialog
