#include "ExportDialog.h"
#include "UiState.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace gocue::recorder
{
struct ExportDialog::Window final : juce::DocumentWindow
{
    ExportDialog& owner;
    explicit Window(ExportDialog& d) : DocumentWindow(ko("내보내기"), Palette::background, closeButton), owner(d)
    { setUsingNativeTitleBar(true); setContentNonOwned(&owner, true); setResizable(true, false); setResizeLimits(700, 640, 1400, 1000); centreWithSize(800, 700); }
    void closeButtonPressed() override { owner.closeWindow(); }
    bool keyPressed(const juce::KeyPress& key) override
    { return owner.onShortcut && owner.onShortcut(key, juce::Component::getCurrentlyFocusedComponent()); }
};
struct ExportDialog::Preview final : IAudioOutputClient
{
    std::shared_ptr<ExportJob> job;
    TimelineAudioRenderer renderer;
    Sample cursor, end;
    std::atomic<bool> ended{false}, underrun{false};
    Preview(std::shared_ptr<ExportJob> j, std::vector<AudioSourceBinding> bindings, AudioSourceMask mask, unsigned block)
        : job(std::move(j)), renderer(job->snapshot.Fs, block), cursor(job->range.startSample), end(cursor + job->range.sampleCount)
    {
        auto plan = std::make_shared<CompiledRenderPlan>(*job->audioPlan->timeline);
        plan->timelineEnd = end; // preserve mappings/fades, prefetch only this range; absent spans are silence
        renderer.setPlan(std::move(plan), std::move(bindings), std::move(mask)); renderer.prepare(cursor, 1);
    }
    void processOutput(const BlockStamp& stamp, float* left, float* right) noexcept override
    {
        std::fill_n(left, stamp.numSamples, 0.0f); std::fill_n(right, stamp.numSamples, 0.0f);
        if (ended.load(std::memory_order_relaxed) || underrun.load(std::memory_order_relaxed)) return;
        const auto validEnd = (std::min)(end, job->range.requested.start + job->range.requested.length);
        const auto count = static_cast<unsigned>((std::min)(Sample(stamp.numSamples), (std::max)(Sample{0}, validEnd - cursor)));
        if (count && !renderer.queue().consume(cursor, 1, left, right, count)) { underrun.store(true); return; }
        cursor += stamp.numSamples;
        if (cursor >= end) ended.store(true);
    }
};
ExportDialog::ExportDialog(RecorderDocument& d, RecorderSession& s, juce::Button& button,
                           std::function<void(const juce::String&)> n)
    : document(d), session(s), exportButton(button), notice(std::move(n))
{
    for (juce::Component* c : std::initializer_list<juce::Component*>{&materialsTab, &finalTab, &rangeLabel, &commonRange,
        &folderLabel, &listLabel, &videoLabel, &audioLabel, &referenceLabel, &sourceStatus, &progressLabel,
        &rangeChoice, &videoChoice, &audioChoice, &micChoice, &importChoice, &referenceChoice,
        &rangeStart, &rangeEnd, &folder, &includeImports, &browse, &listen, &exportNow, &cancel, &retry, &openFolder, &progressBar}) addAndMakeVisible(c);
    materialsTab.setClickingTogglesState(true); finalTab.setClickingTogglesState(true);
    materialsTab.setRadioGroupId(23); finalTab.setRadioGroupId(23); materialsTab.setToggleState(true, juce::dontSendNotification);
    materialsTab.onClick = [this] { stopPreview(); finalMode = false; refreshSelection(); };
    finalTab.onClick = [this] { stopPreview(); finalMode = true; refreshSelection(); };
    rangeLabel.setText(ko("범위"), juce::dontSendNotification); folderLabel.setText(ko("출력 폴더"), juce::dontSendNotification);
    rangeChoice.addItem(ko("전체"), 1); rangeChoice.addItem(ko("선택 구간"), 2); rangeChoice.setSelectedId(1);
    rangeStart.setText("0"); rangeEnd.setText("0");
    rangeStart.setInputRestrictions(18, "0123456789."); rangeEnd.setInputRestrictions(18, "0123456789.");
    rangeStart.setTooltip(ko("선택 구간 시작 (초)")); rangeEnd.setTooltip(ko("선택 구간 끝 (초)"));
    for (auto* field : {&rangeStart, &rangeEnd}) { field->setFont(recorderMonoFont(14)); field->setJustification(juce::Justification::centredRight); }
    commonRange.setFont(recorderMonoFont(14));
    videoLabel.setText(ko("영상 소스: 캠1 / 캠2"), juce::dontSendNotification);
    audioLabel.setText(ko("오디오 소스"), juce::dontSendNotification);
    referenceLabel.setText(ko("참조 오디오"), juce::dontSendNotification);
    audioChoice.addItem(ko("마이크 전체 믹스"), 1); audioChoice.addItem(ko("개별 마이크"), 2); audioChoice.addItem(ko("완성 오디오 파일"), 3); audioChoice.setSelectedId(1);
    for (auto* combo : {&rangeChoice, &videoChoice, &audioChoice, &micChoice, &importChoice, &referenceChoice})
        combo->onChange = [this] { if (!updating) { stopPreview(); refreshSelection(); } };
    rangeStart.onTextChange = rangeEnd.onTextChange = [this] { if (!updating) { stopPreview(); refreshSelection(); } };
    includeImports.onClick = [this] { refreshSelection(); };
    browse.onClick = [this] { chooseFolder(); }; exportNow.onClick = [this] { startExport(false); };
    retry.onClick = [this] { startExport(true); }; cancel.onClick = [this] { controller.cancel(); stopPreview(); };
    listen.onClick = [this] { if (preview || previewWork.valid()) stopPreview(); else startPreview(); };
    openFolder.onClick = [this] { const auto path = controller.status().outputDirectory; if (path.isDirectory()) path.startAsProcess(); };
    retry.setTooltip(ko("실패하거나 취소된 작업의 기존 소스·범위·편집 revision으로 다시 내보냅니다. 설정을 바꾸려면 내보내기를 누르세요."));
    exportButton.setEnabled(true); exportButton.setTooltip(ko("소재 뽑기 / 최종본 뽑기"));
    exportButton.onClick = [this] { show(); };
    sourceStatus.setJustificationType(juce::Justification::topLeft); listLabel.setJustificationType(juce::Justification::topLeft);
    sourceStatus.setMinimumHorizontalScale(1); listLabel.setMinimumHorizontalScale(1);
    setSize(800, 700); startTimer(50);
}
ExportDialog::~ExportDialog()
{
    stopTimer(); exportButton.onClick = nullptr; pendingRecording = {}; restoreTimeline = false;
    stopPreview();
    if (previewWork.valid()) try { previewWork.get(); } catch (...) {}
    controller.cancel(); controller.wait();
    releaseExportGate();
    if (window) window->clearContentComponent(); window.reset();
}
bool ExportDialog::holdExportGate()
{
    if (exportGateHeld) return true;
    exportGateHeld = session.beginExclusive(RecorderLifecycle::exporting, [this] { controller.cancel(); }).wasOk();
    return exportGateHeld;
}
void ExportDialog::releaseExportGate()
{ if (exportGateHeld) { exportGateHeld = false; session.endExclusive(RecorderLifecycle::exporting); } }
void ExportDialog::show()
{
    refreshSources(); if (!window) window = std::make_unique<Window>(*this);
    window->setVisible(true); styleRecorderWindow(*window); window->toFront(true); refreshSelection();
}
bool ExportDialog::ownsShortcutOrigin(const juce::Component* origin) const
{ return origin && window && (origin == window.get() || window->isParentOf(origin)); }
void ExportDialog::closeWindow()
{ stopPreview(); if (window) window->setVisible(false); }
void ExportDialog::refreshSources()
{
    if (controller.busy() || preview || previewWork.valid()) return;
    const auto& p = document.getProject();
    if (displayedProject == p.projectId && displayedRevision == p.editRevision) return;
    updating = true; const bool changedProject = displayedProject != p.projectId;
    displayedProject = p.projectId; displayedRevision = p.editRevision;
    videoChoice.clear(juce::dontSendNotification); micChoice.clear(juce::dontSendNotification); importChoice.clear(juce::dontSendNotification); referenceChoice.clear(juce::dontSendNotification);
    imports.clear(); microphones.clear(); juce::StringArray listed;
    for (auto cam : MaterialExporter::cameras(p))
    {
        const auto name = cam == TrackKind::cam1 ? ko("캠1") : ko("캠2");
        videoChoice.addItem(name, cam == TrackKind::cam1 ? 1 : 2); listed.add(name);
    }
    referenceChoice.addItem(ko("마이크 전체 믹스"), 1);
    for (const auto& t : p.tracks)
    {
        if (t.kind == TrackKind::mic)
        {
            const auto name = ko("마이크 ") + juce::String(t.microphoneIndex + 1) + (t.name.isNotEmpty() ? ko(" · ") + t.name : juce::String());
            microphones.push_back({AudioSourceMask::Kind::microphone, t.trackId}); micChoice.addItem(name, int(microphones.size())); listed.add(name);
        }
        if (t.kind == TrackKind::importAudio) for (const auto& c : t.clips.items()) if (p.isActive(c))
        {
            const auto found = std::find_if(imports.begin(), imports.end(), [&](const auto& m) { return m.assetId == c.assetId && m.trackId == t.trackId; });
            if (found != imports.end()) continue;
            const auto* a = p.media->findAsset(c.assetId); if (!a) continue;
            const auto name = t.name.isNotEmpty() ? t.name : document.getFile().getParentDirectory().getChildFile(a->relativePath).getFileName();
            imports.push_back({AudioSourceMask::Kind::completedAudio, t.trackId, c.assetId});
            importChoice.addItem(name, int(imports.size())); referenceChoice.addItem(ko("완성 오디오 파일: ") + name, int(imports.size()) + 1);
        }
    }
    videoChoice.setSelectedItemIndex(0, juce::dontSendNotification); micChoice.setSelectedItemIndex(0, juce::dontSendNotification); importChoice.setSelectedItemIndex(0, juce::dontSendNotification);
    bool dub = false; for (const auto& take : p.media->takes) dub |= take.mode == TakeMode::dub;
    referenceChoice.setItemEnabled(1, !dub);
    for (int i = 0; i < int(imports.size()); ++i) referenceChoice.setItemEnabled(i + 2, dub);
    referenceChoice.setSelectedId(dub ? (imports.size() == 1 ? 2 : 0) : 1, juce::dontSendNotification);
    referenceChoice.setTextWhenNothingSelected(ko("완성 오디오 파일을 선택하세요"));
    if (dub && imports.size() == 1) audioChoice.setSelectedId(3, juce::dontSendNotification);
    listLabel.setText(ko("존재하는 캠/마이크 목록\n") + listed.joinIntoString("   ·   "), juce::dontSendNotification);
    includeImports.setEnabled(!imports.empty());
    Sample first = 0, end = p.activeTimelineEnd(); bool selected = false;
    for (const auto& id : document.getSelection()) if (const auto* clip = p.findClip(id))
    { if (!selected) { first = clip->timelineStartSample; end = clip->timelineEnd(); selected = true; } else { first = (std::min)(first, clip->timelineStartSample); end = (std::max)(end, clip->timelineEnd()); } }
    rangeStart.setText(juce::String(double(first) / p.Fs, 9), false); rangeEnd.setText(juce::String(double(end) / p.Fs, 9), false);
    if (changedProject || folder.getText().isEmpty()) folder.setText(document.getFile().getParentDirectory().getChildFile("exports").getFullPathName(), false);
    updating = false; refreshSelection();
}
std::optional<SampleRange> ExportDialog::selectedRange() const
{
    if (rangeChoice.getSelectedId() == 1) return {};
    const auto a = rangeStart.getText().getDoubleValue(), b = rangeEnd.getText().getDoubleValue();
    const auto Fs = document.getProject().Fs;
    exportRequire(std::isfinite(a) && std::isfinite(b) && a >= 0 && b > a && b * Fs < 9.0e18, "Enter a valid selected range in seconds");
    const Sample start = std::llround(a * Fs), end = std::llround(b * Fs);
    exportRequire(end > start, "Selected range is shorter than one sample"); return SampleRange{start, end - start};
}
AudioSourceMask ExportDialog::selectedAudio(const ExportJob& job, bool reference) const
{
    const auto completed = [&](AudioSourceMask mask)
    {
        const auto* asset = job.snapshot.media->findAsset(mask.assetId);
        exportRequire(asset && asset->originalFormat.channels >= 1 && asset->originalFormat.channels <= 2,
            "선택한 완성 오디오 파일은 지원하지 않는 채널 수입니다. 모노 또는 스테레오 파일을 선택하세요.");
        return mask;
    };
    if (reference)
    {
        const int index = referenceChoice.getSelectedId();
        exportRequire(index > 0, "참조에 사용할 완성 오디오 파일을 선택하세요.");
        return index == 1 ? AudioSourceMask{AudioSourceMask::Kind::microphoneMix} : completed(imports.at(std::size_t(index - 2)));
    }
    if (audioChoice.getSelectedId() == 1) return {AudioSourceMask::Kind::microphoneMix};
    if (audioChoice.getSelectedId() == 2)
    {
        exportRequire(micChoice.getSelectedId() > 0, "개별 마이크를 하나 선택하세요.");
        return microphones.at(std::size_t(micChoice.getSelectedId() - 1));
    }
    exportRequire(importChoice.getSelectedId() > 0, "완성 오디오 파일을 하나 선택하세요.");
    return completed(imports.at(std::size_t(importChoice.getSelectedId() - 1)));
}
ExportController::Request ExportDialog::request() const
{
    exportRequire(document.getFile() != juce::File(), "프로젝트를 저장한 뒤 내보내세요.");
    exportRequire(!document.isRecordingStructureLocked() && !session.busy(), "녹화와 준비·마무리가 끝난 뒤 내보내기를 시작하세요.");
    exportRequire(juce::File::isAbsolutePath(folder.getText()), "출력 폴더의 전체 경로를 선택하세요.");
    ExportController::Request r; r.range = selectedRange(); r.mode = finalMode ? ExportController::Mode::finalVideo : ExportController::Mode::materials;
    const auto stem = juce::File::createLegalFileName(document.getProject().name).substring(0, 100);
    r.destination = juce::File(folder.getText()).getChildFile(stem + (finalMode ? "-final" : "-materials"));
    ExportJob job = ExportJob::fromDocument(document, ExportController::resolveDestination(r.destination), r.range);
    if (finalMode)
    {
        exportRequire(videoChoice.getSelectedId() > 0, "영상 소스로 카메라를 하나 선택하세요.");
        r.finalSource = {videoChoice.getSelectedId() == 1 ? TrackKind::cam1 : TrackKind::cam2, selectedAudio(job, false)};
        FinalVideoExporter::validateSelection(job, r.finalSource);
    }
    else
    {
        r.materials.includeImports = includeImports.getToggleState();
        if (!MaterialExporter::cameras(job.snapshot).empty()) r.materials.referenceAudio = selectedAudio(job, true);
        MaterialExporter::outputs(job, r.materials);
    }
    return r;
}
void ExportDialog::refreshSelection()
{
    if (updating) return;
    const bool busy = controller.busy() || session.busy() || document.isRecordingStructureLocked();
    for (auto* c : std::initializer_list<juce::Component*>{&materialsTab, &finalTab, &rangeChoice, &rangeStart, &rangeEnd,
        &folder, &browse, &videoChoice, &audioChoice, &micChoice, &importChoice, &referenceChoice, &includeImports}) c->setEnabled(!busy);
    rangeStart.setEnabled(!busy && rangeChoice.getSelectedId() == 2); rangeEnd.setEnabled(rangeStart.isEnabled());
    includeImports.setVisible(!finalMode && !imports.empty()); listLabel.setVisible(!finalMode);
    referenceChoice.setVisible(!finalMode); referenceLabel.setVisible(!finalMode);
    videoLabel.setVisible(finalMode); videoChoice.setVisible(finalMode); audioLabel.setVisible(finalMode); audioChoice.setVisible(finalMode);
    micChoice.setVisible(finalMode && audioChoice.getSelectedId() == 2); importChoice.setVisible(finalMode && audioChoice.getSelectedId() == 3);
    exportNow.setEnabled(false); listen.setEnabled(false);
    if (!busy) try
    {
        const auto r = request(); ExportJob job = ExportJob::fromDocument(document, ExportController::resolveDestination(r.destination), r.range);
        commonRange.setText(ko("예상 공통 범위: ") + juce::String(job.range.startSeconds(job.snapshot.Fs, job.snapshot.fps), 6) + " – "
            + juce::String(job.range.endSeconds(job.snapshot.Fs, job.snapshot.fps), 6) + ko("초 → 모든 파일 0 시작 · ")
            + juce::String(job.range.frameCount) + " frames / " + juce::String(job.range.sampleCount) + " samples", juce::dontSendNotification);
        const auto mask = finalMode ? r.finalSource.audio : r.materials.referenceAudio.value_or(AudioSourceMask{AudioSourceMask::Kind::microphoneMix});
        juce::String text;
        for (const auto& track : job.snapshot.tracks) if (track.trackId == mask.trackId && track.mute)
            text = mask.kind == AudioSourceMask::Kind::microphone ? ko("선택한 마이크가 음소거되어 있습니다") : ko("선택한 완성 오디오 파일이 음소거되어 있습니다");
        if (mask.kind == AudioSourceMask::Kind::microphoneMix && TimelineExporter::assetIds(job, mask).isEmpty()) text = ko("마이크 믹스에 포함할 트랙이 0개입니다. 무음으로 내보냅니다.");
        if (!finalMode) text += (text.isNotEmpty() ? "\n" : "") + ko("소재 WAV는 mute/solo와 무관한 PCM24 · 프로젝트 Fs · 모노입니다. Stereo import는 L/R 두 WAV로 보존합니다.");
        sourceStatus.setText(text, juce::dontSendNotification); exportNow.setEnabled(true); listen.setEnabled(session.deviceInfo().sampleRate == job.snapshot.Fs);
    }
    catch (const std::exception& e) { commonRange.setText(ko("예상 공통 범위: 확인 필요"), juce::dontSendNotification); sourceStatus.setText(juce::String::fromUTF8(e.what()), juce::dontSendNotification); }
    if (preview || previewWork.valid()) { listen.setEnabled(true); listen.setButtonText(ko("미리 듣기 정지")); }
    else listen.setButtonText(ko("선택 소스 미리 듣기"));
    resized();
}
void ExportDialog::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser>(ko("출력 폴더"), juce::File(folder.getText()));
    juce::Component::SafePointer<ExportDialog> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe](const juce::FileChooser& chosen) { if (safe && chosen.getResult() != juce::File()) { safe->folder.setText(chosen.getResult().getFullPathName()); safe->refreshSelection(); } });
}
void ExportDialog::startExport(bool again)
{
    stopPreview(); if (previewWork.valid()) { sourceStatus.setText(ko("미리 듣기가 정지된 뒤 내보내기를 시작하세요."), juce::dontSendNotification); return; }
    try
    {
        juce::Result result = juce::Result::ok();
        if (again)
        {
            const auto state = controller.status();
            exportRequire(state.projectId == document.getProject().projectId && !session.busy() && !document.isRecordingStructureLocked(), "같은 프로젝트에서 녹화가 끝난 뒤 재시도하세요.");
            exportRequire(juce::File::isAbsolutePath(folder.getText()), "출력 폴더의 전체 경로를 선택하세요.");
            exportRequire(holdExportGate(), "녹화·재생이 끝난 뒤 내보내기를 시작하세요.");
            result = controller.retry(juce::File(folder.getText()).getChildFile(state.outputDirectory.getFileName()));
        }
        else { const auto r = request(); exportRequire(holdExportGate(), "녹화·재생이 끝난 뒤 내보내기를 시작하세요."); result = controller.start(document, r); }
        if (result.failed()) { releaseExportGate(); sourceStatus.setText(result.getErrorMessage(), juce::dontSendNotification); }
    }
    catch (const std::exception& e) { if (!controller.busy()) releaseExportGate(); sourceStatus.setText(juce::String::fromUTF8(e.what()), juce::dontSendNotification); }
    refreshSelection();
}
void ExportDialog::startPreview()
{
    try
    {
        const auto r = request(); auto job = std::make_shared<ExportJob>(ExportJob::fromDocument(document, {}, r.range));
        const auto mask = selectedAudio(*job, !finalMode); const auto device = session.deviceInfo();
        exportRequire(device.sampleRate == job->snapshot.Fs && device.bufferFrames > 0, "Connect the project sample-rate ASIO output for preview");
        restoreTimeline = session.showingPlayback(); session.enterTimeline(false);
        previewControl = std::make_shared<ExportControl>(previewActivity);
        previewWork = std::async(std::launch::async, [job, mask, device, c = previewControl]
        { auto bindings = TimelineExporter::openSources(*job, mask, *c); c->checkpoint(); return std::make_shared<Preview>(job, std::move(bindings), mask, device.bufferFrames); });
        if (window) window->enterModalState(false);
    }
    catch (const std::exception& e) { sourceStatus.setText(juce::String::fromUTF8(e.what()), juce::dontSendNotification); stopPreview(); }
    catch (...) { sourceStatus.setText(ko("알 수 없는 미리 듣기 준비 오류"), juce::dontSendNotification); stopPreview(); }
    refreshSelection();
}
void ExportDialog::stopPreview()
{
    if (previewControl) previewControl->cancelled.store(true);
    if (preview) { if (previewAttached) session.audioEngine().setPlaybackClient(nullptr); previewAttached = false; preview.reset(); }
    if (window && window->isCurrentlyModal()) window->exitModalState(0);
    if (restoreTimeline && !previewWork.valid()) { restoreTimeline = false; session.enterTimeline(true); }
}
bool ExportDialog::beforeRecording(std::function<void()> resume)
{
    stopPreview();
    if (previewWork.valid() || !controller.beforeRecording())
    {
        pendingRecording = std::move(resume); pendingRecordingProject = document.getProject().projectId;
        if (notice) notice(ko("녹화를 시작하기 위해 내보내기를 멈추는 중입니다. 작업이 정지되면 녹화를 시작합니다."));
        return false;
    }
    recordingReserved = true; return true;
}
void ExportDialog::timerCallback()
{
    if (previewWork.valid() && previewWork.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
    {
        try { auto prepared = previewWork.get(); if (!previewControl->cancelled.load()) preview = std::move(prepared); }
        catch (const ExportCancelled&) {}
        catch (const std::exception& e) { sourceStatus.setText(juce::String::fromUTF8(e.what()), juce::dontSendNotification); stopPreview(); }
        catch (...) { sourceStatus.setText(ko("알 수 없는 미리 듣기 준비 오류"), juce::dontSendNotification); stopPreview(); }
        if (!preview) stopPreview();
    }
    if (preview)
    {
        if (preview->ended.load() || preview->underrun.load() || preview->renderer.status().failed())
        {
            if (preview->underrun.load()) sourceStatus.setText(ko("미리 듣기 오디오 준비가 늦어 정지했습니다. 다시 재생하세요."), juce::dontSendNotification);
            stopPreview();
        }
        else if (!previewAttached && preview->renderer.ready()) { session.audioEngine().setPlaybackClient(preview.get()); previewAttached = true; }
    }
    if (pendingRecording && !controller.busy() && !previewWork.valid())
    {
        auto action = std::move(pendingRecording); pendingRecording = {};
        if (pendingRecordingProject == document.getProject().projectId) action();
        else if (notice) notice(ko("프로젝트가 바뀌어 대기 중인 녹화 요청을 취소했습니다."));
    }
    if (recordingReserved && !session.recording() && !document.isRecordingStructureLocked())
    { recordingReserved = false; controller.endRecording(); }
    if (exportGateHeld && !controller.busy()) releaseExportGate(); // completed, cancelled or failed
    const auto state = controller.status(); progressValue = state.progress.fraction;
    using S = ExportController::State;
    juce::String text;
    if (state.state == S::running) text = ko("내보내는 중 · ") + juce::String(progressValue * 100, 1) + "% · "
        + (state.progress.etaSeconds ? ko("예상 남은 시간 ") + juce::String(*state.progress.etaSeconds, 0) + ko("초 (실효 처리량 추정)") : ko("남은 시간 계산 중"));
    else if (state.state == S::cancelling) text = ko("취소 중 · 현재 작업이 정지될 때까지 기다리는 중입니다.");
    else if (state.state == S::completed) text = ko("완료 · ") + state.outputDirectory.getFullPathName();
    else if (state.state == S::cancelled) text = ko("취소했습니다. 재시도할 수 있습니다.");
    else if (state.state == S::failed) text = ko("내보내기에 실패했습니다. 출력 폴더와 남은 공간을 확인한 뒤 재시도하세요. ") + state.error;
    if (state.state == S::failed || state.state == S::cancelled)
        text += ko("\n재시도: ") + (state.mode == ExportController::Mode::materials ? ko("소재 뽑기") : ko("최종본 뽑기")) + " · revision " + juce::String(state.editRevision) + ko("의 기존 설정");
    if (state.notice.isNotEmpty()) text += "\n" + state.notice;
    progressLabel.setText(text, juce::dontSendNotification);
    cancel.setEnabled(controller.busy() || preview || previewWork.valid()); openFolder.setEnabled(state.state == S::completed);
    retry.setEnabled(!controller.busy() && !session.busy() && !document.isRecordingStructureLocked() && state.projectId == document.getProject().projectId
        && finalMode == (state.mode == ExportController::Mode::finalVideo) && (state.state == S::failed || state.state == S::cancelled));
    if (window && window->isVisible())
    {
        refreshSources();
        const bool wantEnabled = !controller.busy() && !session.busy() && !document.isRecordingStructureLocked();
        if (materialsTab.isEnabled() != wantEnabled) refreshSelection();
    }
}
void ExportDialog::resized()
{
    auto area = getLocalBounds().reduced(22);
    auto row = area.removeFromTop(38); materialsTab.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(2)); finalTab.setBounds(row.reduced(2)); area.removeFromTop(14);
    row = area.removeFromTop(34); rangeLabel.setBounds(row.removeFromLeft(52)); rangeChoice.setBounds(row.removeFromLeft(150).reduced(2));
    rangeStart.setBounds(row.removeFromLeft(170).reduced(2)); rangeEnd.setBounds(row.removeFromLeft(170).reduced(2));
    commonRange.setBounds(area.removeFromTop(52));
    row = area.removeFromTop(36); folderLabel.setBounds(row.removeFromLeft(82)); browse.setBounds(row.removeFromRight(95).reduced(2)); folder.setBounds(row.reduced(2)); area.removeFromTop(12);
    auto sources = area.removeFromTop(170);
    if (!finalMode)
    {
        listLabel.setBounds(sources.removeFromTop(65)); includeImports.setBounds(sources.removeFromTop(34));
        row = sources.removeFromTop(36); referenceLabel.setBounds(row.removeFromLeft(100)); referenceChoice.setBounds(row.reduced(2));
    }
    else
    {
        row = sources.removeFromTop(40); videoLabel.setBounds(row.removeFromLeft(200)); videoChoice.setBounds(row.reduced(2));
        row = sources.removeFromTop(40); audioLabel.setBounds(row.removeFromLeft(200)); audioChoice.setBounds(row.reduced(2));
        row = sources.removeFromTop(40); row.removeFromLeft(200); micChoice.setBounds(row.reduced(2)); importChoice.setBounds(row.reduced(2));
    }
    row = area.removeFromBottom(38); exportNow.setBounds(row.removeFromLeft(130).reduced(2)); cancel.setBounds(row.removeFromLeft(90).reduced(2));
    retry.setBounds(row.removeFromLeft(90).reduced(2)); openFolder.setBounds(row.removeFromRight(170).reduced(2));
    progressLabel.setBounds(area.removeFromBottom(74)); progressBar.setBounds(area.removeFromBottom(22)); area.removeFromBottom(10);
    listen.setBounds(area.removeFromBottom(34).removeFromLeft(205)); sourceStatus.setBounds(area.reduced(0, 6));
}
}
