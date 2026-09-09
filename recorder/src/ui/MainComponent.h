#pragma once
#include "RecordView.h"
#include "TimelineView.h"
#include "AudioSettingsPanel.h"
#include "CameraSettingsPanel.h"
#include "app/RecorderSession.h"

namespace gocue::recorder
{
class MainComponent : public juce::Component, private juce::Timer, private juce::KeyListener
{
public:
    MainComponent(RecorderDocument&, RecorderSettings&);
    ~MainComponent() override;
    void resized() override;
    void showError(const juce::String&);
    void openProject(const juce::File&);
    void requestClose(std::function<void()>);
    void createProject(const juce::String&, const juce::File&, unsigned fps);
    void startDemo(int iterations, const juce::File& devices, int asioDevice, const juce::File& report);
private:
    struct FileResult
    {
        juce::Result result = juce::Result::ok(); bool opening = false;
        juce::File file; RecorderProject loaded; CheckpointInfo info; RecorderDocument::Snapshot written;
    };
    struct Demo
    {
        enum class Step { opening, configuring, ready, recording, waitingPlayback, showingPlayback, finished } step = Step::opening;
        int iterations = 20, iteration = 0, asioIndex = -1, returnCode = 1;
        juce::File devices, report, folder;
        std::int64_t phaseQpc = qpcNow(), stopQpc = 0, clipQpc = 0;
        juce::Array<juce::var> rows;
        std::future<juce::Result> writing;
    };
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    void refresh(); void setTimeline(bool); void recordClicked(); void stopClicked(); void latestClicked();
    void projectMenu(); void newProjectDialog(); void chooseOpen(); void saveProject(); void saveTo(const juce::File&);
    void beforeSwitch(std::function<void()>); void showSettings(); void persistSettings(); void continueClose();
    void demoTick(); void finishDemo(const juce::String&, const juce::String&);
    RecorderDocument& document;
    RecorderSettings& settings;
    RecordView recordView;
    TimelineView timelineView;
    RecorderSession session; // joins native host users before RecordView destruction
    std::unique_ptr<juce::DocumentWindow> settingsWindow, projectWindow;
    AudioSettingsPanel* audioPanel = nullptr;
    juce::Label* settingsError = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<Demo> demo;
    std::future<FileResult> fileWork;
    std::future<juce::Result> settingsWork;
    std::future<juce::int64> spaceWork;
    std::function<void()> afterSave, closeAction;
    bool timeline = false, refreshPending = true, settingsPending = false;
    juce::int64 remainingBytes = -1;
    std::uint32_t lastUi = 0, lastSpace = 0;
    std::int64_t lastStopButtonQpc = 0;
    juce::String banner;
    juce::TooltipWindow tooltips {this};
};
}
