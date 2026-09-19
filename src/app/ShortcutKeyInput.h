#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::ShortcutKeyInput
{
/** Pure ownership probe for an editable JUCE TextEditor, including ordinary Tab focus
    traversal. Pass the original event with its text character for runtime input;
    character-less printable/NumPad bindings are also recognised for conflict previews.
    Recognised operations keep ownership even if there is no selection/undo action. */
bool isStandardTextEditorKey (const juce::KeyPress& key);

/** The exact character predicate used to start direct value entry in a level matrix. */
bool isLevelMatrixValueKey (const juce::KeyPress& key);
}
