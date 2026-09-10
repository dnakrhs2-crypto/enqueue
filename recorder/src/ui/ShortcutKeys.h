#pragma once
#include "app/RecorderShortcuts.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>

namespace gocue::recorder
{
inline bool shortcutTextFocus(const juce::Component* origin)
{
    return dynamic_cast<const juce::TextEditor*>(origin)
        || (origin && origin->findParentComponentOfClass<juce::TextEditor>());
}
inline std::optional<RecorderCommand> shortcutCommand(const RecorderShortcuts& bindings,
                                                     const juce::KeyPress& key, const juce::Component* origin)
{
    if (shortcutTextFocus(origin)) return {};
    for (std::size_t i = 0; i < bindings.count; ++i)
        if (key == juce::KeyPress::createFromDescription(bindings.keys[i])) return RecorderCommand(i);
    return {};
}
}
