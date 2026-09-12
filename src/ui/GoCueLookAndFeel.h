#pragma once

#include "ui/UiUtils.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue
{

/** Enqueue's slate cards and controls. All theme tokens live in Palette. */
class GoCueLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GoCueLookAndFeel()
        : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::ColourScheme (
              Palette::background, Palette::panel, Palette::panel, Palette::outline, Palette::text,
              Palette::button, Palette::accentInk, Palette::standby, Palette::text))
    {
        setDefaultSansSerifTypefaceName (Palette::bodyTypeface);

        setColour (juce::TextButton::buttonColourId, Palette::button);
        setColour (juce::TextButton::buttonOnColourId, Palette::standby);
        setColour (juce::TextButton::textColourOffId, Palette::text);
        setColour (juce::TextButton::textColourOnId, Palette::accentInk);
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
        setColour (juce::TableHeaderComponent::textColourId, Palette::muted);
        setColour (juce::TableHeaderComponent::outlineColourId, Palette::outline);
        setColour (juce::TableHeaderComponent::highlightColourId, Palette::standby.withAlpha (0.35f));
        setColour (juce::ListBox::backgroundColourId, Palette::panel);
        setColour (juce::ListBox::outlineColourId, Palette::outline);
        setColour (juce::ScrollBar::thumbColourId, Palette::muted);
        setColour (juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::PopupMenu::backgroundColourId, Palette::panel);
        setColour (juce::PopupMenu::textColourId, Palette::text);
        setColour (juce::PopupMenu::headerTextColourId, Palette::dimText);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::accent.withAlpha (Palette::menuHighlightAlpha));
        setColour (juce::PopupMenu::highlightedTextColourId, Palette::text);
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

    void drawTableHeaderBackground (juce::Graphics& g, juce::TableHeaderComponent& header) override
    {
        g.fillAll (Palette::panel2);
        g.setColour (Palette::outline);
        g.fillRect (0, header.getHeight() - 1, header.getWidth(), 1);
    }

    /** Slate header typography applies to every column, including the number and name. */
    void drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent& header, const juce::String& columnName,
                                int, int width, int height, bool isMouseOver, bool isMouseDown, int columnFlags) override
    {
        static const juce::StringArray centredColumns { juce::String::fromUTF8 ("\xED\x94\x84\xEB\xA6\xAC\xEC\x9B\xA8\xEC\x9D\xB4\xED\x8A\xB8"),   // 프리웨이트
                                                        juce::String::fromUTF8 ("\xEA\xB8\xB8\xEC\x9D\xB4"),                                         // 길이
                                                        juce::String::fromUTF8 ("\xED\x8F\xAC\xEC\x8A\xA4\xED\x8A\xB8\xEC\x9B\xA8\xEC\x9D\xB4\xED\x8A\xB8"),   // 포스트웨이트
                                                        juce::String::fromUTF8 ("\xEC\xA7\x84\xED\x96\x89") };                                       // 진행

        auto highlightColour = header.findColour (juce::TableHeaderComponent::highlightColourId);

        if (isMouseDown)
            g.fillAll (highlightColour);
        else if (isMouseOver)
            g.fillAll (highlightColour.withMultipliedAlpha (0.625f));

        juce::Rectangle<int> area (width, height);
        area.reduce (4, 0);

        if ((columnFlags & (juce::TableHeaderComponent::sortedForwards | juce::TableHeaderComponent::sortedBackwards)) != 0)
        {
            juce::Path sortArrow;
            sortArrow.addTriangle (0.0f, 0.0f, 0.5f, (columnFlags & juce::TableHeaderComponent::sortedForwards) != 0 ? -0.8f : 0.8f, 1.0f, 0.0f);
            g.setColour (Palette::muted);
            g.fillPath (sortArrow, sortArrow.getTransformToScaleToFit (area.removeFromRight (height / 2).reduced (2).toFloat(), true));
        }

        g.setColour (columnName == ko ("진행") ? Palette::accent : header.findColour (juce::TableHeaderComponent::textColourId));
        auto font = Palette::font (Palette::headerSize, true);
        font.setExtraKerningFactor (Palette::headerTracking);
        g.setFont (font);
        g.drawText (columnName, area, centredColumns.contains (columnName) ? juce::Justification::centred : juce::Justification::centredLeft, true);
    }

    juce::Font getTextButtonFont (juce::TextButton& button, int) override
    {
        if (button.getProperties().getWithDefault ("slateSegment", false))
            return Palette::font (Palette::fileSize, button.getToggleState());
        return Palette::font (button.getProperties().getWithDefault ("slateSmall", false) ? Palette::headerSize : Palette::bodySize, true);
    }

    /** Flat surfaces; connected mode segments share one perimeter drawn by their owner. */
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override
    {
        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f, 0.5f);
        auto base = backgroundColour.withMultipliedAlpha (button.isEnabled() ? 1.0f : Palette::disabledAlpha);

        if (isButtonDown)
            base = base.darker (Palette::pressedDarken);
        else if (isMouseOverButton)
            base = base.brighter (Palette::hoverBrighten);

        // buttons glued to a neighbour (a plugin's generic editor, the plugin manager's path row) keep that side square
        const auto flags = button.getConnectedEdgeFlags();
        const bool flatOnLeft   = (flags & juce::Button::ConnectedOnLeft) != 0;
        const bool flatOnRight  = (flags & juce::Button::ConnectedOnRight) != 0;
        const bool flatOnTop    = (flags & juce::Button::ConnectedOnTop) != 0;
        const bool flatOnBottom = (flags & juce::Button::ConnectedOnBottom) != 0;

        const bool pill = button.getProperties().getWithDefault ("slatePill", false);
        const bool small = button.getProperties().getWithDefault ("slateSmall", false);
        const bool segment = button.getProperties().getWithDefault ("slateSegment", false);
        const float radius = pill ? Palette::pillRadius : small ? Palette::fieldRadius : Palette::cornerRadius;
        juce::Path shape;
        shape.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), radius, radius,
                                   ! (flatOnLeft || flatOnTop), ! (flatOnRight || flatOnTop), ! (flatOnLeft || flatOnBottom), ! (flatOnRight || flatOnBottom));

        if (! small && ! segment && ! pill)
            juce::DropShadow { Palette::shadowColour, Palette::shadowRadius, { 0, Palette::shadowOffsetY } }.drawForPath (g, shape);
        g.setColour (base);
        g.fillPath (shape);
        if (! segment)
        {
            g.setColour (button.getProperties().getWithDefault ("slateColourOutline", false)
                             ? button.findColour (juce::TextButton::textColourOffId) : Palette::outline);
            g.strokePath (shape, juce::PathStrokeType (Palette::borderWidth));
            Palette::drawTopHighlight (g, shape, bounds);
        }

        if (button.hasKeyboardFocus (true))   // the dialogs' buttons take focus: show which one Tab landed on
        {
            g.setColour (Palette::accent);
            g.strokePath (shape, juce::PathStrokeType (Palette::selectionWidth));
        }
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox& box) override
    {
        const auto r = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, Palette::fieldRadius);
        g.setColour (box.findColour (box.hasKeyboardFocus (true) ? juce::ComboBox::focusedOutlineColourId : juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r, Palette::fieldRadius, Palette::borderWidth);
        const auto c = juce::Rectangle<int> (buttonX, buttonY, buttonW, buttonH).toFloat().getCentre();
        juce::Path arrow;
        arrow.startNewSubPath (c.x - 4.0f, c.y - 2.0f);
        arrow.lineTo (c.x, c.y + 2.0f);
        arrow.lineTo (c.x + 4.0f, c.y - 2.0f);
        g.setColour (Palette::muted.withMultipliedAlpha (box.isEnabled() ? 1.0f : Palette::disabledAlpha));
        g.strokePath (arrow, juce::PathStrokeType (1.5f));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return Palette::font (Palette::timeSize); }

    void fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
        g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), Palette::fieldRadius);
    }

    void drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
        g.setColour (editor.findColour (editor.hasKeyboardFocus (true) ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
        g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), Palette::fieldRadius, Palette::borderWidth);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool over, bool down) override
    {
        const float size = Palette::tickSize;
        drawTickBox (g, button, 0.5f, ((float) button.getHeight() - size) * 0.5f, size, size,
                     button.getToggleState(), button.isEnabled(), over, down);
        g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (button.isEnabled() ? 1.0f : Palette::disabledAlpha));
        g.setFont (Palette::font (Palette::timeSize));
        g.drawText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft (23), juce::Justification::centredLeft, true);
    }

    void drawTickBox (juce::Graphics& g, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool enabled, bool over, bool) override
    {
        const auto r = juce::Rectangle<float> (x, y, w, h).reduced (0.5f);
        g.setColour ((ticked ? Palette::accent : Palette::field).withMultipliedAlpha (enabled ? 1.0f : Palette::disabledAlpha));
        g.fillRoundedRectangle (r, Palette::tickRadius);
        g.setColour (ticked || over ? Palette::accent : Palette::muted);
        g.drawRoundedRectangle (r, Palette::tickRadius, Palette::borderWidth);
        if (ticked)
        {
            const auto tick = getTickShape (1.0f);
            g.setColour (Palette::accentInk);
            g.fillPath (tick, tick.getTransformToScaleToFit (r.reduced (3.0f), true));
        }
    }

    int getDefaultScrollbarWidth() override { return Palette::scrollBarWidth; }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                         int thumbStart, int thumbSize, bool over, bool down) override
    {
        const auto thumb = vertical ? juce::Rectangle<int> (x + (width - Palette::scrollBarWidth) / 2, thumbStart, Palette::scrollBarWidth, thumbSize)
                                    : juce::Rectangle<int> (thumbStart, y + (height - Palette::scrollBarWidth) / 2, thumbSize, Palette::scrollBarWidth);
        g.setColour (over || down ? Palette::text : Palette::muted);
        g.fillRoundedRectangle (thumb.toFloat(), Palette::pillRadius);
    }

    int getTabButtonOverlap (int) override { return 0; }
    int getTabButtonBestWidth (juce::TabBarButton& button, int) override
    {
        return juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::tabSize, true), button.getButtonText()) + 28;
    }

    void createTabButtonShape (juce::TabBarButton& button, juce::Path& path, bool, bool) override
    {
        path = Palette::topTabShape (button.getLocalBounds().toFloat());
    }

    void drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool, bool) override
    {
        g.fillAll (Palette::panel2);
        const auto r = button.getLocalBounds().withTrimmedTop (6).reduced (1, 0);
        const bool active = button.isFrontTab();
        Palette::drawTab (g, r, active);
        g.setColour (active ? Palette::text : Palette::muted);
        g.setFont (Palette::font (Palette::tabSize, active));
        g.drawText (button.getButtonText(), r.reduced (12, 0), juce::Justification::centred, true);
    }

    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar& bar, juce::Graphics& g, int w, int h) override
    {
        // JUCE paints this component over the inactive tabs: fill the gaps without erasing their text.
        {
            juce::Graphics::ScopedSaveState save (g);
            for (int i = 0; i < bar.getNumTabs(); ++i)
                if (auto* button = bar.getTabButton (i); button != nullptr && button->isVisible())
                    g.excludeClipRegion (button->getBounds());
            g.fillAll (Palette::panel2);
        }
        g.setColour (Palette::outline);
        g.fillRect (0, h - 1, w, 1);
    }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        Palette::drawCard (g, { width, height });
    }

    void drawMenuBarBackground (juce::Graphics& g, int width, int height, bool, juce::MenuBarComponent&) override
    {
        g.fillAll (Palette::panel);
        g.setColour (Palette::outline);
        g.fillRect (0, height - 1, width, 1);
    }

    /** A combo box shows a long entry (a cue name) cut with an ellipsis, never with squashed glyphs. */
    juce::Label* createComboBoxTextBox (juce::ComboBox& box) override
    {
        auto* label = juce::LookAndFeel_V4::createComboBoxTextBox (box);
        label->setMinimumHorizontalScale (1.0f);
        return label;
    }

    juce::Font getPopupMenuFont() override
    {
        return Palette::font();
    }

    /** Keep the existing generous menu hit areas and full-size shortcut text. */
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override
    {
        LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);

        if (! isSeparator)
        {
            idealHeight = juce::jmax (idealHeight, juce::roundToInt (getPopupMenuFont().getHeight() * 1.7f));
            idealWidth += 20;
        }
    }

    juce::Font getMenuBarFont (juce::MenuBarComponent&, int, const juce::String&) override
    {
        return Palette::font();
    }

    int getDefaultMenuBarHeight() override
    {
        return Palette::menuBarHeight;
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
