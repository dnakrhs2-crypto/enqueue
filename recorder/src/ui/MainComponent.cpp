#include "MainComponent.h"
#include "ExportDialog.h"
#include <chrono>

namespace gocue::recorder
{
namespace { template<class T> bool completed(std::future<T>& f) { return f.valid() && f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; } }
MainComponent::MainComponent(RecorderDocument& d, RecorderSettings& s) : document(d), settings(s), timelineView(d), session(d)
{
    addAndMakeVisible(recordView); addChildComponent(timelineView); setWantsKeyboardFocus(true); addKeyListener(this);
    recordView.projectButton.onClick = [this] { projectMenu(); };
    recordView.recordTab.onClick = [this] { setTimeline(false); }; recordView.timelineTab.onClick = [this] { setTimeline(true); };
    recordView.settingsButton.onClick = [this] { showSettings(); };
    recordView.startButton.onClick = [this] { recordClicked(); }; recordView.stopButton.onClick = [this] { stopClicked(); };
    recordView.latestButton.onClick = [this] { latestClicked(); }; recordView.markerButton.onClick = [this] { session.addMarker(); refreshPending = true; };
    recordView.onArm = [this](unsigned i, bool on)
    { const auto r = session.audioEngine().arm(i, on); if (r.failed()) showError(r.getErrorMessage()); else { auto next = settings.get(); next.microphoneArmed[i] = on; settings.set(next); session.updateMicrophoneSettings(next); persistSettings(); } refreshPending = true; };
    recordView.onMonitor = [this, mask = std::uint8_t{0}](unsigned i, bool on) mutable { mask = std::uint8_t(on ? mask | (1u << i) : mask & ~(1u << i)); session.setMonitoring(mask); };
    recordView.onName = [this](unsigned i, const juce::String& name) { auto next = settings.get(); next.microphoneNames[i] = name; settings.set(next); session.updateMicrophoneSettings(next); persistSettings(); };
    auto& t = timelineView.transport; t.play.onClick = [this] { session.play(); }; t.pause.onClick = [this] { session.pause(); }; t.stop.onClick = [this] { session.stopPlayback(); }; t.beginning.onClick = [this] { session.goToStart(); };
    timelineView.onScrub = [this](Sample at, bool released) { session.scrub(at, released); };
    timelineView.onListeningChanged = [this] { session.refreshPlaybackPlan(); };
    session.onConfigured = [this](const juce::Result& result, const UserSettings& s)
    {
        settings.set(s); persistSettings(); if (result.failed()) showError(result.getErrorMessage());
        if (audioPanel) { audioPanel->setSettings(s); audioPanel->setDeviceInfo(session.deviceInfo()); }
        if (settingsError) settingsError->setText(result.wasOk() ? ko("설정을 적용했습니다.") : result.getErrorMessage(), juce::dontSendNotification);
        refreshPending = true;
    };
    session.onPeaks = [this](const Id& id, auto peaks, unsigned channel) { timelineView.setPeaks(id, peaks, channel); };
    session.onLoadedPeaks = [this](const Id& id, auto peaks, unsigned channel) { timelineView.setLoadedPeaks(id, std::move(peaks), channel); };
    session.onThumbnails = [this](const Id& id, auto frames) { timelineView.setThumbnails(id, std::move(frames)); };
    document.onChanged = [this] { refreshPending = true; };
    exportDialog = std::make_unique<ExportDialog>(document, session, recordView.exportButton, [this](const juce::String& text) { showError(text); });
    setSize(1180, 780); refresh(); startTimer(10);
}
MainComponent::~MainComponent()
{ stopTimer(); document.onChanged = nullptr; removeKeyListener(this); settingsWindow.reset(); projectWindow.reset(); }
void MainComponent::resized() { recordView.setBounds(getLocalBounds()); timelineView.setBounds(recordView.timelineBounds()); }
void MainComponent::showError(const juce::String& message) { banner = message; refreshPending = true; }
void MainComponent::setTimeline(bool on)
{ timeline = on; session.enterTimeline(on); timelineView.setVisible(on); refresh(); }
void MainComponent::recordClicked()
{ if (fileWork.valid()) return; if (exportDialog && !exportDialog->beforeRecording([this] { recordClicked(); })) return; const auto r = session.record(); if (r.failed()) showError(r.getErrorMessage()); else banner.clear(); refreshPending = true; }
void MainComponent::stopClicked()
{
    lastStopButtonQpc = qpcNow(); timelineView.lastClipPaintQpc = 0; timelineView.lastPaintedTake.clear();
    const auto r = session.stopRecording(); if (r.failed()) showError(r.getErrorMessage());
    else { setTimeline(true); timelineView.reveal(session.takeController().placementSample()); }
    refreshPending = true;
}
void MainComponent::latestClicked() { timeline = true; timelineView.setVisible(true); session.play(true); timelineView.reveal(session.takeController().placementSample()); refresh(); }
void MainComponent::refresh()
{
    auto ui = mapUiState(document.getProject(), settings.get(), session.takeController().state(), document.isRecordingStructureLocked(), session.configuring() || fileWork.valid(), session.deviceInfo().sampleRate != 0, session.cameraReady(0));
    ui.canRecord = !fileWork.valid() && session.readyToRecord();
    const auto message = document.getError().isNotEmpty() ? document.getError() : banner.isNotEmpty() ? banner : session.error.isNotEmpty() ? session.error : session.notice;
    const auto takeStatus = session.takeController().statusText();
    const auto status = session.takeController().state() == TakeController::State::idle ? (fileWork.valid() ? ko("저장 중") : document.getStatusText()) : takeStatus;
    recordView.update(ui, document.getProject(), settings.get(), status, message, session.elapsed(), remainingBytes, timeline);
    if (settingsWindow) settingsWindow->getContentComponent()->setEnabled(!session.configuring() && !session.recording());
    for (unsigned i = 0; i < 2; ++i)
    {
        const bool enabled = settings.get().cameraEnabled[i]; bool hasPlayback = false;
        if (session.showingPlayback()) for (const auto& track : document.getProject().tracks) if (track.kind == (i ? TrackKind::cam2 : TrackKind::cam1) && !track.clips.items().empty()) hasPlayback = true;
        recordView.setCamera(i, session.cameraCaption(i), i && !enabled && !hasPlayback ? ko("캠2 사용 안 함 · 설정에서 연결")
            : session.showingPlayback() ? ko("영상 없음") : ko("카메라 연결 준비 전"), session.showingPlayback() ? hasPlayback : session.cameraReady(i));
    }
    if (!session.configuring()) recordView.updateMeters(session.audioEngine().inputPeaks());
    timelineView.refresh(ui.live, session.recording() ? session.takeController().placementSample() + session.elapsed() : session.playhead(), takeStatus == ko("대기") ? juce::String() : takeStatus);
    timelineView.transport.setState(ui.canTransport, session.playing(), session.playhead(), document.getProject().Fs);
    timelineView.setVisible(timeline); resized(); refreshPending = false;
}
bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component* origin)
{
    if (dynamic_cast<juce::TextEditor*>(origin) || (origin && origin->findParentComponentOfClass<juce::TextEditor>())) return false;
    if (key.getKeyCode() == juce::KeyPress::spaceKey && timeline && !session.recording()) { if (session.playing()) session.pause(); else session.play(); return true; }
    if ((key.getTextCharacter() == 'm' || key.getTextCharacter() == 'M') && !fileWork.valid()) { session.addMarker(); return true; }
    return false;
}
void MainComponent::persistSettings()
{ settingsPending = true; if (!settingsWork.valid()) { settingsPending = false; settingsWork = settings.save(); } }
void MainComponent::requestClose(std::function<void()> action)
{ closeAction = std::move(action); if (session.takeController().state() == TakeController::State::recording) stopClicked(); continueClose(); }
void MainComponent::continueClose()
{
    if (!closeAction || fileWork.valid() || session.busy() || settingsWork.valid()) return;
    if (document.isDirty() && document.getFile() != juce::File()) { saveProject(); return; }
    auto action = std::move(closeAction); closeAction = {}; action();
}
void MainComponent::timerCallback()
{
    session.setHosts(recordView.nativeHosts()); session.tick();
    if (completed(fileWork))
    {
        auto r = fileWork.get();
        if (r.result.wasOk())
        {
            if (r.opening)
            {
                const auto result = document.adopt(std::move(r.loaded), r.file, r.info);
                if (result.failed()) showError(result.getErrorMessage());
                else { timelineView.clearCaches(); session.projectChanged(); if (!demo) session.configure(settings.get()); banner.clear(); setTimeline(false); }
            }
            else document.checkpointFinished(r.written, r.file, r.result);
            settings.rememberProject(r.file); persistSettings();
            if (afterSave) { auto action = std::move(afterSave); afterSave = {}; action(); }
        }
        else { if (r.written) document.checkpointFinished(r.written, r.file, r.result); showError(r.result.getErrorMessage()); afterSave = {}; closeAction = {}; }
        refreshPending = true;
    }
    if (completed(settingsWork))
    {
        const auto r = settingsWork.get(); if (r.failed()) { showError(ko("설정을 저장할 수 없습니다. ") + r.getErrorMessage()); closeAction = {}; }
        else if (settingsPending) persistSettings();
    }
    if (completed(spaceWork)) remainingBytes = spaceWork.get();
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastSpace >= 5000 && !spaceWork.valid() && document.getFile() != juce::File())
    { lastSpace = now; const auto path = document.getFile().getParentDirectory(); spaceWork = std::async(std::launch::async, [path] { return path.getBytesFreeOnVolume(); }); }
    if (refreshPending || now - lastUi >= 33) { lastUi = now; refresh(); }
    if (demo) demoTick();
    if (closeAction && session.takeController().state() == TakeController::State::recording) stopClicked();
    continueClose();
}
}
