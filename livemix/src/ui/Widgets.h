#pragma once

#include "LiveMixPalette.h"
#include "MixEngine.h"
#include "ui/UiUtils.h"   // ko()

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::livemix
{

inline juce::Font captionFont() { return juce::Font (juce::FontOptions (pt (12.5f), juce::Font::bold)); }
inline juce::Font bodyFont (float size = 14.5f) { return juce::Font (juce::FontOptions (pt (size))); }
inline juce::Font titleFont (float size = 17.0f) { return juce::Font (juce::FontOptions (pt (size), juce::Font::bold)); }

inline void styleCaption (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (captionFont());
    label.setColour (juce::Label::textColourId, Palette::dimText);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setMinimumHorizontalScale (1.0f);
}

/** A horizontal peak meter with hold and decay, fed from MixEngine::Meter values (max since the last read). */
class MeterBar : public juce::Component
{
public:
    explicit MeterBar (bool stereoBars = false) : stereo (stereoBars) {}

    void push (MixEngine::Meter m)
    {
        push (0, m.left);
        push (1, stereo ? m.right : m.left);
        repaint();
    }

    void setStereo (bool shouldBeStereo) { stereo = shouldBeStereo; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const int rows = stereo ? 2 : 1;
        auto area = getLocalBounds();
        const int scaleH = 12;
        auto bars = area.removeFromTop (juce::jmax (0, area.getHeight() - scaleH));
        const int gap = 4;
        const int barH = juce::jmax (4, (bars.getHeight() - gap * (rows - 1)) / rows);

        for (int r = 0; r < rows; ++r)
        {
            auto bar = juce::Rectangle<int> (bars.getX(), bars.getY() + r * (barH + gap), bars.getWidth(), barH).toFloat();
            g.setColour (Palette::meterBg);
            g.fillRoundedRectangle (bar, barH * 0.5f);

            const float level = juce::jlimit (0.0f, 1.0f, position (display[r]));

            if (level > 0.001f)
            {
                juce::ColourGradient grad (Palette::meterGreen, bar.getX(), 0.0f, Palette::meterRed, bar.getRight(), 0.0f, false);
                grad.addColour (0.6, Palette::meterGreen);
                grad.addColour (0.8, Palette::meterYellow);
                g.setGradientFill (grad);
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * level), barH * 0.5f);
            }

            const float peak = juce::jlimit (0.0f, 1.0f, position (hold[r]));

            if (peak > 0.001f)
            {
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.fillRect (juce::Rectangle<float> (bar.getX() + bar.getWidth() * peak - 1.0f, bar.getY(), 2.0f, bar.getHeight()));
            }
        }

        g.setColour (Palette::dimText);
        g.setFont (juce::Font (juce::FontOptions (pt (10.5f))));
        const char* marks[] = { "-40", "-20", "-10", "-6", "0" };
        const float dbs[] = { -40.0f, -20.0f, -10.0f, -6.0f, 0.0f };

        for (int i = 0; i < 5; ++i)
        {
            const float x = area.getX() + area.getWidth() * position (juce::Decibels::decibelsToGain (dbs[i]));
            const auto justification = i == 0 ? juce::Justification::centredLeft : i == 4 ? juce::Justification::centredRight : juce::Justification::centred;
            g.drawText (marks[i], juce::Rectangle<int> ((int) x - 16, area.getY(), 32, scaleH), justification, false);
        }
    }

private:
    static float position (float gain)
    {
        // -40 dB at the left edge, 0 dB at the right
        const float db = juce::Decibels::gainToDecibels (gain, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 40.0f) / 40.0f);
    }

    void push (int row, float value)
    {
        display[row] = juce::jmax (value, display[row] * 0.82f);   // ~ -6 dB per 30 Hz tick: a smooth fall

        if (value >= hold[row])
        {
            hold[row] = value;
            holdTicks[row] = 45;   // 1.5 s at 30 Hz
        }
        else if (--holdTicks[row] <= 0)
            hold[row] = juce::jmax (value, hold[row] * 0.9f);
    }

    bool stereo;
    float display[2] = { 0.0f, 0.0f }, hold[2] = { 0.0f, 0.0f };
    int holdTicks[2] = { 0, 0 };
};

