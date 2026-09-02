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

    const juce::int64 totalMs = (juce::int64) std::llround (seconds * 1000.0);
    const juce::int64 minutes = totalMs / 60000;
    const int secs = (int) ((totalMs / 1000) % 60);
    const int millis = (int) (totalMs % 1000);

    if (withMillis)
        return juce::String (minutes) + juce::String::formatted (":%02d.%03d", secs, millis);

    return juce::String (minutes) + juce::String::formatted (":%02d", secs);
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

/** The "큐랩 스타일" theme (design pick 10, 2026-09-02): mid greys, blue selection, bootstrap-like status colours,
    5 px corners and a faint vertical gradient on buttons. */
namespace Palette
{
    const juce::Colour background   { 0xff2b2b2b };   // window / list / fields
    const juce::Colour panel        { 0xff333333 };   // transport, inspector, footer, menus
    const juce::Colour rowEven      { 0xff3a3a3a };
    const juce::Colour rowOdd       { 0xff363636 };
    const juce::Colour header       { 0xff404040 };   // table header
    const juce::Colour button       { 0xff454545 };   // plain buttons
    const juce::Colour field        { 0xff2b2b2b };   // text fields, combos
    const juce::Colour standby      { 0xff2f80ed };   // selection, playhead, accent
    const juce::Colour selected     { 0xff2c4a6e };   // selected row fill
    const juce::Colour playing      { 0xff5cb85c };   // status colours: icons, progress, labels, cart fills
    const juce::Colour fadingOut    { 0xffe9902b };
    const juce::Colour paused       { 0xffe6c84a };
    const juce::Colour playingRow   { 0xff2f4a2f };   // status colours as row backgrounds
    const juce::Colour fadingRow    { 0xff4a3520 };
    const juce::Colour pausedRow    { 0xff4a4020 };
    const juce::Colour missing      { 0xffe8706a };
    const juce::Colour text         { 0xfff5f5f5 };
    const juce::Colour dimText      { 0xffa5a5a5 };
    const juce::Colour goButton     { 0xff5cb85c };
    const juce::Colour stopButton   { 0xffd9534f };
    const juce::Colour outline      { 0xff4d4d4d };
    constexpr float cornerRadius = 5.0f;

    /** The faint top-to-bottom gradient every button carries. */
    inline juce::ColourGradient buttonGradient (juce::Colour base, juce::Rectangle<float> bounds)
    {
        return juce::ColourGradient (base.brighter (0.10f), 0.0f, bounds.getY(), base.darker (0.12f), 0.0f, bounds.getBottom(), false);
    }
}

} // namespace gocue
