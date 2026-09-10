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
AudioImportPanel::~AudioImportPanel() { onBusyChanged = {}; shutdown(); }
void AudioImportPanel::shutdown()
{ stopTimer(); chooser.reset(); worker.reset(); if (onBusyChanged) onBusyChanged(); }
void AudioImportPanel::report(const juce::String& text)
{ status.setText(text, juce::dontSendNotification); if (onStatusChanged) onStatusChanged(text); }
void AudioImportPanel::setImportContext(juce::File folder, Sample sample)
{ directory = std::move(folder); playhead = sample; }
void AudioImportPanel::setRecordingActive(bool active)
{
    recording = active; if (worker) worker->setRecordingActive(active);
    importButton.setEnabled(!active && !isBusy());
}
void AudioImportPanel::chooseFile()
{
    if (recording || isBusy()) return;
    if (chooseFileForTesting) { const auto file = chooseFileForTesting(); if (file != juce::File()) importFile(file); return; }
    chooser = std::make_unique<juce::FileChooser>("오디오 파일 불러오기", juce::File(), "*.wav;*.wave;*.mp3;*.m4a;*.aac;*.m4b;*.mp4;*.wma");
    cancelButton.setEnabled(true);
    report(juce::String::fromUTF8("불러올 오디오 파일을 선택하세요."));
    if (onBusyChanged) onBusyChanged();
    const juce::Component::SafePointer<AudioImportPanel> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& choice)
        {
            if (!safe || !safe->chooser) return;
            const auto file = choice.getResult(); safe->chooser.reset();
            if (file != juce::File()) safe->importFile(file);
            else { safe->report(juce::String::fromUTF8("오디오 파일 선택을 취소했습니다.")); safe->cancelButton.setEnabled(false); }
            if (safe->onBusyChanged) safe->onBusyChanged();
        });
}
void AudioImportPanel::importFile(const juce::File& file)
{
    if (worker) return;
    if (recording) { report(juce::String::fromUTF8("녹화와 마무리가 끝난 뒤 오디오를 불러오세요.")); return; }
    if (directory == juce::File())
    { report(juce::String::fromUTF8("오디오를 저장할 프로젝트 폴더를 먼저 선택하세요.")); return; }
    AudioImportRequest request; request.source = file; request.projectDirectory = directory;
    request.projectId = document.getProject().projectId; request.projectFs = document.getProject().Fs; request.playhead = playhead;
    try { worker = std::make_unique<ImportedAudioCache::Worker>(request, recording); }
    catch (const std::exception& e) { report(juce::String::fromUTF8("오디오 불러오기를 시작하지 못했습니다. ") + juce::String::fromUTF8(e.what())); if (onBusyChanged) onBusyChanged(); return; }
    importButton.setEnabled(false); cancelButton.setEnabled(true); progressBar.setVisible(true); progress = 0;
    report(juce::String::fromUTF8("오디오를 불러오는 중입니다.")); startTimerHz(20);
    if (onBusyChanged) onBusyChanged();
}
void AudioImportPanel::cancelImport()
{
    if (chooser) { chooser.reset(); cancelButton.setEnabled(false); report(juce::String::fromUTF8("오디오 파일 선택을 취소했습니다.")); if (onBusyChanged) onBusyChanged(); }
    if (worker) { worker->cancel(); cancelButton.setEnabled(false); report(juce::String::fromUTF8("불러오기를 취소하는 중입니다.")); }
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
    if (!worker->control.cancelled.load()) report(juce::String::fromUTF8(message) + " " + juce::String(int(progress * 100)) + "%");
    if (!worker->finished()) return;
    if (recording && !worker->control.cancelled.load()) return; // never publish structure while capture/finalization owns it
    std::unique_ptr<PreparedAudioImport> prepared; CachedImportedAudio cache;
    const bool cancelled = worker->control.cancelled.load();
    auto result = juce::Result::ok();
    try
    {
        result = worker->takeResult(prepared, cache);
        if (result.wasOk()) result = commitImportedAudio(document, *prepared, worker->control);
    }
    catch (const std::exception& e) { result = juce::Result::fail(juce::String::fromUTF8(e.what())); }
    catch (...) { result = juce::Result::fail(juce::String::fromUTF8("알 수 없는 오디오 불러오기 오류")); }
    worker.reset(); stopTimer(); importButton.setEnabled(!recording); cancelButton.setEnabled(false); progressBar.setVisible(false);
    auto messageText = cancelled ? juce::String::fromUTF8("오디오 불러오기를 취소했습니다.")
        : juce::String::fromUTF8("오디오 불러오기 실패: ") + result.getErrorMessage();
    if (result.wasOk())
    {
        messageText = juce::String::fromUTF8("오디오를 독립 트랙에 추가했습니다. ");
        if (prepared->info().sampleRate != cache.sampleRate)
            messageText += juce::String(prepared->info().sampleRate) + " → " + juce::String(cache.sampleRate) + juce::String::fromUTF8(" Hz로 재생용 변환됨 · 원본 보존");
        else messageText += juce::String(cache.sampleRate) + juce::String::fromUTF8(" Hz · 샘플레이트 변환 없음");
    }
    report(messageText);
    if (result.wasOk() && onImported) onImported(prepared->asset(), cache);
    if (onBusyChanged) onBusyChanged();
}
void AudioImportPanel::resized()
{
    auto area = getLocalBounds().reduced(8); auto buttons = area.removeFromTop(30);
    importButton.setVisible(!compact); status.setVisible(!compact);
    if (compact)
    {
        area = getLocalBounds().reduced(2); cancelButton.setBounds(area.removeFromRight(72));
        progressBar.setBounds(area.reduced(8, 6)); return;
    }
    importButton.setBounds(buttons.removeFromLeft(190)); buttons.removeFromLeft(8); cancelButton.setBounds(buttons.removeFromLeft(64));
    area.removeFromTop(6); progressBar.setBounds(area.removeFromTop(18)); status.setBounds(area.removeFromTop(32));
}
}
