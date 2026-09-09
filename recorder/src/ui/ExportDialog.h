#pragma once
#include "RecorderLookAndFeel.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include "app/RecorderSession.h"
#include "export/ExportController.h"

namespace gocue::recorder
{
// Owned immediately after RecorderSession by MainComponent, so all workers and
// ASIO preview clients detach before the session/document can be destroyed.
class ExportDialog : public juce::Component, private juce::Timer
{
public:
    ExportDialog(RecorderDocument&, RecorderSession&, juce::Button& exportButton,
                 std::function<void(const juce::String&)> notice);
    ~ExportDialog() override;
    void show();
    void resized() override;
    bool beforeRecording(std::function<void()> resume);
private:
    struct Window;
    struct Preview;
    void timerCallback() override;
    void refreshSources();
    void refreshSelection();
    void chooseFolder();
    void startExport(bool retry);
    void startPreview();
    void stopPreview();
    std::optional<SampleRange> selectedRange() const;
    ExportController::Request request() const;
    AudioSourceMask selectedAudio(const ExportJob&, bool reference) const;
    void closeWindow();
    RecorderDocument& document;
    RecorderSession& session;
    juce::Button& exportButton;
    std::function<void(const juce::String&)> notice;
    ExportController controller;
    std::unique_ptr<Window> window;
    std::unique_ptr<juce::FileChooser> chooser;
    std::function<void()> pendingRecording;
    bool recordingReserved = false, finalMode = false, restoreTimeline = false, updating = false, previewAttached = false;
    std::vector<AudioSourceMask> imports, microphones;
    std::shared_ptr<ExportControl> previewControl;
    ExportActivity previewActivity;
    std::future<std::shared_ptr<Preview>> previewWork;
    std::shared_ptr<Preview> preview;
    Id displayedProject;
    Id pendingRecordingProject;
    Sample displayedRevision = -1;
    double progressValue = 0;
    juce::TextButton materialsTab{ko("소재 뽑기")}, finalTab{ko("최종본 뽑기")};
    juce::Label rangeLabel, commonRange, folderLabel, listLabel, videoLabel, audioLabel, referenceLabel, sourceStatus, progressLabel;
    juce::ComboBox rangeChoice, videoChoice, audioChoice, micChoice, importChoice, referenceChoice;
    juce::TextEditor rangeStart, rangeEnd, folder;
    juce::ToggleButton includeImports{ko("불러온 오디오도 WAV로 포함")};
    juce::TextButton browse{ko("찾아보기")}, listen{ko("선택 소스 미리 듣기")}, exportNow{ko("내보내기")},
        cancel{ko("취소")}, retry{ko("재시도")}, openFolder{ko("완료 폴더 열기")};
    juce::ProgressBar progressBar{progressValue};
};
}
