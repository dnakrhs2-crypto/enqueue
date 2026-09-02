#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue
{

/** LookAndFeel_V4 with menus that can be read from the desk: 15 pt items, and the shortcut text drawn at the
    same size as the item (V4 shrinks it to 75 %, which is unreadable on the dark menu). */
class GoCueLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GoCueLookAndFeel()
    {
        setDefaultSansSerifTypefaceName ("Malgun Gothic");
    }

    juce::Font getPopupMenuFont() override
    {
        return juce::Font (juce::FontOptions (15.0f));
    }

    juce::Font getMenuBarFont (juce::MenuBarComponent&, int, const juce::String&) override
    {
        return juce::Font (juce::FontOptions (15.0f));
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                            const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColourToUse) override
    {
        if (isSeparator || shortcutKeyText.isEmpty())
        {
            LookAndFeel_V4::drawPopupMenuItem (g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu,
                                               text, shortcutKeyText, icon, textColourToUse);
            return;
        }

        // V4's layout, with the shortcut in the item font
        const auto textColour = textColourToUse == nullptr ? findColour (juce::PopupMenu::textColourId) : *textColourToUse;
        auto r = area.reduced (1);

        if (isHighlighted && isActive)
        {
            g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
            g.fillRect (r);
            g.setColour (findColour (juce::PopupMenu::highlightedTextColourId));
        }
        else
        {
            g.setColour (textColour.withMultipliedAlpha (isActive ? 1.0f : 0.5f));
        }

        r.reduce (juce::jmin (5, area.getWidth() / 20), 0);

        auto font = getPopupMenuFont();
        const float maxFontHeight = (float) r.getHeight() / 1.3f;

        if (font.getHeight() > maxFontHeight)
            font.setHeight (maxFontHeight);

        g.setFont (font);

        const auto iconArea = r.removeFromLeft (juce::roundToInt (maxFontHeight)).toFloat();

        if (icon != nullptr)
        {
            icon->drawWithin (g, iconArea, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize, 1.0f);
            r.removeFromLeft (juce::roundToInt (maxFontHeight * 0.5f));
        }
        else if (isTicked)
        {
            const auto tick = getTickShape (1.0f);
            g.fillPath (tick, tick.getTransformToScaleToFit (iconArea.reduced (iconArea.getWidth() / 5, 0).toFloat(), true));
        }

        if (hasSubMenu)
        {
            const float arrowH = 0.6f * getPopupMenuFont().getAscent();
            const float x = (float) r.removeFromRight ((int) arrowH).getX();
            const float halfH = (float) r.getCentreY();

            juce::Path path;
            path.startNewSubPath (x, halfH - arrowH * 0.5f);
            path.lineTo (x + arrowH * 0.6f, halfH);
            path.lineTo (x, halfH + arrowH * 0.5f);
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }

        r.removeFromRight (3);
        const int shortcutWidth = juce::GlyphArrangement::getStringWidthInt (font, shortcutKeyText);
        const auto shortcutArea = r.removeFromRight (shortcutWidth);
        r.removeFromRight (12);
        g.drawFittedText (text, r, juce::Justification::centredLeft, 1);
        g.drawText (shortcutKeyText, shortcutArea, juce::Justification::centredRight, true);
    }
};

} // namespace gocue
