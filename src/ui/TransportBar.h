#pragma once

#include "model/Cue.h"

#include <juce_gui_basics/juce_gui_basics.h>

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

    juce::TextButton goButton { "GO" };
    juce::TextButton pauseButton, fadeOutButton, panicButton;
    juce::Label standbyTitle, cueNumber, cueName, cueFile, cueMeta, playingLabel, statusLabel;
    bool goLocked = false;
    bool goFlashing = false;
    bool auditionMode = false;
};

} // namespace gocue
