#pragma once
#include "RecorderLookAndFeel.h"
#include "../app/RecorderDocument.h"
#include "../app/RecorderSettings.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::recorder
{
class MainComponent : public juce::Component, private juce::Timer
{
public:
    MainComponent(RecorderDocument&, RecorderSettings&);
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void showError(const juce::String&);
    void openProject(const juce::File&);
    void requestClose(std::function<void()>);
private:
    struct FileResult
    {
        juce::Result result = juce::Result::ok();
        bool opening = false;
        juce::File file;
        RecorderProject loaded;
        CheckpointInfo info;
        RecorderDocument::Snapshot written;
    };
    class TrackRows : public juce::Component
    {
    public:
        explicit TrackRows(RecorderDocument& d) : document(d) {}
        void paint(juce::Graphics&) override;
        RecorderDocument& document;
    };
    void timerCallback() override;
    void refresh();
    void projectMenu();
    void chooseProject(bool create);
    void saveProject();
    void saveTo(const juce::File&);
    void beforeSwitch(std::function<void()>);
    void continueClose();
    void cameraCard(juce::Graphics&, juce::Rectangle<int>, bool second);
    RecorderDocument& document;
    RecorderSettings& settings;
    juce::TextButton projectButton {ko("프로젝트")}, recordTab {ko("녹화")}, timelineTab {ko("타임라인")};
    juce::TextButton normalButton {ko("일반")}, dubButton {ko("더빙")}, settingsButton {ko("설정")}, exportButton {ko("내보내기")};
    juce::TextButton startButton {ko("녹화 시작")}, markerButton {ko("마커 추가")}, undoButton {ko("실행취소")}, redoButton {ko("다시실행")};
    juce::Label projectName, statusLabel, errorLabel;
    juce::Viewport tracksViewport;
    TrackRows trackRows;
    std::unique_ptr<juce::FileChooser> chooser;
    std::future<FileResult> fileWork;
    std::future<juce::Result> settingsWork;
    std::future<juce::int64> spaceWork;
    std::function<void()> afterSave, closeAction;
    bool timeline = false, dub = false, showSettings = false, closingSettings = false, settingsPending = false;
    juce::int64 remainingBytes = -1;
    int spacePollCountdown = 0;
    juce::Rectangle<int> leftCamera, rightCamera, lowerArea;
    juce::String banner;
    juce::TooltipWindow tooltips {this};
};
}
