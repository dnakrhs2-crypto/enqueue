#include "app/ShortcutKeyInput.h"

namespace gocue::ShortcutKeyInput
{
namespace
{
// Reuse JUCE's modifier/platform rules without changing text, selection or clipboard.
struct TextEditorKeyProbe
{
    bool scrollUp() { return true; }
    bool scrollDown() { return true; }
    bool moveCaretLeft (bool, bool) { return true; }
    bool moveCaretRight (bool, bool) { return true; }
    bool moveCaretToTop (bool) { return true; }
    bool moveCaretToEnd (bool) { return true; }
    bool moveCaretToStartOfLine (bool) { return true; }
    bool moveCaretToEndOfLine (bool) { return true; }
    bool moveCaretUp (bool) { return true; }
    bool moveCaretDown (bool) { return true; }
    bool pageUp (bool) { return true; }
    bool pageDown (bool) { return true; }
    bool copyToClipboard() { return true; }
    bool cutToClipboard() { return true; }
    bool pasteFromClipboard() { return true; }
    bool deleteBackwards (bool) { return true; }
    bool deleteForwards (bool) { return true; }
    bool selectAll() { return true; }
    bool undo() { return true; }
    bool redo() { return true; }
};

bool isCharacterlessTextBinding (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (key.getTextCharacter() != 0 || mods.isCtrlDown() || mods.isAltDown() || mods.isCommandDown())
        return false;

    const int code = key.getKeyCode();
    if ((code >= ' ' && code <= '~') || (code >= juce::KeyPress::numberPad0 && code <= juce::KeyPress::numberPad9))
        return true;
    for (const int padCode : { juce::KeyPress::numberPadAdd, juce::KeyPress::numberPadSubtract,
                               juce::KeyPress::numberPadMultiply, juce::KeyPress::numberPadDivide,
                               juce::KeyPress::numberPadSeparator, juce::KeyPress::numberPadDecimalPoint,
                               juce::KeyPress::numberPadEquals })
        if (code == padCode)
            return true;
    return false;
}
}

bool isStandardTextEditorKey (const juce::KeyPress& key)
{
    TextEditorKeyProbe probe;
    return juce::TextEditorKeyMapper<TextEditorKeyProbe>::invokeKeyFunction (probe, key)
        || key == juce::KeyPress::returnKey
        || key.isKeyCode (juce::KeyPress::escapeKey)
        || key.getTextCharacter() >= ' '
        || key.getTextCharacter() == '\t'
        || isCharacterlessTextBinding (key)
        || key == juce::KeyPress (juce::KeyPress::tabKey)
        || key == juce::KeyPress (juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0);
}

bool isLevelMatrixValueKey (const juce::KeyPress& key)
{
    const auto ch = key.getTextCharacter();
    return (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '+';
}
}
