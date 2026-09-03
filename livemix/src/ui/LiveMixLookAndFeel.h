#pragma once

#include "LiveMixPalette.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::livemix
{

class LiveMixLookAndFeel : public juce::LookAndFeel_V4
{
public:
    LiveMixLookAndFeel()
        : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::ColourScheme (
              Palette::background, Palette::card, Palette::card2, Palette::line, Palette::text,
              Palette::card2, juce::Colours::white, Palette::accent, Palette::text))
    {
        setDefaultSansSerifTypefaceName ("Malgun Gothic");

        setColour (juce::TextButton::buttonColourId, Palette::card2);
        setColour (juce::TextButton::buttonOnColourId, Palette::accent);
        setColour (juce::TextButton::textColourOffId, Palette::text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::ComboBox::backgroundColourId, Palette::card2);
        setColour (juce::ComboBox::outlineColourId, Palette::line);
        setColour (juce::ComboBox::arrowColourId, Palette::dimText);
        setColour (juce::ComboBox::textColourId, Palette::text);
        setColour (juce::ComboBox::focusedOutlineColourId, Palette::accent);
        setColour (juce::TextEditor::backgroundColourId, Palette::card2);
        setColour (juce::TextEditor::outlineColourId, Palette::line);
        setColour (juce::TextEditor::focusedOutlineColourId, Palette::accent);
        setColour (juce::TextEditor::textColourId, Palette::text);
        setColour (juce::TextEditor::highlightColourId, Palette::accent.withAlpha (0.45f));
        setColour (juce::CaretComponent::caretColourId, Palette::text);
        setColour (juce::Label::textColourId, Palette::text);
        setColour (juce::Label::backgroundWhenEditingColourId, Palette::card2);
        setColour (juce::Label::textWhenEditingColourId, Palette::text);
        setColour (juce::Label::outlineWhenEditingColourId, Palette::accent);
        setColour (juce::ToggleButton::textColourId, Palette::text);
        setColour (juce::ToggleButton::tickColourId, Palette::accent);
        setColour (juce::ToggleButton::tickDisabledColourId, Palette::dimText);
        setColour (juce::Slider::backgroundColourId, Palette::slotBg);
        setColour (juce::Slider::trackColourId, Palette::accent);
        setColour (juce::Slider::thumbColourId, juce::Colours::white);
        setColour (juce::PopupMenu::backgroundColourId, Palette::bar);
        setColour (juce::PopupMenu::textColourId, Palette::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::accent);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::PopupMenu::headerTextColourId, Palette::dimText);
        setColour (juce::ScrollBar::thumbColourId, Palette::slotLine);
        setColour (juce::TooltipWindow::backgroundColourId, Palette::card2);
        setColour (juce::TooltipWindow::textColourId, Palette::text);
        setColour (juce::TooltipWindow::outlineColourId, Palette::line);
        setColour (juce::AlertWindow::backgroundColourId, Palette::card);
        setColour (juce::AlertWindow::textColourId, Palette::text);
        setColour (juce::AlertWindow::outlineColourId, Palette::line);
        setColour (juce::ListBox::backgroundColourId, Palette::card);
        setColour (juce::ResizableWindow::backgroundColourId, Palette::background);
        setColour (juce::DocumentWindow::textColourId, Palette::text);
    }

    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (16.0f)); }
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override { return juce::Font (juce::FontOptions (juce::jmin (15.0f, (float) buttonHeight * 0.55f), juce::Font::bold)); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (14.5f)); }
    juce::Font getLabelFont (juce::Label& label) override { return label.getFont(); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        auto colour = backgroundColour;

        if (isButtonDown)
            colour = colour.darker (0.15f);
        else if (isMouseOverButton)
            colour = colour.brighter (0.08f);

        g.setColour (colour);
        g.fillRoundedRectangle (bounds, Palette::controlRadius);
        g.setColour (button.getToggleState() ? Palette::accent : Palette::line);
        g.drawRoundedRectangle (bounds, Palette::controlRadius, 1.0f);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (bounds, Palette::controlRadius);
        g.setColour (box.hasKeyboardFocus (true) ? Palette::accent : Palette::line);
        g.drawRoundedRectangle (bounds, Palette::controlRadius, 1.0f);

        juce::Path arrow;
        const float x = (float) width - 16.0f, y = (float) height * 0.5f;
        arrow.addTriangle (x - 4.0f, y - 2.0f, x + 4.0f, y - 2.0f, x, y + 3.0f);
        g.setColour (Palette::dimText);
        g.fillPath (arrow);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (10, 1, box.getWidth() - 30, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
        label.setMinimumHorizontalScale (1.0f);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float, float,
                           juce::Slider::SliderStyle, juce::Slider& slider) override
    {
        // the send / return fader: a thick rounded track, a filled part and a big white knob
        const float trackH = juce::jmin (16.0f, (float) height * 0.6f);
        const auto track = juce::Rectangle<float> ((float) x, (float) y + ((float) height - trackH) * 0.5f, (float) width, trackH);
        g.setColour (Palette::slotBg);
        g.fillRoundedRectangle (track, trackH * 0.5f);
        g.setColour (Palette::line);
        g.drawRoundedRectangle (track, trackH * 0.5f, 1.0f);

        const float fillW = juce::jlimit (0.0f, (float) width, sliderPos - (float) x);

        if (fillW > 1.0f)
        {
            g.setColour (slider.isEnabled() ? Palette::accent : Palette::dimText);
            g.fillRoundedRectangle (track.withWidth (fillW), trackH * 0.5f);
        }

        const float knobR = juce::jmin (15.0f, (float) height * 0.5f);
        const juce::Point<float> centre (sliderPos, track.getCentreY());
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (juce::Rectangle<float> (knobR * 2.0f, knobR * 2.0f).withCentre (centre.translated (0.0f, 2.0f)));
        g.setColour (juce::Colours::white);
        g.fillEllipse (juce::Rectangle<float> (knobR * 2.0f, knobR * 2.0f).withCentre (centre));
        g.setColour (Palette::accent);
        g.drawEllipse (juce::Rectangle<float> (knobR * 2.0f, knobR * 2.0f).withCentre (centre), 2.0f);
    }

    int getSliderThumbRadius (juce::Slider&) override { return 15; }
};

} // namespace gocue::livemix
