#pragma once

#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace gocue::AutoLevelDialog
{

/** How far the cues' loudness match has got: audio cues with a file, those measured and matched, those still waiting
    to be measured. */
struct MatchStatus
{
    int audioCues = 0, matched = 0, waiting = 0;
};

/** The same content is usable without a native window; the meter and status readers can be supplied by a UI test. */
class Content final : public juce::Component, private juce::Timer, private ProjectDocument::Listener
{
public:
    Content (ProjectDocument&, AudioEngine&, std::function<double()> gainReader = {}, std::function<MatchStatus()> statusReader = {});
    /** The text beside the on/off pair: 큐 분석 중 3/12 while measuring, then 큐 12개 맞춤; nothing when off or when
        there is no audio cue (nothing to match). */
    static juce::String statusText (bool on, const MatchStatus& status);
    ~Content() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void refreshMeter();

private:
    void documentStateChanged() override;
    void timerCallback() override { refreshMeter(); }
    void commitTarget();
    void enable (bool);
    ProjectDocument& document;
    AudioEngine& engine;
    std::function<double()> readGain;
    std::function<MatchStatus()> readStatus;
    juce::TextButton offButton, onButton;
    juce::Label targetLabel, unitsLabel, correctionLabel, gainLabel, statusLabel;
    juce::TextEditor targetEditor;
    juce::Rectangle<int> meterBounds;
    double shownGain = 0.0;
};

void show (ProjectDocument&, AudioEngine&, juce::Component* centreAround, std::function<MatchStatus()> statusReader = {});
void closeIfOpen();

} // namespace gocue::AutoLevelDialog
