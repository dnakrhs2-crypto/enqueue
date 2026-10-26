#pragma once

#include "LiveMixSettings.h"
#include "Widgets.h"
#include "recording/Recorder.h"

namespace gocue::livemix
{
juce::String recordingElapsedText (double seconds);

// The same TextButton shape as LUFS, with the recording dot (or stop square).
class RecordingButton : public juce::TextButton
{
public:
    void paintButton (juce::Graphics&, bool over, bool down) override;
    bool red = false, stop = false, large = false;
};

class RecordingPanel : public juce::Component
{
public:
    struct Actions
    {
        std::function<bool (const recording::Options&)> start;
        std::function<void()> stop, hide;
    };
    RecordingPanel (LiveMixSettings&, Actions);
    void setSnapshot (const recording::Snapshot&);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void refresh();
    void saveOptions();
    void chooseDirectory();
    LiveMixSettings& settings;
    Actions actions;
    recording::Options options;
    recording::Snapshot snapshot;
    bool startPending = false, stopPending = false, startFailed = false;
    std::array<Chip, 2> formats;
    std::array<Chip, 3> bitrates;
    juce::Label formatCaption, bitrateCaption, directoryCaption, directory, status, elapsed, filename, error;
    juce::TextButton change;
    RecordingButton record;
    juce::Rectangle<int> cardBounds;
    std::unique_ptr<juce::FileChooser> chooser;
};

class RecordingWindow : public juce::DocumentWindow, private juce::Timer
{
public:
    RecordingWindow (MixEngine&, LiveMixSettings&);
    ~RecordingWindow() override;
    void open();
    void closeButtonPressed() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    MixEngine& engine;
    RecordingPanel* content = nullptr;
    juce::TooltipWindow tooltips { this, 700 };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordingWindow)
};
}
