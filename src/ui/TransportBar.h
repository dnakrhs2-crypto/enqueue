#pragma once

#include "model/Cue.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue
{

/** Top strip: the standby cue's details, the big GO button and the stop / fade buttons. */
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    explicit TransportBar (juce::ApplicationCommandManager& commands);

    /** index is 0-based; cue may be null when nothing is selected. */
    void setStandbyCue (int index, const Cue* cue);
    void setPlayingCount (int numPlaying);
    /** Shows a transient message (errors in red) for a few seconds. */
    void showStatus (const juce::String& message, bool isError);

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void styleButton (juce::TextButton& button, juce::Colour colour);

    juce::ApplicationCommandManager& commands;

    juce::TextButton goButton { "GO" };
    juce::TextButton stopButton, fadeOutButton, stopAllButton;
    juce::Label standbyTitle, cueNumber, cueName, cueFile, cueMeta, playingLabel, statusLabel;
};

} // namespace gocue
