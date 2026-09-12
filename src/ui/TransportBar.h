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
    /** The current project's patch name and output count. */
    std::function<juce::String (const Cue&)> describePatch;
    void setContextText (const juce::String& text);
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
    /** "항상 오디션": accent colour and tooltip, while the visible label remains GO. */
    void setAuditionMode (bool auditioning);

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void styleButton (juce::TextButton& button, juce::Colour colour);
    void updateGoLook();
    void updateStandbyCue (int index, const Cue* cue);

    juce::ApplicationCommandManager& commands;

    /** The big GO button: fills its area and draws its text large (a plain TextButton keeps a small font). */
    struct GoButton : public juce::TextButton
    {
        using juce::TextButton::TextButton;

        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
        bool locked = false;
    };

    struct TransportButton : public juce::TextButton
    {
        enum class Icon { pause, fade, stop };
        explicit TransportButton (Icon i) : icon (i) {}
        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
        Icon icon;
        juce::String key, detail;
    };

    /** Keeps the complete existing metadata text, painting its labels and values separately. */
    struct MetaLabel : public juce::Label
    {
        void paint (juce::Graphics&) override;
    };

    struct GearButton : public juce::Button
    {
        GearButton() : juce::Button ("panicSettings") {}

        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    };

    GoButton goButton { "GO" };
    GearButton panicSettingsButton;
    double panicSeconds = 1.0;
    TransportButton pauseButton { TransportButton::Icon::pause }, fadeOutButton { TransportButton::Icon::fade }, panicButton { TransportButton::Icon::stop };
    juce::Label standbyTitle, cueNumber, cueName, cueFile, playingLabel, statusLabel, contextLabel;
    MetaLabel cueMeta;
    juce::Rectangle<int> nextCard;
    bool goLocked = false;
    bool goFlashing = false;
    bool auditionMode = false;
};

} // namespace gocue
