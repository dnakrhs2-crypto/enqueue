#pragma once
#include "AudioImportPanel.h"
#include "record/DubbingController.h"

namespace gocue::recorder
{
class DubbingPanel final : public juce::Component, private juce::Timer
{
public:
    DubbingPanel(RecorderDocument&, DubbingController&);
    ~DubbingPanel() override;
    void setContext(DubbingController::Config); // host supplies devices, directory and button-time playhead
    void setPlayhead(Sample);
    void refresh();
    void resized() override;
    std::function<void()> onTakeChanged;
private:
    void timerCallback() override;
    RecorderDocument& document; DubbingController& controller;
    AudioImportPanel importer;
    DubbingController::Config config;
    juce::ComboBox audioTracks;
    std::vector<Id> trackIds;
    juce::Label target, status;
    juce::ToggleButton microphone{juce::String::fromUTF8("마이크도 녹음")};
    juce::TextButton record{juce::String::fromUTF8("영상 녹화 + 재생")}, again{juce::String::fromUTF8("같은 구간 다시 녹화")}, stopButton{juce::String::fromUTF8("정지")};
    bool autoStart = false;
    DubbingController::State previous = DubbingController::State::idle;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DubbingPanel)
};
}
