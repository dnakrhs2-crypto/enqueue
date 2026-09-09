#include "AudioImportPanel.h"

namespace gocue::recorder
{
AudioImportPanel::AudioImportPanel(RecorderDocument& d) : document(d)
{
    addAndMakeVisible(importButton); addAndMakeVisible(cancelButton);
    addAndMakeVisible(status); addAndMakeVisible(progressBar);
    importButton.onClick = [this] { chooseFile(); }; cancelButton.onClick = [this] { cancelImport(); };
    cancelButton.setEnabled(false); progressBar.setVisible(false);
    status.setText("WAV · MP3 · M4A · AAC / mono · stereo", juce::dontSendNotification);
}
AudioImportPanel::~AudioImportPanel() { stopTimer(); chooser.reset(); worker.reset(); }
void AudioImportPanel::setImportContext(juce::File folder, Sample sample)
{ directory = std::move(folder); playhead = sample; }
void AudioImportPanel::setRecordingActive(bool active)
{ recording = active; if (worker) worker->setRecordingActive(active); }
void AudioImportPanel::chooseFile()
{
    if (worker || chooser) return;
    chooser = std::make_unique<juce::FileChooser>("오디오 파일 불러오기", juce::File(), "*.wav;*.wave;*.mp3;*.m4a;*.aac;*.m4b;*.mp4;*.wma");
    const juce::Component::SafePointer<AudioImportPanel> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& choice)
        {
            if (!safe) return;
            const auto file = choice.getResult(); safe->chooser.reset();
            if (file != juce::File()) safe->importFile(file);
        });
}
void AudioImportPanel::importFile(const juce::File& file)
{
    if (worker) return;
    if (directory == juce::File())
    { status.setText("오디오를 저장할 프로젝트 폴더를 먼저 선택하세요.", juce::dontSendNotification); return; }
    AudioImportRequest request; request.source = file; request.projectDirectory = directory;
    request.projectId = document.getProject().projectId; request.projectFs = document.getProject().Fs; request.playhead = playhead;
    worker = std::make_unique<ImportedAudioCache::Worker>(request, recording);
    importButton.setEnabled(false); cancelButton.setEnabled(true); progressBar.setVisible(true); progress = 0;
    status.setText("오디오를 불러오는 중입니다.", juce::dontSendNotification); startTimerHz(20);
}
void AudioImportPanel::cancelImport()
{
    if (worker) { worker->cancel(); cancelButton.setEnabled(false); status.setText("불러오기를 취소하는 중입니다.", juce::dontSendNotification); }
}
void AudioImportPanel::timerCallback()
{
    if (!worker) return;
    progress = worker->control.progress.load();
    using Stage = AudioImportControl::Stage;
    const auto stage = worker->control.stage.load();
    const char* message = stage == Stage::pausedForRecording ? "녹화가 끝나면 오디오 불러오기를 계속합니다."
        : stage == Stage::copying ? "원본 오디오를 복사하는 중입니다."
        : stage == Stage::verifying ? "오디오 형식과 길이를 확인하는 중입니다."
        : stage == Stage::cache ? "오디오 재생과 파형을 준비하는 중입니다." : "오디오를 불러오는 중입니다.";
    if (!worker->control.cancelled.load()) status.setText(juce::String::fromUTF8(message), juce::dontSendNotification);
    if (!worker->finished()) return;
    std::unique_ptr<PreparedAudioImport> prepared; CachedImportedAudio cache;
    auto result = worker->takeResult(prepared, cache);
    if (result.wasOk()) result = commitImportedAudio(document, *prepared, worker->control);
    worker.reset(); stopTimer(); importButton.setEnabled(true); cancelButton.setEnabled(false); progressBar.setVisible(false);
    status.setText(result.wasOk() ? "오디오를 독립 트랙에 추가했습니다." : result.getErrorMessage(), juce::dontSendNotification);
    if (result.wasOk() && onImported) onImported(prepared->asset(), cache);
}
void AudioImportPanel::resized()
{
    auto area = getLocalBounds().reduced(8); auto buttons = area.removeFromTop(30);
    importButton.setBounds(buttons.removeFromLeft(190)); buttons.removeFromLeft(8); cancelButton.setBounds(buttons.removeFromLeft(64));
    area.removeFromTop(6); progressBar.setBounds(area.removeFromTop(18)); status.setBounds(area.removeFromTop(32));
}
}
