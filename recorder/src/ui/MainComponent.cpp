#include "MainComponent.h"
#include "app/RecorderUpdater.h"
#include "storage/RecoveryScanner.h"
#include "storage/IoHealth.h"
#include "ExportDialog.h"
#include "ShortcutSettingsPanel.h"
#include <chrono>

namespace gocue::recorder
{
namespace { template<class T> bool completed(std::future<T>& f) { return f.valid() && f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; } }
MainComponent::MainComponent(RecorderDocument& d, RecorderSettings& s, TakeController::VideoFactory factory) : document(d), settings(s), timelineView(d), session(d, std::move(factory)), audioImporter(d)
{
    addAndMakeVisible(recordView); addChildComponent(timelineView); setWantsKeyboardFocus(true); addKeyListener(this);
    juce::Desktop::getInstance().addFocusChangeListener(this);
    recordView.projectButton.onClick = [this] { projectMenu(); };
    recordView.recordTab.onClick = [this] { setTimeline(false); }; recordView.timelineTab.onClick = [this] { setTimeline(true); };
    recordView.settingsButton.onClick = [this] { showSettings(); };
    recordView.importButton.onClick = [this] { importAudio(); };
    addChildComponent(audioImporter); audioImporter.setCompact(true);
    audioImporter.onStatusChanged = [this](const juce::String& message) { showError(message + importPlacement); };
    audioImporter.onBusyChanged = [this] { audioImporter.setVisible(importBusy()); publishLifecycle(); refreshPending = true; };
    audioImporter.onImported = [this](const MediaAsset& asset, const CachedImportedAudio& cache)
    {
        timelineView.setLoadedPeaks(asset.assetId, ImportedAudioCache::peakSnapshot(cache), 0);
        session.refreshPlaybackPlan(); setTimeline(true);
        const auto& tracks = document.getProject().tracks;
        for (unsigned row = 0; row < tracks.size(); ++row) for (const auto& clip : tracks[row].clips.items())
            if (clip.assetId == asset.assetId)
            {
                document.setSelection({clip.clipId}); timelineView.selectionChanged();
                session.scrub(clip.timelineStartSample, true);
                timelineView.reveal(clip.timelineStartSample); timelineView.revealTrack(row);
            }
    };
    recordView.startButton.onClick = [this] { recordClicked(); }; recordView.stopButton.onClick = [this] { stopClicked(); };
    recordView.latestButton.onClick = [this] { latestClicked(); }; recordView.markerButton.onClick = [this] { session.addMarker(); refreshPending = true; };
    recordView.onArm = [this](unsigned i, bool on)
    { const auto r = session.audioEngine().arm(i, on); if (r.failed()) showError(r.getErrorMessage()); else { auto next = settings.get(); next.microphoneArmed[i] = on; settings.set(next); session.updateMicrophoneSettings(next); persistSettings(); if (cameraPanel) cameraPanel->setSettings(next); } refreshPending = true; };
    recordView.onMonitor = [this, mask = std::uint8_t{0}](unsigned i, bool on) mutable { mask = std::uint8_t(on ? mask | (1u << i) : mask & ~(1u << i)); session.setMonitoring(mask); };
    recordView.onName = [this](unsigned i, const juce::String& name) { auto next = settings.get(); next.microphoneNames[i] = name; settings.set(next); session.updateMicrophoneSettings(next); persistSettings(); };
    auto& t = timelineView.transport; t.play.onClick = [this] { session.play(); }; t.pause.onClick = [this] { session.pause(); }; t.stop.onClick = [this] { session.stopPlayback(); }; t.beginning.onClick = [this] { session.goToStart(); };
    timelineView.onScrub = [this](Sample at, bool released) { session.scrub(at, released); };
    timelineView.onListeningChanged = [this] { session.refreshPlaybackPlan(); };
    timelineView.onGlobalKey = [this](const juce::KeyPress& key, juce::Component* origin) { return keyPressed(key, origin); };
    session.onConfigured = [this](const juce::Result& result, const UserSettings& s)
    {
        auto applied = s; applied.shortcuts = settings.get().shortcuts;
        settings.set(applied); persistSettings(); if (result.failed()) showError(result.getErrorMessage()); else banner.clear();
        if (audioPanel && !pendingConfigure) { audioPanel->setSettings(s); audioPanel->setDeviceInfo(session.deviceInfo()); } // a queued edit keeps the user's latest choices on screen
        if (cameraPanel) cameraPanel->setSettings(s);
        if (settingsError) settingsError->setText(result.wasOk() ? ko("설정을 적용했습니다.") : result.getErrorMessage(), juce::dontSendNotification);
        refreshPending = true;
        if (pendingConfigure)
        {
            const auto next = *pendingConfigure; pendingConfigure.reset();
            const auto queued = audioPanel ? audioPanel->configure(session, next, s) : session.configure(next);
            if (settingsError) settingsError->setText(queued.failed() ? queued.getErrorMessage() : ko("장치를 연결하는 중입니다."), juce::dontSendNotification);
        }
    };
    session.onPeaks = [this](const Id& id, auto peaks, unsigned channel) { timelineView.setPeaks(id, peaks, channel); };
    session.onLoadedPeaks = [this](const Id& id, auto peaks, unsigned channel) { timelineView.setLoadedPeaks(id, std::move(peaks), channel); };
    session.onThumbnails = [this](const Id& id, auto frames) { timelineView.setThumbnails(id, std::move(frames)); };
    document.onChanged = [this] { refreshPending = true; publishLifecycle(); };
    exportDialog = std::make_unique<ExportDialog>(document, session, recordView.exportButton, [this](const juce::String& text) { showError(text); });
    exportDialog->onShortcut = [this](const juce::KeyPress& key, juce::Component* origin) { return routeShortcut(key, origin); };
    aboutButton.setButtonText(ko("앱 정보")); updateButton.setButtonText(ko("업데이트")); retryButton.setButtonText(ko("마무리 재시도"));
    for (auto* button : {&aboutButton, &updateButton, &retryButton}) addAndMakeVisible(button);
    aboutButton.onClick = [] { RecorderUpdater::showAboutDialog(); };
    updateButton.onClick = [this] { checkForUpdates(); };
    retryButton.onClick = [this] { if (closeAction) { closeCommitRequested = false; persistSettings(); continueClose(); } else retryFinalization(); };
    publishLifecycle();
    setSize(1180, 780); refresh(); startTimer(10);
}
MainComponent::~MainComponent()
{
    stopTimer(); document.onChanged = nullptr; removeKeyListener(this);
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    if (shortcutFocus) shortcutFocus->removeKeyListener(this);
    audioImporter.onImported = {}; audioImporter.onStatusChanged = {}; audioImporter.onBusyChanged = {};
    audioImporter.shutdown(); importStarting = false; publishLifecycle();
    session.onConfigured = {}; session.onPeaks = {}; session.onLoadedPeaks = {}; session.onThumbnails = {};
    exportDialog.reset(); // cancels/joins a running export and releases the session's exporting gate before the wait below
    session.requestShutdown();
    if (fileWork.valid()) { const auto r = collectFileWork(); if (r.written) document.checkpointFinished(r.written, r.file, r.result); }
    session.lifecycleState()->end(RecorderLifecycle::recovering);
    // JUCE can enter shutdown directly (automation/OS quit). Keep the owner and
    // HWNDs alive until its collection and durable workers have returned.
    while (!session.readyForShutdownCommit()) { session.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    if (document.isDirty() && document.getFile() != juce::File())
    {
        const auto saved = document.saveCheckpoint(document.getFile());
        if (saved.failed()) { juce::Logger::writeToLog(saved.getErrorMessage()); if (auto* app = juce::JUCEApplication::getInstance()) app->setApplicationReturnValue(1); }
    }
    if (settingsWork.valid()) try { const auto r = settingsWork.get(); if (r.failed()) juce::Logger::writeToLog(r.getErrorMessage()); }
    catch (const std::exception& e) { juce::Logger::writeToLog(juce::String::fromUTF8(e.what())); }
    catch (...) { juce::Logger::writeToLog("Unknown settings completion exception"); }
    session.releaseForShutdown();
    while (!session.shutdownComplete()) { session.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    settingsWindow.reset(); projectWindow.reset();
}
void MainComponent::resized()
{
    recordView.setBounds(getLocalBounds()); timelineView.setBounds(recordView.timelineBounds());
    const auto lastButton = recordView.latestButton.getBounds();
    audioImporter.setBounds(lastButton.getRight() + 8, lastButton.getY(), juce::jmax(0, getWidth() - lastButton.getRight() - 20), lastButton.getHeight());
    auto row = getLocalBounds().removeFromBottom(26).removeFromRight(320);
    updateButton.setBounds(row.removeFromRight(90)); aboutButton.setBounds(row.removeFromRight(90)); retryButton.setBounds(row);
}
void MainComponent::showError(const juce::String& message) { banner = message; refreshPending = true; }
void MainComponent::showUnhandledException(const juce::File& report)
{
    exceptionBanner = report == juce::File() ? ko("예상치 못한 오류가 발생했으며 오류 기록을 저장하지 못했습니다.")
        : ko("예상치 못한 오류가 기록됐습니다: ") + report.getFileName();
    if (session.recording() || session.lifecycleState()->captureBusy()
        || session.takeController().state() == TakeController::State::finalizing)
        exceptionBanner += ko(" 녹화를 정지하고 프로젝트를 저장하세요");
    refreshPending = true;
}
void MainComponent::setTimeline(bool on)
{ timeline = on; session.enterTimeline(on); timelineView.setVisible(on); refresh(); }
bool MainComponent::canImportAudio() const
{
    return !closeAction && !importBusy() && !fileWork.valid() && !session.busy()
        && !document.isRecordingStructureLocked() && !session.lifecycleState()->captureBusy()
        && session.lifecycleState()->acceptsCommands()
        && !(session.lifecycleState()->snapshot() & (RecorderLifecycle::fileWork | RecorderLifecycle::recording
            | RecorderLifecycle::finalizing | RecorderLifecycle::dubbing | RecorderLifecycle::exporting | RecorderLifecycle::recovering))
        && !(settingsWindow && settingsWindow->isVisible()) && !(exportDialog && exportDialog->previewActive());
}
void MainComponent::importAudio(const juce::File& file)
{
    if (!canImportAudio()) { showError(ko("녹화·마무리와 파일 작업이 끝난 뒤 오디오를 불러오세요.")); return; }
    if (document.getFile() == juce::File()) { showError(ko("먼저 프로젝트 > 새 프로젝트에서 저장할 폴더를 선택하세요.")); return; }
    const auto at = timeline ? session.playhead() : document.getProject().activeTimelineEnd();
    importPlacement = (timeline ? ko(" · 요청 시 재생헤드 ") : ko(" · 요청 시 타임라인 끝 "))
        + juce::String(double(at) / document.getProject().Fs, 3) + ko("초에 배치");
    if (!session.lifecycleState()->begin(RecorderLifecycle::fileWork)) return;
    importStarting = true; publishLifecycle();
    session.pause(); // capture the insertion sample first, then pause while the file is prepared
    audioImporter.setImportContext(document.getFile().getParentDirectory(), at);
    audioImporter.setRecordingActive(false);
    try { if (file == juce::File()) audioImporter.chooseFile(); else audioImporter.importFile(file); }
    catch (const std::exception& e) { audioImporter.shutdown(); showError(ko("오디오 불러오기를 시작하지 못했습니다. ") + juce::String::fromUTF8(e.what())); }
    importStarting = false; audioImporter.setVisible(importBusy()); publishLifecycle(); refresh();
}
bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{ return !files.isEmpty(); } // also receive unsupported drops so the user gets a reason
void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    if (files.size() != 1) { showError(ko("오디오 파일은 한 번에 하나씩 끌어다 놓으세요.")); return; }
    const juce::File file(files[0]);
    if (!file.hasFileExtension("wav;wave;mp3;m4a;aac;m4b;mp4;wma"))
    { showError(ko("지원하지 않는 오디오 형식입니다. WAV · MP3 · M4A · AAC 파일을 선택하세요.")); return; }
    importAudio(file);
}
void MainComponent::recordClicked()
{ if (fileWork.valid() || importBusy() || closeAction || (settingsWindow && settingsWindow->isVisible()) || (exportDialog && exportDialog->previewActive())) return; if (exportDialog && !exportDialog->beforeRecording([this] { recordClicked(); })) return; const auto r = session.record(); if (r.failed()) showError(r.getErrorMessage()); else banner.clear(); publishLifecycle(); refreshPending = true; }
void MainComponent::stopClicked()
{
    lastStopButtonQpc = qpcNow(); timelineView.lastClipPaintQpc = 0; timelineView.lastPaintedTake.clear();
    const auto r = session.stopRecording(); if (r.failed()) showError(r.getErrorMessage());
    else { setTimeline(true); timelineView.reveal(session.takeController().placementSample()); }
    refreshPending = true;
}
void MainComponent::latestClicked() { setTimeline(true); session.play(true); timelineView.reveal(session.takeController().placementSample()); refresh(); }
void MainComponent::refresh()
{
    auto ui = mapUiState(document.getProject(), settings.get(), session.takeController().state(), document.isRecordingStructureLocked(), session.configuring() || fileWork.valid() || importBusy(), session.deviceInfo().sampleRate != 0, session.cameraReady(0));
    ui.canRecord = !fileWork.valid() && !importBusy() && session.readyToRecord();
    audioImporter.setRecordingActive(session.recording() || document.isRecordingStructureLocked()
        || (session.lifecycleState()->snapshot() & RecorderLifecycle::finalizing));
    recordView.importButton.setEnabled(canImportAudio());
    recordView.importButton.setTooltip(ko("WAV · MP3 · M4A · AAC / 모노 · 스테레오 · 타임라인: 요청 시 재생헤드, 녹화 화면: 타임라인 끝"));
    const auto& take = session.takeController();
    const auto audioFault = session.configuring() || closeAction ? RecorderAudioEngine::Error::none : session.audioEngine().error();
    using E = RecorderAudioEngine::Error;
    const auto originalFailure = audioFault == E::writeFailed ? recorderFaultText(RecorderFault::storageWrite)
        : audioFault == E::rawOverflow || audioFault == E::pcmOverflow ? recorderFaultText(RecorderFault::audioOverflow)
        : audioFault == E::sampleRateChanged ? recorderFaultText(RecorderFault::audioRateChanged)
        : audioFault == E::asioReset ? recorderFaultText(RecorderFault::audioReset)
        : audioFault != E::none && audioFault != E::cancelled ? recorderFaultText(RecorderFault::audioInput) : juce::String();
    auto message = originalFailure.isNotEmpty() ? originalFailure : document.getError().isNotEmpty() ? recorderFaultText(RecorderFault::save) + " " + document.getError()
        : session.error.isNotEmpty() ? session.error : banner.isNotEmpty() ? banner : session.notice.isNotEmpty() ? session.notice
        : take.warning().isNotEmpty() ? take.warning() : document.getRecoveryMessage();
    const auto delayed = recorderFaultText(RecorderFault::processingDelay);
    if (exceptionBanner.isNotEmpty()) message = exceptionBanner + (message.isNotEmpty() ? " · " + message : juce::String());
    if (session.notice == delayed && !message.contains(delayed)) message = delayed + " · " + message;
    const auto takeStatus = session.takeController().statusText();
    const auto status = importBusy() ? ko("오디오 불러오는 중") : session.takeController().state() == TakeController::State::idle ? (fileWork.valid() ? ko("저장 중") : document.getStatusText()) : takeStatus;
    recordView.update(ui, document.getProject(), settings.get(), status, message, session.elapsed(), remainingBytes, timeline);
    const auto& shortcuts = settings.get().shortcuts;
    recordView.startButton.setTooltip(ko("녹화 시작 · ") + shortcuts[RecorderCommand::recordStart]);
    recordView.stopButton.setTooltip(ko("녹화 정지 · ") + shortcuts[RecorderCommand::recordStop]);
    recordView.markerButton.setTooltip(ko("마커 추가 · ") + shortcuts[RecorderCommand::marker]);
    timelineView.setShortcuts(shortcuts);
    timelineView.transport.play.setTooltip(ko("재생 / 정지 · ") + shortcuts[RecorderCommand::playStop]);
    retryButton.setButtonText(closeAction ? ko("저장 재시도") : ko("마무리 재시도"));
    retryButton.setVisible((message.contains(ko("MP4 마무리 실패")) || take.state() == TakeController::State::partialFailure || (closeAction && closeCommitRequested)) && !session.busy() && !fileWork.valid());
    if (closeAction) { recordView.setEnabled(false); timelineView.setEnabled(false); }
    if (settingsWindow) settingsWindow->getContentComponent()->setEnabled(!session.recording()); // edits made while applying are queued
    if (audioPanel) audioPanel->setBusy(session.configuring());
    for (unsigned i = 0; i < 2; ++i)
    {
        const bool enabled = settings.get().cameraEnabled[i]; bool hasPlayback = false;
        if (session.showingPlayback()) for (const auto& track : document.getProject().tracks) if (track.kind == (i ? TrackKind::cam2 : TrackKind::cam1) && !track.clips.items().empty()) hasPlayback = true;
        recordView.setCamera(i, session.cameraCaption(i), i && !enabled && !hasPlayback ? ko("캠2 사용 안 함 · 설정에서 연결")
            : session.showingPlayback() ? ko("영상 없음") : ko("카메라 연결 준비 전"), session.showingPlayback() ? hasPlayback : session.cameraReady(i));
    }
    if (!session.configuring() && !closeAction) recordView.updateMeters(session.audioEngine().inputPeaks());
    timelineView.setRecordingPreview(ui.live, take.placementSample(), session.elapsed(),
        {{session.cameraReady(0), session.cameraReady(1)}, ui.live ? session.audioEngine().armedMicrophones() : std::vector<unsigned>{}}, settings.get());
    timelineView.refresh(ui.structureLocked, session.recording() ? session.takeController().placementSample() + session.elapsed() : session.playhead(), takeStatus == ko("대기") ? juce::String() : takeStatus);
    timelineView.transport.setState(ui.canTransport, session.playing(), session.playhead(), document.getProject().Fs);
    timelineView.setVisible(timeline); resized(); refreshPending = false;
}
bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component* origin)
{ return routeShortcut(key, origin); }
bool MainComponent::routeShortcut(const juce::KeyPress& key, juce::Component* origin)
{
    if (!ownsShortcutOrigin(origin)) return false;
    // Capture buttons carry the command's accessibility title. Other settings
    // controls (including Reset) still route Stop; focused capture buttons own keys.
    if (auto* panel = origin->findParentComponentOfClass<ShortcutSettingsPanel>())
        for (auto* control = origin; control && control != panel; control = control->getParentComponent())
            for (std::size_t i = 0; i < RecorderShortcuts::count; ++i)
                if (control->getTitle() == RecorderShortcuts::name(RecorderCommand(i))) return false;
    const auto command = shortcutCommand(settings.get().shortcuts, key, origin); if (!command) return false;
    // Stop remains available from every owned window even during settings/close.
    // Other settings-window commands are suspended; export preview owns ASIO output.
    if (*command != RecorderCommand::recordStop
        && (closeAction || !session.lifecycleState()->acceptsCommands() || (settingsWindow && settingsWindow->isVisible()))) return false;
    if (*command != RecorderCommand::recordStop && (importBusy() || (exportDialog && exportDialog->previewActive()))) return false;
    if (heldShortcut == key) return true;
    heldShortcut = key;
    switch (*command)
    {
        case RecorderCommand::recordStart: if (recordView.startButton.isEnabled()) recordClicked(); break;
        case RecorderCommand::recordStop: if (session.recording()) stopClicked(); break;
        case RecorderCommand::playStop:
            if (!session.recording() && !fileWork.valid()) { if (session.playing()) session.stopPlayback(); else { if (!timeline) setTimeline(true); session.play(); } } break;
        case RecorderCommand::split: if (timeline && !fileWork.valid() && timelineView.edits.enabled(TimelineAction::split)) timelineView.invoke(TimelineAction::split); break;
        case RecorderCommand::marker: if (!fileWork.valid()) session.addMarker(); break;
        default: break;
    }
    refreshPending = true; return true;
}
bool MainComponent::keyStateChanged(bool, juce::Component*)
{ if (!heldShortcut.isCurrentlyDown()) heldShortcut = {}; return false; }
void MainComponent::globalFocusChanged(juce::Component* focus)
{
    if (shortcutFocus) shortcutFocus->removeKeyListener(this);
    shortcutFocus = nullptr; heldShortcut = {};
    // Listen before the focused widget consumes keys such as Space/arrow keys.
    // Owned top-level windows need the same listener before their widgets consume input.
    if (focus && focus != this && ownsShortcutOrigin(focus)) { shortcutFocus = focus; focus->addKeyListener(this); }
}
bool MainComponent::ownsShortcutOrigin(const juce::Component* origin) const
{
    if (!origin) return false;
    const auto inWindow = [origin](const auto& window) { return window && (origin == window.get() || window->isParentOf(origin)); };
    return origin == this || isParentOf(origin) || inWindow(settingsWindow) || inWindow(projectWindow)
        || (exportDialog && exportDialog->ownsShortcutOrigin(origin));
}
void MainComponent::persistSettings()
{
    settingsPending = true;
    if (settingsWork.valid()) return;
    settingsPending = false;
    try { if (beforeWorkerStart) beforeWorkerStart("settings"); settingsWork = settings.save(); }
    catch (const std::exception& e) { settingsFailed(juce::String::fromUTF8(e.what())); }
    catch (...) { settingsFailed(ko("알 수 없는 설정 저장 오류")); }
}
void MainComponent::settingsFailed(const juce::String& reason)
{ settingsPending = false; showError(ko("설정을 저장할 수 없습니다. ") + reason); closeCommitRequested = bool(closeAction); publishLifecycle(); }
bool MainComponent::startFileWork(FileResult context, std::function<FileResult()> work)
{
    pendingFile = std::move(context);
    try { if (beforeWorkerStart) beforeWorkerStart("file"); fileWork = std::async(std::launch::async, std::move(work)); publishLifecycle(); return true; }
    catch (const std::exception& e) { showError(ko("파일 작업을 시작할 수 없습니다. ") + juce::String::fromUTF8(e.what())); }
    catch (...) { showError(ko("파일 작업을 시작할 수 없습니다. 알 수 없는 오류")); }
    session.lifecycleState()->end(RecorderLifecycle::recovering);
    pendingFile = {}; afterSave = {}; closeCommitRequested = bool(closeAction); publishLifecycle(); return false;
}
MainComponent::FileResult MainComponent::collectFileWork()
{
    auto result = std::move(pendingFile); pendingFile = {};
    try { return fileWork.get(); }
    catch (const std::exception& e) { result.result = juce::Result::fail(juce::String::fromUTF8(e.what())); }
    catch (...) { result.result = juce::Result::fail(ko("알 수 없는 파일 작업 오류")); }
    result.recovered = true; // a failed worker must not start recovery on incomplete metadata
    return result;
}
void MainComponent::requestClose(std::function<void()> action)
{
    if (closeAction) return;
    closeAction = std::move(action); closeCommitRequested = false;
    persistSettings();
    afterSave = {}; chooser.reset(); recordView.setEnabled(false); timelineView.setEnabled(false);
    audioImporter.cancelImport();
    if (settingsWindow) settingsWindow->setVisible(false); if (projectWindow) projectWindow->setVisible(false);
    session.requestShutdown(); publishLifecycle(); refreshPending = true; continueClose();
}
void MainComponent::continueClose()
{
    if (!closeAction || fileWork.valid() || importBusy() || !session.readyForShutdownCommit() || settingsWork.valid()) return;
    if (closeCommitRequested) return; // a failed save waits for the explicit retry action
    if (document.isDirty() && document.getFile() != juce::File()) { saveProject(); closeCommitRequested = true; return; }
    if (document.isDirty() && document.getFile() == juce::File() && document.getProject().activeTimelineEnd() > 0)
    { showError(recorderFaultText(RecorderFault::save)); return; }
    session.releaseForShutdown(); if (!session.shutdownComplete()) return;
    auto action = std::move(closeAction); closeAction = {}; action();
}
void MainComponent::timerCallback()
{
    if (powerMonitor.poll() && !closeAction) session.resumeFromSleep();
    session.setHosts(recordView.nativeHosts()); session.tick();
    // Updates: a quiet check 20 s after launch and then once a day, only while nothing is recording/exporting.
    if (!demo && !closeAction && RecorderUpdater::isAvailable() && !session.busy() && session.lifecycleState()->canShutdown())
    {
        const auto t = juce::Time::getCurrentTime();
        if ((t - launchedAt).inSeconds() >= 20.0 && (lastQuietCheck == juce::Time() || (t - lastQuietCheck).inHours() >= 24.0))
        { lastQuietCheck = t; RecorderUpdater::checkQuietly(); }
    }
    if (completed(fileWork))
    {
        auto r = collectFileWork();
        if (r.opening && !r.recovered && !closeAction)
        {
            session.lifecycleState()->set(RecorderLifecycle::recovering, true);
            session.lifecycleState()->invalidate();
            banner = ko("저장된 자료를 복구하는 중입니다.");
            startFileWork(r, [r]() mutable
            {
                RecoveryReport recovered; r.result = RecoveryScanner().run(r.file.getParentDirectory(), recovered); r.recovered = true;
                if (r.result.wasOk()) { r.loaded = std::move(recovered.project); r.info = recovered.checkpointInfo; r.info.recoveryMessage = recorderFaultText(RecorderFault::recovery); }
                return r;
            });
            publishLifecycle(); return;
        }
        session.lifecycleState()->end(RecorderLifecycle::recovering);
        if (r.opening && closeAction) { publishLifecycle(); continueClose(); return; }
        if (r.result.wasOk())
        {
            if (r.opening)
            {
                const auto result = document.adopt(std::move(r.loaded), r.file, r.info);
                if (result.failed()) showError(result.getErrorMessage());
                else { timelineView.clearCaches(); session.projectChanged(); if (!demo) session.configure(settings.get()); banner.clear(); setTimeline(false); }
            }
            else { document.checkpointFinished(r.written, r.file, r.result); closeCommitRequested = false; }
            settings.rememberProject(r.file); persistSettings();
            if (afterSave) { auto action = std::move(afterSave); afterSave = {}; action(); }
        }
        else
        {
            if (r.written) document.checkpointFinished(r.written, r.file, r.result); showError(recorderFaultText(RecorderFault::save) + " " + r.result.getErrorMessage()); afterSave = {}; closeCommitRequested = bool(closeAction);
            if (r.opening && !closeAction && !demo) session.configure(settings.get()); // a missing recent project must not leave ASIO/cameras unconnected
        }
        refreshPending = true;
    }
    if (completed(settingsWork))
    {
        try
        {
            const auto r = settingsWork.get(); if (r.failed()) settingsFailed(r.getErrorMessage());
            else if (settingsPending) persistSettings();
        }
        catch (const std::exception& e) { settingsFailed(juce::String::fromUTF8(e.what())); }
        catch (...) { settingsFailed(ko("알 수 없는 설정 저장 오류")); }
    }
    if (completed(spaceWork))
    {
        try { const auto bytes = spaceWork.get(); if (spaceGeneration == session.lifecycleState()->generation()) remainingBytes = bytes; }
        catch (...) { remainingBytes = -1; showError(ko("남은 저장 공간을 확인할 수 없습니다.")); }
    }
    const auto now = juce::Time::getMillisecondCounter();
    if (!heldShortcut.isCurrentlyDown()) heldShortcut = {};
    if (now - lastSpace >= 5000 && !spaceWork.valid() && document.getFile() != juce::File())
    {
        lastSpace = now; spaceGeneration = session.lifecycleState()->generation(); const auto path = document.getFile().getParentDirectory();
        try { spaceWork = std::async(std::launch::async, [path] { return path.getBytesFreeOnVolume(); }); }
        catch (...) { remainingBytes = -1; showError(ko("남은 저장 공간 확인을 시작할 수 없습니다.")); }
    }
    const bool displayDue = !lastUi || now - lastUi >= 33;
    if (displayDue) lastUi = !lastUi ? now : lastUi + (now - lastUi) / 33 * 33;
    if (refreshPending || displayDue) refresh();
    if (demo) demoTick();
    publishLifecycle();
    continueClose();
}
void MainComponent::publishLifecycle()
{
    auto state = session.lifecycleState();
    state->set(RecorderLifecycle::unsaved, document.isDirty());
    state->set(RecorderLifecycle::fileWork, fileWork.valid() || importBusy() || settingsWork.valid() || settingsPending);
}
void MainComponent::updateShutdownBlocked() { showError(recorderFaultText(RecorderFault::updateBusy)); }
void MainComponent::updateShutdownRequested()
{
    publishLifecycle(); if (!session.lifecycleState()->canShutdown()) { updateShutdownBlocked(); return; }
    if (auto* app = juce::JUCEApplication::getInstance()) app->systemRequestedQuit();
}
void MainComponent::checkForUpdates()
{
    publishLifecycle(); if (!session.lifecycleState()->canShutdown()) { updateShutdownBlocked(); return; }
    if (!RecorderUpdater::isAvailable()) { showError(ko("공개 업데이트 설정이 준비되지 않았습니다.")); return; }
    RecorderUpdater::checkForUpdatesWithUI();
}
void MainComponent::retryFinalization()
{
    if (session.busy() || fileWork.valid() || importBusy() || document.getFile() == juce::File() || closeAction) return;
    session.stopPlayback(); session.lifecycleState()->invalidate();
    const auto path = document.getFile();
    FileResult context; context.opening = true; context.file = path;
    startFileWork(context, [context] { return context; });
    publishLifecycle();
}
}
