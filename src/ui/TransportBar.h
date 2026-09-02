#pragma once

#include "model/Cue.h"

#include "ui/UiUtils.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace gocue
{

/** Top strip: the standby cue's details, the big GO button and the pause / fade / panic buttons. */
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    explicit TransportBar (juce::ApplicationCommandManager& commands);

    /** index is 0-based; cue may be null when nothing is selected. */
    void setStandbyCue (int index, const Cue* cue);
    /** Describes a fade cue's target ("→ 1 Intro"); set by the app, which can look cues up. */
    std::function<juce::String (const Cue& fadeCue)> describeFadeTarget;
    /** Group cues: "mode · N children · length" for the standby display. */
    std::function<juce::String (const Cue& groupCue)> describeGroup;
    /** The gear next to the panic button: open the fade time menu at this screen position. */
    std::function<void (juce::Point<int> screenPosition)> onPanicSettings;
    /** Shows the panic fade time on the button. */
    void setPanicSeconds (double seconds);
    void setPlayingCount (int numPlaying, int numPaused);
    /** Shows a transient message (errors in red) for a few seconds. */
    void showStatus (const juce::String& message, bool isError);
    /** Red border on GO while double-GO protection refuses GOs. */
    void setGoLocked (bool locked);
    /** Brief red flash: a GO was received but refused. */
    void flashGoRejected();
    /** "항상 오디션": the GO button turns blue and says so. */
    void setAuditionMode (bool auditioning);

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void styleButton (juce::TextButton& button, juce::Colour colour);
    void updateGoLook();

    juce::ApplicationCommandManager& commands;

    /** The big GO button: fills its area and draws its text large (a plain TextButton keeps a small font). */
    struct GoButton : public juce::TextButton
    {
        using juce::TextButton::TextButton;

        void paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
        {
            auto colour = findColour (juce::TextButton::buttonColourId);

            if (isButtonDown)
                colour = colour.darker (0.25f);
            else if (isMouseOver)
                colour = colour.brighter (0.12f);

            const auto bounds = getLocalBounds().toFloat();
            g.setGradientFill (Palette::buttonGradient (colour, bounds));
            g.fillRoundedRectangle (bounds, 8.0f);
            g.setColour (colour.darker (0.4f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);
            g.setColour (findColour (juce::TextButton::textColourOffId));
            const auto label = getButtonText();
            const float size = juce::jmin ((float) getHeight() * 0.55f, label.length() > 3 ? 30.0f : 64.0f);
            g.setFont (juce::Font (juce::FontOptions (size, juce::Font::bold)));
            g.drawText (label, getLocalBounds(), juce::Justification::centred, false);
        }
    };

    /** The gear button: a ring with teeth, drawn like the other buttons. */
    struct GearButton : public juce::Button
    {
        GearButton() : juce::Button ("panicSettings") {}

        void paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
        {
            auto colour = Palette::button;

            if (isButtonDown)
                colour = colour.darker (0.2f);
            else if (isMouseOver)
                colour = colour.brighter (0.08f);

            const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
            g.setGradientFill (Palette::buttonGradient (colour, bounds));
            g.fillRoundedRectangle (bounds, Palette::cornerRadius);
            g.setColour (colour.darker (0.4f));
            g.drawRoundedRectangle (bounds, Palette::cornerRadius, 1.0f);

            const auto c = bounds.getCentre();
            const float r = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.26f;
            g.setColour (Palette::text);
            g.drawEllipse (c.x - r, c.y - r, 2.0f * r, 2.0f * r, 2.2f);
            g.fillEllipse (c.x - r * 0.3f, c.y - r * 0.3f, r * 0.6f, r * 0.6f);

            for (int i = 0; i < 8; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                g.drawLine (c.x + std::cos (a) * r, c.y + std::sin (a) * r,
                            c.x + std::cos (a) * (r + 3.5f), c.y + std::sin (a) * (r + 3.5f), 2.4f);
            }
        }
    };

    GoButton goButton { "GO" };
    GearButton panicSettingsButton;
    double panicSeconds = 1.0;
    juce::TextButton pauseButton, fadeOutButton, panicButton;
    juce::Label standbyTitle, cueNumber, cueName, cueFile, cueMeta, playingLabel, statusLabel;
    bool goLocked = false;
    bool goFlashing = false;
    bool auditionMode = false;
};

} // namespace gocue
