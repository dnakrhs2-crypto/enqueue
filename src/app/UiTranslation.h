#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <initializer_list>

namespace gocue
{
// Only LiveMix installs this hook, before starting workers. Enqueue keeps the UTF-8 fallback.
using UiTranslator = juce::String (*) (const char*);
inline std::atomic<UiTranslator> uiTranslator { nullptr };

inline juce::String sharedTr (const char* source)
{
    const auto translator = uiTranslator.load (std::memory_order_acquire);
    return translator != nullptr ? translator (source) : juce::String::fromUTF8 (source);
}

// One pass: a value containing "{1}" must remain a value, not become another placeholder.
inline juce::String formatUiText (const juce::String& pattern, std::initializer_list<juce::String> values)
{
    juce::String result;
    for (int i = 0; i < pattern.length();)
    {
        if (pattern[i] == '{')
        {
            const int end = pattern.indexOfChar (i + 1, '}');
            const auto index = end > i + 1 ? pattern.substring (i + 1, end) : juce::String();
            if (index.isNotEmpty() && index.containsOnly ("0123456789")
                && index.length() < 10 && index.getLargeIntValue() < (juce::int64) values.size())
            {
                result += *(values.begin() + index.getIntValue());
                i = end + 1;
                continue;
            }
        }
        result += juce::String::charToString (pattern[i++]);
    }
    return result;
}

template <typename... Values>
juce::String sharedTrf (const char* source, const Values&... values)
{
    return formatUiText (sharedTr (source), { juce::String (values)... });
}
}
