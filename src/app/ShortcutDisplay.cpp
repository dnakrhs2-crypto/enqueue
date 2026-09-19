#include "app/ShortcutDisplay.h"

namespace gocue::ShortcutDisplay
{
juce::String key (const juce::KeyPress& input)
{
    const auto normal = ShortcutKeyCodec::normalise (input);
    const auto mods = normal.getModifiers();
    return juce::String (mods.isCtrlDown() ? "Ctrl+" : "")
         + (mods.isAltDown() ? "Alt+" : "") + (mods.isShiftDown() ? "Shift+" : "")
         + ShortcutKeyCodec::keyName (normal);
}

juce::String keys (const ShortcutKeys& values, int maximum)
{
    if (values.isEmpty())
        return juce::String::fromUTF8 ("미지정");
    const int count = maximum > 0 ? juce::jmin (maximum, values.size()) : values.size();
    juce::StringArray text;
    for (int i = 0; i < count; ++i)
        text.add (key (values[i]));
    auto result = text.joinIntoString (", ");
    if (count < values.size())
        result += " +" + juce::String (values.size() - count) + juce::String::fromUTF8 ("개");
    return result;
}

juce::String currentKeys (const ShortcutService* service, juce::CommandID id, int maximum, bool showRemainder)
{
    const auto* entry = ShortcutCatalog::get().find (id);
    const auto values = service != nullptr ? service->getKeys (id) : entry != nullptr ? entry->defaultKeys : ShortcutKeys();
    if (! showRemainder && maximum > 0)
    {
        ShortcutKeys shown;
        for (int i = 0; i < juce::jmin (maximum, values.size()); ++i) shown.add (values[i]);
        return keys (shown);
    }
    return keys (values, maximum);
}
}
