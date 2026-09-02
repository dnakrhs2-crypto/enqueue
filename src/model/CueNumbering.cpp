#include "model/CueNumbering.h"

#include <cmath>

namespace gocue::CueNumbering
{

juce::String format (double value)
{
    if (! std::isfinite (value))
        return {};

    const double rounded = std::round (value * 1000.0) / 1000.0;
    juce::String text (rounded, 3);

    if (text.containsChar ('.'))
    {
        text = text.trimCharactersAtEnd ("0");
        text = text.trimCharactersAtEnd (".");
    }

    return text;
}

bool isNumeric (const juce::String& text)
{
    const auto t = text.trim();

    if (t.isEmpty() || ! t.containsOnly ("0123456789."))
        return false;

    return t.indexOfChar ('.') == t.lastIndexOfChar ('.') && t != ".";
}

bool isUnique (const std::vector<Cue>& cues, const juce::String& number, const juce::Uuid& except)
{
    if (number.isEmpty())
        return true;

    for (const auto& c : cues)
        if (c.id != except && c.number == number)
            return false;

    return true;
}

juce::String next (const std::vector<Cue>& cues, int insertIndex, double increment)
{
    if (! (increment > 0.0))
        return {};

    double base = 0.0;
    bool found = false;

    if (insertIndex > 0 && insertIndex - 1 < (int) cues.size() && isNumeric (cues[(size_t) insertIndex - 1].number))
    {
        base = cues[(size_t) insertIndex - 1].number.getDoubleValue();
        found = true;
    }

    if (! found)
    {
        for (const auto& c : cues)
        {
            if (isNumeric (c.number))
            {
                base = juce::jmax (base, c.number.getDoubleValue());
                found = true;
            }
        }
    }

    double candidate = found ? base + increment : increment;

    for (int guard = 0; guard < 100000; ++guard)
    {
        const auto text = format (candidate);

        if (isUnique (cues, text))
            return text;

        candidate += increment;
    }

    return {};
}

std::vector<juce::String> generate (int count, const RenumberOptions& options)
{
    std::vector<juce::String> result;

    for (int i = 0; i < count; ++i)
        result.push_back (options.prefix + format (options.start + i * options.increment) + options.suffix);

    return result;
}

int compare (const juce::String& a, const juce::String& b)
{
    if (isNumeric (a) && isNumeric (b))
    {
        const double x = a.getDoubleValue(), y = b.getDoubleValue();
        return x < y ? -1 : (x > y ? 1 : 0);
    }

    return a.compareNatural (b);
}

} // namespace gocue::CueNumbering