/** The mic ON/OFF button: a lamp that lights up, big text. */
class LampButton : public juce::Button
{
public:
    LampButton() : juce::Button ("mic")
    {
        setClickingTogglesState (false);
        setWantsKeyboardFocus (false);
    }

    void setOn (bool shouldBeOn)
    {
        if (on != shouldBeOn)
        {
            on = shouldBeOn;
            repaint();
        }
    }

    bool isOn() const noexcept { return on; }

    /** Muted by the mic mute group: drawn red, whatever the switch says (the switch is kept). */
    void setMuted (bool shouldBeMuted)
    {
        if (muted != shouldBeMuted)
        {
            muted = shouldBeMuted;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        auto fill = Palette::card2;

        if (isButtonDown)
            fill = fill.darker (0.15f);
        else if (isMouseOverButton)
            fill = fill.brighter (0.08f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, 11.0f);
        g.setColour (muted ? Palette::danger : on ? Palette::lampOn : Palette::line);
        g.drawRoundedRectangle (bounds, 11.0f, muted ? 2.0f : 1.0f);

        if (muted)
        {
            const float lampR = 8.0f;
            const juce::Point<float> centre (bounds.getX() + 18.0f, bounds.getCentreY());
            g.setColour (Palette::danger);
            g.fillEllipse (juce::Rectangle<float> (lampR * 2.0f, lampR * 2.0f).withCentre (centre));
            g.setColour (Palette::danger);
            g.setFont (juce::Font (juce::FontOptions (pt (15.0f), juce::Font::bold)));
            g.drawText (ko ("뮤트그룹 - 뮤트 중"), getLocalBounds().withTrimmedLeft (36), juce::Justification::centredLeft, false);
            return;
        }

        const float lampR = 8.0f;
        const juce::Point<float> centre (bounds.getX() + 18.0f, bounds.getCentreY());

        if (on)
        {
            g.setColour (Palette::lampOn.withAlpha (0.35f));
            g.fillEllipse (juce::Rectangle<float> (lampR * 3.0f, lampR * 3.0f).withCentre (centre));
        }

        g.setColour (on ? Palette::lampOn : Palette::lampOff);
        g.fillEllipse (juce::Rectangle<float> (lampR * 2.0f, lampR * 2.0f).withCentre (centre));

        g.setColour (on ? Palette::text : Palette::dimText);
        g.setFont (juce::Font (juce::FontOptions (pt (15.0f), juce::Font::bold)));
        g.drawText (on ? ko ("마이크 ON") : ko ("마이크 OFF"), getLocalBounds().withTrimmedLeft (36), juce::Justification::centredLeft, false);
    }

private:
    bool on = true, muted = false;
};

/** A toggle chip (마스터 / 직접 출력): outlined in the accent colour when on. */
class Chip : public juce::TextButton
{
public:
    Chip() : Chip (juce::String()) {}
    explicit Chip (const juce::String& text) : juce::TextButton (text)
    {
        setClickingTogglesState (true);
        setWantsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        auto fill = Palette::card2;

        if (isButtonDown)
            fill = fill.darker (0.15f);
        else if (isMouseOverButton)
            fill = fill.brighter (0.08f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, Palette::controlRadius);
        g.setColour (getToggleState() ? Palette::accent : Palette::line);
        g.drawRoundedRectangle (bounds, Palette::controlRadius, getToggleState() ? 2.0f : 1.0f);
        g.setColour (getToggleState() ? Palette::text : Palette::dimText);
        g.setFont (juce::Font (juce::FontOptions (pt (13.0f), juce::Font::bold)));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }
};

/** A name that edits itself on a double click (Enter commits, Esc cancels). */
class NameLabel : public juce::Label
{
public:
    NameLabel()
    {
        setFont (titleFont());
        setEditable (false, true, false);
        setMinimumHorizontalScale (1.0f);
        setColour (juce::Label::textColourId, Palette::text);
        setTooltip (ko ("더블클릭해서 이름 바꾸기"));
    }

    std::function<void (const juce::String&)> onRenamed;

    void textWasEdited() override
    {
        if (onRenamed)
            onRenamed (getText().trim());
    }

