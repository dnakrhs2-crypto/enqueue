#pragma once

#include "ui/UiUtils.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue
{

/** LookAndFeel_V4 in the 큐랩 스타일 palette (see Palette), with 5 px corners and a faint gradient on buttons,
    and menus that can be read from the desk: 15 pt items, the shortcut text drawn at the same size as the item
    (V4 shrinks it to 75 %, which is unreadable on a dark menu). */
class GoCueLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GoCueLookAndFeel()
        : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::ColourScheme (
              Palette::background, Palette::panel, Palette::panel, Palette::outline, Palette::text,
              Palette::button, juce::Colours::white, Palette::standby, Palette::text))
    {
        setDefaultSansSerifTypefaceName ("Malgun Gothic");

        setColour (juce::TextButton::buttonColourId, Palette::button);
        setColour (juce::TextButton::buttonOnColourId, Palette::standby);
        setColour (juce::TextButton::textColourOffId, Palette::text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::ComboBox::backgroundColourId, Palette::field);
        setColour (juce::ComboBox::outlineColourId, Palette::outline);
        setColour (juce::ComboBox::arrowColourId, Palette::dimText);
        setColour (juce::ComboBox::textColourId, Palette::text);
        setColour (juce::ComboBox::focusedOutlineColourId, Palette::standby);
        setColour (juce::TextEditor::backgroundColourId, Palette::field);
        setColour (juce::TextEditor::outlineColourId, Palette::outline);
        setColour (juce::TextEditor::focusedOutlineColourId, Palette::standby);
        setColour (juce::TextEditor::textColourId, Palette::text);
        setColour (juce::TextEditor::highlightColourId, Palette::standby.withAlpha (0.45f));
        setColour (juce::CaretComponent::caretColourId, Palette::text);
        setColour (juce::Label::textColourId, Palette::text);
        setColour (juce::ToggleButton::textColourId, Palette::text);
        setColour (juce::ToggleButton::tickColourId, Palette::standby);
        setColour (juce::ToggleButton::tickDisabledColourId, Palette::dimText);
        setColour (juce::TableHeaderComponent::backgroundColourId, Palette::header);
        setColour (juce::TableHeaderComponent::textColourId, Palette::text);
        setColour (juce::TableHeaderComponent::outlineColourId, Palette::outline);
        setColour (juce::TableHeaderComponent::highlightColourId, Palette::standby.withAlpha (0.35f));
        setColour (juce::ListBox::backgroundColourId, Palette::background);
        setColour (juce::ListBox::outlineColourId, Palette::outline);
        setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff5e5e5e));
        setColour (juce::ScrollBar::backgroundColourId, Palette::background);
        setColour (juce::PopupMenu::backgroundColourId, Palette::panel);
        setColour (juce::PopupMenu::textColourId, Palette::text);
        setColour (juce::PopupMenu::headerTextColourId, Palette::dimText);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::standby);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TabbedButtonBar::tabTextColourId, Palette::dimText);
        setColour (juce::TabbedButtonBar::frontTextColourId, Palette::text);
        setColour (juce::TabbedButtonBar::tabOutlineColourId, Palette::outline);
        setColour (juce::TabbedButtonBar::frontOutlineColourId, Palette::outline);
        setColour (juce::Slider::thumbColourId, Palette::standby);
        setColour (juce::Slider::trackColourId, Palette::outline);
        setColour (juce::Slider::backgroundColourId, Palette::field);
        setColour (juce::Slider::textBoxBackgroundColourId, Palette::field);
        setColour (juce::Slider::textBoxTextColourId, Palette::text);
        setColour (juce::Slider::textBoxOutlineColourId, Palette::outline);
        setColour (juce::AlertWindow::backgroundColourId, Palette::panel);
        setColour (juce::AlertWindow::textColourId, Palette::text);
        setColour (juce::AlertWindow::outlineColourId, Palette::outline);
        setColour (juce::TooltipWindow::backgroundColourId, Palette::header);
        setColour (juce::TooltipWindow::textColourId, Palette::text);
        setColour (juce::TooltipWindow::outlineColourId, Palette::outline);
        setColour (juce::ResizableWindow::backgroundColourId, Palette::background);
        setColour (juce::DocumentWindow::textColourId, Palette::text);
        setColour (juce::GroupComponent::outlineColourId, Palette::outline);
        setColour (juce::GroupComponent::textColourId, Palette::dimText);
        setColour (juce::ProgressBar::backgroundColourId, Palette::field);
        setColour (juce::ProgressBar::foregroundColourId, Palette::standby);
    }

    /** Buttons: 5 px corners, a faint top-to-bottom gradient, a darker edge (V4 draws them flat with 6 px corners). */
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override
    {
        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f, 0.5f);
        auto base = backgroundColour.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f);

        if (isButtonDown)
            base = base.darker (0.2f);
        else if (isMouseOverButton)
            base = base.brighter (0.08f);

        g.setGradientFill (Palette::buttonGradient (base, bounds));
        g.fillRoundedRectangle (bounds, Palette::cornerRadius);
        g.setColour (base.darker (0.4f).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.drawRoundedRectangle (bounds, Palette::cornerRadius, 1.0f);
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
