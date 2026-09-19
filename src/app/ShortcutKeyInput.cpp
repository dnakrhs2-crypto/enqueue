#include "app/ShortcutKeyInput.h"

namespace gocue::ShortcutKeyInput
{
const std::array<NumberPadAlias, 7>& numberPadAliases()
{
    using K = juce::KeyPress;
    static const std::array<NumberPadAlias, 7> aliases {{
        { K::numberPadMultiply, 0x6a, "*" }, { K::numberPadAdd, 0x6b, "+" },
        { K::numberPadSeparator, 0x6c, "," }, { K::numberPadSubtract, 0x6d, "-" },
        { K::numberPadDecimalPoint, 0x6e, ".," }, { K::numberPadDivide, 0x6f, "/" },
        { K::numberPadEquals, 0x92, "=" }
    }};
    return aliases;
}

bool keysOverlap (const juce::KeyPress& a, const juce::KeyPress& b)
{
    if (a == b)
        return true;
    if (a.getModifiers() != b.getModifiers())
        return false;
    const auto characters = [] (const juce::KeyPress& key)
    {
        for (const auto& alias : numberPadAliases())
            if (key.isKeyCode (alias.keyCode))
                return juce::String (alias.characters);
        return juce::String::charToString (static_cast<juce::juce_wchar> (key.getKeyCode()));
    };
    return characters (a).containsAnyOf (characters (b));
}

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
