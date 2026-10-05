#pragma once

#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace gocue::AutoLevelDialog
{

/** The same content is usable without a native window; the meter reader can be supplied by a UI test. */
class Content final : public juce::Component, private juce::Timer, private ProjectDocument::Listener
{
public:
    Content (ProjectDocument&, AudioEngine&, std::function<double()> gainReader = {});
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
    juce::TextButton offButton, onButton;
    juce::Label targetLabel, unitsLabel, correctionLabel, gainLabel;
    juce::TextEditor targetEditor;
    juce::Rectangle<int> meterBounds;
    double shownGain = 0.0;
};

void show (ProjectDocument&, AudioEngine&, juce::Component* centreAround);
void closeIfOpen();

} // namespace gocue::AutoLevelDialog
