#pragma once

#include <juce_graphics/juce_graphics.h>

#include <cmath>

namespace gocue
{

/** Wraps a UTF-8 literal (Korean UI text) into a juce::String. */
inline juce::String ko (const char* utf8)
{
    return juce::String::fromUTF8 (utf8);
}

/** 0 or negative -> "--:--", otherwise m:ss.t */
inline juce::String formatSeconds (double seconds)
{
    if (! (seconds > 0.0))
        return "--:--";

    const int whole = (int) seconds;
    const int minutes = whole / 60;
    const int secs = whole % 60;
    const int tenths = juce::jlimit (0, 9, (int) ((seconds - (double) whole) * 10.0));

    return juce::String::formatted ("%d:%02d.%d", minutes, secs, tenths);
}

/** m:ss.mmm (or m:ss when 'withMillis' is false). Negative -> 0. */
inline juce::String formatTimeMs (double seconds, bool withMillis = true)
{
    if (! (seconds > 0.0))
        seconds = 0.0;

    const int totalMs = (int) std::llround (seconds * 1000.0);
    const int minutes = totalMs / 60000;
    const int secs = (totalMs / 1000) % 60;
    const int millis = totalMs % 1000;

    if (withMillis)
        return juce::String::formatted ("%d:%02d.%03d", minutes, secs, millis);

    return juce::String::formatted ("%d:%02d", minutes, secs);
}

/** Parses "12.5", "1:02.250", "1:02" or "0:00:05" into seconds. Returns -1 when unparsable. */
inline double parseTimeText (juce::String text)
{
    text = text.trim().replace (",", ".");

    if (text.isEmpty())
        return -1.0;

    juce::StringArray parts;
    parts.addTokens (text, ":", "");
    parts.trim();

    if (parts.size() < 1 || parts.size() > 3)
        return -1.0;

    double seconds = 0.0;

    for (const auto& part : parts)
    {
        if (part.isEmpty() || ! part.containsOnly ("0123456789."))
            return -1.0;

        seconds = seconds * 60.0 + part.getDoubleValue();
    }

    return seconds >= 0.0 ? seconds : -1.0;
}

namespace Palette
{
    const juce::Colour background   { 0xff1b1b1f };
    const juce::Colour panel        { 0xff24242a };
    const juce::Colour rowEven      { 0xff2a2a31 };
    const juce::Colour rowOdd       { 0xff26262c };
    const juce::Colour standby      { 0xff3d8bfd };
    const juce::Colour playing      { 0xff1f7a3a };
    const juce::Colour fadingOut    { 0xffb35f00 };
    const juce::Colour paused       { 0xff8a7a1c };
    const juce::Colour missing      { 0xffff6b6b };
    const juce::Colour text         { 0xffe8e8ea };
    const juce::Colour dimText      { 0xff9a9aa3 };
    const juce::Colour goButton     { 0xff2ea043 };
    const juce::Colour stopButton   { 0xff8b2f2f };
    const juce::Colour outline      { 0xff3a3a44 };
}

} // namespace gocue
