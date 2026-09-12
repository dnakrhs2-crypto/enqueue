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

/** Enqueue's slate-card tokens, from scratch_ui_designs/enqueue/16_slate-card/mockup.html.
    LiveMix includes this file for the text helpers above, but uses its own gocue::livemix::Palette.
    Keep the original names: other Enqueue controls also consume these tokens. */
namespace Palette
{
    const juce::Colour background    { 0xff1f232a };
    const juce::Colour panel         { 0xff2a2f38 };
    const juce::Colour panel2        { 0xff343a45 };
    const juce::Colour field         { 0xff242932 };
    const juce::Colour outline       { 0xff3e4553 };
    const juce::Colour text          { 0xffe9ecf1 };
    const juce::Colour dimText       { 0xff9ba4b3 };
    const juce::Colour standby       { 0xff8b7cf6 };
    const juce::Colour accentInk     { 0xffffffff };
    const juce::Colour selected      { 0xff3a3f66 };
    const juce::Colour selectionRing { 0xffa59bff };
    const juce::Colour playing       { 0xff4ade80 };
    const juce::Colour fadingOut     { 0xfffb923c };
    const juce::Colour paused        { 0xfff5c451 };
    const juce::Colour playingRow    { 0xff2e4742 };
    const juce::Colour fadingRow     { 0xff473d39 };
    const juce::Colour pausedRow     { 0xff46443c };
    const juce::Colour missing       { 0xfff0605a };
    const juce::Colour onBright      { 0xff06210f };
    const juce::Colour rowEven = panel, rowOdd = panel, header = panel2, button = panel2;
    const juce::Colour muted = dimText, accent = standby, goButton = playing, stopButton = missing, warn = paused;
    const juce::Colour shadowColour { 0x59000000 };
    const juce::Colour highlightColour = accentInk.withAlpha (0.04f);

    constexpr float cornerRadius = 12.0f, fieldRadius = 10.0f, pillRadius = 99.0f;
    constexpr float keyRadius = 3.0f, tickRadius = 4.0f, colourBarRadius = 2.0f;
    constexpr float borderWidth = 1.0f, selectionWidth = 2.0f;
    constexpr float rowBorderAlpha = 0.6f, disabledAlpha = 0.55f, keyAlpha = 0.7f;
    constexpr float statePillAlpha = 0.15f, stopTintAlpha = 0.12f, trackAlpha = 0.7f, menuHighlightAlpha = 0.25f;
    constexpr float hoverBrighten = 0.08f, pressedDarken = 0.2f;
    constexpr float bodySize = 13.0f, headerSize = 11.5f, fileSize = 12.0f, timeSize = 12.5f;
    constexpr float tabSize = 12.5f, pillSize = 11.0f, kickerSize = 11.0f, keySize = 10.5f;
    constexpr float nextNameSize = 28.0f, remainingSize = 26.0f, goSize = 58.0f;
    constexpr float headerTracking = 0.02f, kickerTracking = 0.08f;
    constexpr float tickSize = 15.0f;
    constexpr float groupTextStroke = 0.25f, goTextStroke = 0.8f;
    constexpr int gap = 12, cardInset = 10, cardHeaderHeight = 40, tabBarHeight = 41;
    constexpr int menuBarHeight = 32, transportHeight = 128, footerHeight = 30;
    constexpr int goWidth = 220, transportWidth = 360, buttonGap = 8;
    constexpr int tableHeaderHeight = 30, rowHeights[] = { 28, 32, 40 };
    constexpr int statusColumnWidth = 34, numberColumnWidth = 52, fileColumnWidth = 250;
    constexpr int preWaitColumnWidth = 92, durationColumnWidth = 92, postWaitColumnWidth = 104, continueColumnWidth = 46;
    constexpr int childIndent = 24, statusIconSize = 16, colourBarWidth = 3, colourBarHeight = 18;
    constexpr int playheadWidth = 4, rowProgressHeight = 2, progressHeight = 6, scrollBarWidth = 6;
    constexpr int activeCardHeight = 122, miniButtonHeight = 26, pillHeight = 22;
    constexpr int shadowRadius = 24, shadowOffsetY = 8;
    constexpr const char* bodyTypeface = "Malgun Gothic";
    constexpr const char* preferredMonoTypeface = "Cascadia Mono";
    constexpr const char* fallbackMonoTypeface = "Consolas";

