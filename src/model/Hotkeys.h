#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::Hotkeys
{

/** Keys the app itself uses (GO / pause / fade / panic / preview / load / quick edit / list navigation): a cue hotkey
    may never take one of them. Ctrl / Alt combinations belong to the menus. The same rule guards the inspector's
    hotkey capture and the project loader (a file from elsewhere could carry "Space"). */
inline bool isReservedKey (const juce::KeyPress& key)
{
    static const int reserved[] = { juce::KeyPress::spaceKey, juce::KeyPress::escapeKey, juce::KeyPress::returnKey, juce::KeyPress::tabKey,
                                    juce::KeyPress::deleteKey, juce::KeyPress::backspaceKey, juce::KeyPress::insertKey,
                                    juce::KeyPress::upKey, juce::KeyPress::downKey, juce::KeyPress::leftKey, juce::KeyPress::rightKey,
                                    juce::KeyPress::pageUpKey, juce::KeyPress::pageDownKey, juce::KeyPress::homeKey, juce::KeyPress::endKey,
                                    'P', 'F', 'V', 'L', 'N', 'Q', 'E', 'W', 'C', 'O', 'D', juce::KeyPress::F3Key };

    if (key.getModifiers().isCommandDown() || key.getModifiers().isAltDown())
        return true;

    for (int code : reserved)
        if (key.getKeyCode() == code)
            return true;

    return false;
}

/** A saved hotkey description that the loader must not accept: unparsable, or reserved. */
inline bool isReservedDescription (const juce::String& description)
{
    const auto key = juce::KeyPress::createFromDescription (description);
    return ! key.isValid() || isReservedKey (key);
}

} // namespace gocue::Hotkeys
