#pragma once
#include "app/ShortcutService.h"

namespace gocue
{
/** One vocabulary for current keys in menus, hints, capture and search. */
namespace ShortcutDisplay
{
juce::String key (const juce::KeyPress&);
juce::String keys (const ShortcutKeys&, int maximum = 0);
juce::String currentKeys (const ShortcutService*, juce::CommandID, int maximum = 0, bool showRemainder = true);
}
}