    inline juce::Font font (float size = bodySize, bool bold = false)
    {
        return juce::Font (juce::FontOptions (bodyTypeface, size, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::Font monoFont (float size)
    {
        // Query once, on first use by the UI; never enumerate installed fonts in a paint loop.
        static const juce::String family = []
        {
            const auto names = juce::Font::findAllTypefaceNames();
            return juce::String (names.contains (preferredMonoTypeface, true) ? preferredMonoTypeface : fallbackMonoTypeface);
        }();
        return juce::Font (juce::FontOptions (family, size, juce::Font::plain));
    }

    inline juce::Font goFont()
    {
        static const juce::String style = []
        {
            const auto styles = juce::Font::findAllTypefaceStyles (bodyTypeface);
            for (const auto* name : { "Black", "Heavy", "ExtraBold", "Bold" })
                if (styles.contains (name, true))
                    return juce::String (name);
            return juce::String ("Bold");
        }();
        return juce::Font (juce::FontOptions (bodyTypeface, style, goSize));
    }

    inline void drawTopHighlight (juce::Graphics& g, const juce::Path& shape, juce::Rectangle<float> bounds)
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (bounds.withHeight (borderWidth + 0.5f).getSmallestIntegerContainer());
        g.setColour (highlightColour);
        g.strokePath (shape, juce::PathStrokeType (borderWidth));
    }

    /** Malgun Gothic normally has only Regular/Bold. A small outline supplies the mockup's heavier headings. */
    inline void drawHeavyText (juce::Graphics& g, const juce::String& label, juce::Rectangle<int> bounds,
                               const juce::Font& labelFont, juce::Justification justification, float stroke)
    {
        if (bounds.isEmpty())
            return;
        juce::GlyphArrangement glyphs;
        glyphs.addFittedText (labelFont, label, (float) bounds.getX(), (float) bounds.getY(),
                             (float) bounds.getWidth(), (float) bounds.getHeight(), justification, 1, 1.0f);
        juce::Path outlinePath;
        glyphs.createPath (outlinePath);
        g.fillPath (outlinePath);
        g.strokePath (outlinePath, juce::PathStrokeType (stroke));
    }

    inline void drawCard (juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        if (bounds.isEmpty())
            return;
        const auto r = bounds.toFloat().reduced (0.5f);
        juce::Path shape;
        shape.addRoundedRectangle (r, cornerRadius);
        juce::DropShadow { shadowColour, shadowRadius, { 0, shadowOffsetY } }.drawForPath (g, shape);
        g.setColour (panel);
        g.fillPath (shape);
        g.setColour (outline);
        g.strokePath (shape, juce::PathStrokeType (borderWidth));
        drawTopHighlight (g, shape, r);
    }

    /** Finish a card after its existing child components paint, without changing their parentage. */
    inline void finishCard (juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        if (bounds.isEmpty())
            return;
        juce::Path corners;
        corners.setUsingNonZeroWinding (false);
        corners.addRectangle (bounds.toFloat());
        corners.addRoundedRectangle (bounds.toFloat(), cornerRadius);
        g.setColour (background);
        g.fillPath (corners);
        g.setColour (outline);
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), cornerRadius, borderWidth);
        juce::Path shape;
        shape.addRoundedRectangle (bounds.toFloat().reduced (0.5f), cornerRadius);
        drawTopHighlight (g, shape, bounds.toFloat().reduced (0.5f));
    }

    inline void drawPill (juce::Graphics& g, juce::Rectangle<int> bounds, juce::Colour colour,
                          const juce::String& label, const juce::Font& labelFont, bool filled = false)
    {
        const auto r = bounds.toFloat().reduced (0.5f);
        g.setColour (colour);
        if (filled)
            g.fillRoundedRectangle (r, pillRadius);
        else
            g.drawRoundedRectangle (r, pillRadius, borderWidth);
        g.setColour (filled ? accentInk : colour);
        g.setFont (labelFont);
        g.drawText (label, bounds.reduced (7, 0), juce::Justification::centred, true);
    }

    inline juce::Path topTabShape (juce::Rectangle<float> r)
    {
        juce::Path shape;
        shape.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), cornerRadius, cornerRadius,
                                   true, true, false, false);
        return shape;
    }

    inline void drawTab (juce::Graphics& g, juce::Rectangle<int> bounds, bool activeTab)
    {
        if (! activeTab)
            return;
        const auto r = bounds.toFloat().reduced (0.5f, 0.0f).withTrimmedTop (0.5f);
        const auto shape = topTabShape (r);
        g.setColour (panel);
        g.fillPath (shape);
        g.setColour (outline);
        g.strokePath (shape, juce::PathStrokeType (borderWidth));
        g.setColour (panel);
        g.fillRect (r.getX() + borderWidth, r.getBottom() - borderWidth, r.getWidth() - 2.0f * borderWidth, borderWidth);
    }

    /** Compatibility for controls outside this round; the new palette's button surface is flat. */
    inline juce::ColourGradient buttonGradient (juce::Colour base, juce::Rectangle<float> bounds)
    {
        return juce::ColourGradient (base, 0.0f, bounds.getY(), base, 0.0f, bounds.getBottom(), false);
    }
}

} // namespace gocue
