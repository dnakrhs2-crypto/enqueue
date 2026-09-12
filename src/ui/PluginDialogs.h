#pragma once

#include "app/AppSettings.h"
#include "audio/AudioEngine.h"
#include "ui/PluginWindows.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::PluginDialogs
{

/** Routes a chain edit through the document so it becomes an undo step. */
using PerformEdit = std::function<void (const juce::String& name, const std::function<void()>& edit)>;

/** The master bus insert chain. Non-modal, single instance. (The plugin manager is PluginManagerWindow since 0.9.8.) */
void showMasterInserts (AudioEngine& engine, PluginWindowManager& windows,
                        std::function<void()> onOpenPluginManager, PerformEdit performEdit,
                        juce::Component* centreAround);

/** Forward chain changes so an open master-inserts dialog stays in sync. */
void chainChanged (PluginChain* chain);

/** Closes the dialog if it is open (call before the engine goes away). */
void closeAll();

} // namespace gocue::PluginDialogs
