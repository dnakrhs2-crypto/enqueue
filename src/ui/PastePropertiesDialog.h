#pragma once

#include "model/CuePropertyPaste.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::PastePropertiesDialog
{

using Selection = CuePropertyPaste::Selection;

/** Copies the selected groups from 'source' onto 'target' (see CuePropertyPaste::apply). */
inline void applyProperties (const Cue& source, Cue& target, const Selection& selection)
{
    CuePropertyPaste::apply (source, target, selection);
}

/** Shows the group checkboxes; 'onApply' receives the choice. */
void show (juce::Component* centreAround, const juce::String& sourceName, int targetCount, std::function<void (const Selection&)> onApply);

} // namespace gocue::PastePropertiesDialog