    juce::TextEditor* createEditorComponent() override
    {
        auto* textEditor = juce::Label::createEditorComponent();
        textEditor->setFont (titleFont());
        textEditor->setSelectAllWhenFocused (true);
        return textEditor;
    }
};

/** The plugin chips of a chain: each as wide as its text (within limits), flowing left to right and wrapping.
    The rows they take are counted by the same flow that places them - a count guessed from the number of chips is
    wrong as soon as two wide chips no longer fit side by side. */
namespace ChipFlow
{
    constexpr int height = 32, gap = 6, rowStep = height + gap, minWidth = 110;

    inline juce::Font font() { return juce::Font (juce::FontOptions (pt (13.5f), juce::Font::bold)); }

    /** One chip's width: its text plus 'padding', at least minWidth, at most 'available' (when that is wider). */
    inline int width (const juce::String& text, int padding, int available)
    {
        const int wanted = padding + juce::roundToInt (juce::GlyphArrangement::getStringWidth (font(), text));
        return juce::jlimit (minWidth, juce::jmax (minWidth, available), wanted);
    }

    /** Places the chips in 'area' when 'apply' is set; returns the number of rows they take (at least one). */
    template <typename Chips>
    int layout (const Chips& chips, juce::Rectangle<int> area, int padding, bool apply)
    {
        int x = area.getX(), y = area.getY(), rows = 1;

        for (const auto& chip : chips)
        {
            const int w = width (chip->getButtonText(), padding, area.getWidth());

            if (x + w > area.getRight() && x > area.getX())
            {
                x = area.getX();
                y += rowStep;
                ++rows;
            }

            if (apply)
                chip->setBounds (x, y, w, height);

            x += w + gap;
        }

        return rows;
    }
}

/** A button that captures a key press for a hotkey: click, press the key (Esc cancels). */
class HotkeyButton : public juce::TextButton
{
public:
    HotkeyButton() { setWantsKeyboardFocus (false); }

    std::function<void (const juce::String& description)> onHotkeyChanged;
    /** A reason to refuse the key, or an empty string. */
    std::function<juce::String (const juce::KeyPress&)> validate;

    void setHotkey (const juce::String& description)
    {
        hotkey = description;

        if (! capturing)
            setButtonText (hotkey.isEmpty() ? ko ("없음 (눌러서 지정)") : hotkey);
    }

    const juce::String& getHotkey() const noexcept { return hotkey; }

    void clicked() override
    {
        capturing = true;
        setWantsKeyboardFocus (true);
        grabKeyboardFocus();
        setButtonText (ko ("키를 누르세요... (Esc 취소)"));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (! capturing)
            return false;

        if (key.getModifiers().isAnyModifierKeyDown() && key.getKeyCode() == 0)
            return true;   // a lone modifier: keep waiting for the key

        finishCapture();

        if (key.isKeyCode (juce::KeyPress::escapeKey))
            return true;

        if (validate)
        {
            if (const auto reason = validate (key); reason.isNotEmpty())
            {
                setButtonText (reason);
                juce::Component::SafePointer<HotkeyButton> safeThis (this);
                juce::Timer::callAfterDelay (2500, [safeThis] { if (safeThis != nullptr) safeThis->setHotkey (safeThis->hotkey); });
                return true;
            }
        }

        if (onHotkeyChanged)
            onHotkeyChanged (key.getTextDescription());

        return true;
    }

    void focusLost (FocusChangeType) override
    {
        if (capturing)
            finishCapture();
    }

private:
    void finishCapture()
    {
        capturing = false;
        setWantsKeyboardFocus (false);
        setHotkey (hotkey);
    }

    juce::String hotkey;
    bool capturing = false;
};

/** A device input / output picker filled from the device's channel names. */
inline void fillChannelCombo (juce::ComboBox& combo, const juce::StringArray& names, bool pairs, int fallbackCount)
{
    combo.clear (juce::dontSendNotification);
    const int count = names.isEmpty() ? fallbackCount : names.size();

    if (pairs)
    {
        for (int i = 0; i + 1 < count; i += 2)
        {
            const auto label = names.isEmpty() ? juce::String (i + 1) + "-" + juce::String (i + 2)
                                               : juce::String (i + 1) + "-" + juce::String (i + 2) + "  " + names[i];
            combo.addItem (label, i + 1);   // item id = first + 1
        }
    }
    else
    {
        for (int i = 0; i < count; ++i)
            combo.addItem (names.isEmpty() ? juce::String (i + 1) : juce::String (i + 1) + "  " + names[i], i + 1);
    }
}

} // namespace gocue::livemix
