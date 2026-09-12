#include "RecorderSession.h"
#include "ui/UiState.h"
#include "capture/PreviewRecovery.h"
#include "storage/IoHealth.h"
#include <algorithm>
#include <chrono>
#include <tuple>

namespace gocue::recorder
{
namespace
{
juce::String k(const char* s) { return juce::String::fromUTF8(s); }
void checkResult(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
struct RateMismatch : std::runtime_error { using std::runtime_error::runtime_error; }; // the device stays open; the banner names both rates
bool activeTake(TakeController::State s)
{ using S = TakeController::State; return s == S::preparing || s == S::armed || s == S::recording || s == S::stopping; }
template<class T> bool ready(std::future<T>& f) { return f.valid() && f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }
RenderClip mappingFor(const Clip& c, const MediaAsset& a)
{ RenderClip r; r.clipId = c.clipId; r.trackId = c.trackId; r.assetId = c.assetId; r.sourceIn = c.sourceIn; r.timelineStartSample = c.timelineStartSample; r.lengthSamples = c.lengthSamples; r.mediaGeneration = a.mediaGeneration; r.gaps = a.gaps; return r; }
}
bool adoptDeviceSampleRate(RecorderDocument& document, unsigned deviceFs)
{
    const auto& p = document.getProject();
    // Only media fixes the rate; markers are rescaled by the document so their time is kept. This is the same predicate as
    // configure()'s fixedFs, readyToRecord() and TakeController::prepare(): one notion of "fixed" everywhere.
    if (!deviceFs || !p.media->assets.empty() || p.Fs == deviceFs) return false;
    return document.adoptProvisionalTimebase(deviceFs).wasOk();
}
juce::String rateMismatchText(unsigned projectFs, unsigned deviceFs)
{
    return k("프로젝트는 ") + juce::String(projectFs) + k(" Hz로 고정돼 있는데 오디오 장치가 ") + juce::String(deviceFs) + k(" Hz로 열렸습니다. ASIO 제어판이나 장치 소프트웨어에서 ")
        + juce::String(projectFs) + k(" Hz로 맞추고, 장치를 쓰는 다른 프로그램을 닫은 뒤 설정에서 적용을 누르세요.");
}
bool applyAudioDefaults(UserSettings& s, const RecorderAudioEngine::DeviceInfo& info)
{
    if (s.audioDefaultsApplied || !info.sampleRate) return false;
    const auto before = std::make_tuple(s.physicalInputs, s.output.mono, s.output.left, s.output.right, s.output.monoChannel);
    s.audioDefaultsApplied = true;
    if (s.physicalInputs.empty() && info.physicalInputs > 0) s.physicalInputs = {0};
    if (!s.output.mono && s.output.left < 0 && s.output.right < 0 && info.physicalOutputs > 0)
    {
        if (info.physicalOutputs >= 2) { s.output.left = 0; s.output.right = 1; }
        else { s.output.mono = true; s.output.monoChannel = 0; s.output.left = -1; s.output.right = -1; }
    }
    return before != std::make_tuple(s.physicalInputs, s.output.mono, s.output.left, s.output.right, s.output.monoChannel);
}
struct RecorderSession::LiveCamera
{
    std::shared_ptr<CaptureTelemetry> telemetry;
    std::shared_ptr<VideoSurfacePool> pool;
    std::unique_ptr<MfCameraCapture> capture;
    std::unique_ptr<PreviewPresenter> presenter;
    CameraMode mode;
    std::unique_ptr<MfRuntime> runtime;
    bool failed = false;
    bool displayFailed = false;
    PreviewRecovery previewRecovery;
    ~LiveCamera() { presenter.reset(); capture.reset(); }
};
class RecorderSession::SharedOutput final : public IAudioOutput
{
public:
    explicit SharedOutput(RecorderAudioEngine& a) : audio(a) {}
    ~SharedOutput() override { close(); }
    AudioOutputInfo open(const AudioOutputConfig&) override
    { const auto i = audio.deviceInfo(); return {i.name, i.sampleRate, i.bufferFrames, i.outputLatency, -1, -1}; }
    void start(IAudioOutputClient& client) override { audio.setPlaybackClient(&client); }
    void close() noexcept override { audio.setPlaybackClient(nullptr); }
    std::int64_t latestOutputSample() const noexcept override { return audio.currentSample(); }
    juce::Result status() const override { return audio.deviceInfo().sampleRate ? juce::Result::ok() : juce::Result::fail(k("오디오 장치 연결을 확인하세요.")); }
    void drainTiming() override { audio.pollDeviceEvents(); }
private: RecorderAudioEngine& audio;
};
struct RecorderSession::Playback
{
    VideoPlaybackEngine video;
    TimelineAudioRenderer renderer;
    TimelineTransport transport;
    SharedOutput output;
    Playback(unsigned rate, unsigned block, Sample end, RecorderAudioEngine& audio)
        : renderer(rate, block), transport(rate, qpcFrequency(), renderer.queue(), end), output(audio) {}
    ~Playback() { output.close(); renderer.stopWorker(); video.stop(); }
};
RecorderSession::RecorderSession(RecorderDocument& d, TakeController::VideoFactory factory) : document(d), take(d, audio, std::move(factory)) { lifecycle->bindCaptureBlocker(audio.shutdownBlocker()); }
RecorderSession::~RecorderSession()
{
    lifecycle->blockCommands();
    if (planImportControl) planImportControl->cancelled.store(true);
    if (deviceWork.valid()) deviceWork.wait();
    if (planWork.valid()) planWork.wait();
    if (releaseWork.valid()) releaseWork.wait();
    take.requestShutdown();
    while (!take.shutdownComplete()) { take.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    playback.reset();
    for (auto& cam : cameras) if (cam && cam->capture) cam->capture->requestStop();
    for (auto& cam : cameras) cam.reset(); // stop capture offers before TakeController destruction
}
void RecorderSession::setHosts(std::array<void*, 2> next) { hosts = next; }
bool RecorderSession::configuring() const { return deviceWork.valid(); }
bool RecorderSession::recording() const { return activeTake(take.state()); }
bool RecorderSession::busy() const
{ return configuring() || recording() || take.state() == TakeController::State::finalizing || planWork.valid()
    || (lifecycle->snapshot() & (RecorderLifecycle::exporting | RecorderLifecycle::dubbing | RecorderLifecycle::recovering)) != 0; }
bool RecorderSession::cameraReady(unsigned n) const
{ return !shuttingDown && lifecycle->acceptsCommands() && !configuring() && n < 2 && current.cameraEnabled[n]
    && cameras[n] && !cameras[n]->failed && cameras[n]->capture
    && !cameras[n]->capture->failureDetected() && !cameras[n]->capture->finished(); }
bool RecorderSession::readyToRecord() const
{
    const auto exportStop = exclusiveStops.find(RecorderLifecycle::exporting);
    const bool canPauseExport = !(lifecycle->snapshot() & RecorderLifecycle::exporting) || (exportStop != exclusiveStops.end() && bool(exportStop->second));
    return lifecycle->acceptsCommands() && canPauseExport && !lifecycle->captureBusy() && !(lifecycle->snapshot() & (RecorderLifecycle::dubbing | RecorderLifecycle::recovering | RecorderLifecycle::fileWork))
        && !configuring() && !recording() && take.state() != TakeController::State::finalizing && !planWork.valid()
        && document.getFile() != juce::File() && cameraReady(0) && device.sampleRate
        && audio.clockReady() && validateAudioSettings(current, device, document.getProject()).wasOk()
        && (document.getProject().media->assets.empty() || device.sampleRate == document.getProject().Fs); // a fixed project records only at its own rate
}
juce::String RecorderSession::cameraCaption(unsigned n) const
{
    auto text = k(n ? "캠2" : "캠1");
    if (cameraReady(n)) text += k(" · 1080p ") + juce::String(cameras[n]->mode.fps.value(), cameras[n]->mode.fps.denominator == 1 ? 0 : 2) + k(" 입력");
    return text + k(" / 프로젝트 ") + juce::String(document.getProject().fps.numerator);
}
juce::Result RecorderSession::configure(UserSettings settings)
{
    if (!lifecycle->acceptsCommands()) return juce::Result::fail(recorderFaultText(RecorderFault::updateBusy));
    if (busy()) return juce::Result::fail(k("녹화와 저장이 끝난 뒤 설정을 변경하세요."));
    const auto valid = settings.validate(); if (valid.failed()) return valid;
    if (settings.cameraEnabled[0] && settings.cameraEnabled[1]
        && CameraCatalog::sameDevice(settings.cameraDeviceIds[0].toStdString(), settings.cameraDeviceIds[1].toStdString()))
        return juce::Result::fail(k("같은 카메라를 두 번 선택할 수 없습니다."));
    // No connect step: an unset device means "use the first ASIO driver on this PC".
    if (settings.asioDeviceId.isEmpty()) { const auto names = RecorderAudioEngine::deviceNames(); if (!names.isEmpty()) settings.asioDeviceId = names[0]; }
    // Camera choices that did not change keep their running capture: an audio-only change must not blank the previews.
    bool camerasUnchanged = settings.cameraEnabled == current.cameraEnabled && settings.cameraDeviceIds == current.cameraDeviceIds && settings.cameraModes == current.cameraModes;
    for (unsigned i = 0; i < 2 && camerasUnchanged; ++i) if (settings.cameraEnabled[i] && !cameraReady(i)) camerasUnchanged = false;
    clearPlayback(); lifecycle->invalidate(); lifecycle->set(RecorderLifecycle::configuring, true); error.clear(); notice = k("장치를 연결하는 중입니다.");
    const auto fixedFs = document.getProject().media->assets.empty() ? 0u : document.getProject().Fs;
    // ASIO drivers are COM objects and vendor drivers register them apartment-threaded: they can
    // only be created, opened and closed on the (STA) message thread, never on the MTA camera
    // worker. Negotiate audio here on the caller first. A failure travels with the completion and
    // does not stop the cameras.
    juce::Result audioResult = juce::Result::ok();
    try
    {
        checkResult(audio.closeDevice());
        std::array<int, 8> map; map.fill(-1);
        for (std::size_t i = 0; i < settings.physicalInputs.size(); ++i) map[i] = settings.physicalInputs[i];
        checkResult(audio.setInputMap(map, settings.stereoSlots)); checkResult(audio.setOutputMap(settings.output));
        if (settings.asioDeviceId.isNotEmpty())
        {
            checkResult(audio.openDevice(settings.asioDeviceId, fixedFs ? fixedFs : settings.preferredSampleRate, settings.bufferSize));
            if (applyAudioDefaults(settings, audio.deviceInfo()))
            {
                map.fill(-1); for (std::size_t i = 0; i < settings.physicalInputs.size(); ++i) map[i] = settings.physicalInputs[i];
                checkResult(audio.setInputMap(map, settings.stereoSlots)); checkResult(audio.setOutputMap(settings.output));
            }
            for (unsigned i = 0; i < 8; ++i) checkResult(audio.arm(i, map[i] >= 0 && settings.microphoneArmed[i]));
            settings.preferredSampleRate = audio.deviceInfo().sampleRate;
            settings.bufferSize = int(audio.deviceInfo().bufferFrames);
            if (fixedFs && audio.deviceInfo().sampleRate != fixedFs) // mapped and armed already: meters/monitoring work, record/playback stay blocked
                throw RateMismatch(rateMismatchText(fixedFs, audio.deviceInfo().sampleRate).toStdString());
        }
    }
    catch (const RateMismatch& e) { audioResult = juce::Result::fail(juce::String::fromUTF8(e.what())); }
    catch (const std::exception& e) { audioResult = juce::Result::fail(k("장치 연결을 확인하세요. ") + juce::String::fromUTF8(e.what())); }
    catch (...) { audioResult = juce::Result::fail(k("알 수 없는 장치 연결 오류")); }
    return startDeviceWork([this, settings, audioResult, camerasUnchanged]() mutable
    {
        DeviceResult result; result.settings = settings; result.result = audioResult;
        const auto addFailure = [&result](const juce::String& text)
        { result.result = juce::Result::fail(result.result.failed() ? result.result.getErrorMessage() + " / " + text : text); };
        if (camerasUnchanged) return result;
        try
        {
            ComApartment apartment;
            for (auto& cam : cameras) cam.reset();
            for (unsigned i = 0; i < 2; ++i) if (settings.cameraEnabled[i] && settings.cameraDeviceIds[i].isNotEmpty())
            {
                try
                {
                    auto cam = std::make_unique<LiveCamera>(); cam->runtime = std::make_unique<MfRuntime>();
                    cam->mode = CameraMode::parse(settings.cameraModes[i].toStdString());
                    if (cam->mode.width != 1920 || cam->mode.height != 1080) throw std::runtime_error("1080p 입력 모드를 선택하세요.");
                    cam->telemetry = std::make_shared<CaptureTelemetry>(cam->mode.fps, i ? "cam2" : "cam1");
                    cam->pool = std::make_shared<VideoSurfacePool>(1920, 1080);
                    cam->capture = std::make_unique<MfCameraCapture>(cam->telemetry, *cam->pool,
                        [this, i, stats = cam->telemetry, discontinuities = std::uint64_t{0}, types = std::uint64_t{0}](const VideoSurface& frame) mutable
                    {
                        const auto d = stats->count(LossReason::sourceDiscontinuity), t = stats->count(LossReason::sourceTypeChanged);
                        if (d != discontinuities || t != types) take.cameraDiscontinuity(i, frame.stamp.generation);
                        discontinuities = d; types = t;
                        take.offer(i, frame);
                    });
                    const auto opened = cam->capture->start(settings.cameraDeviceIds[i].toStdString(), cam->mode, cam->mode.subtype == CaptureSubtype::mjpeg);
                    cam->mode = opened.nativeMode; cameras[i] = std::move(cam);
                }
                catch (const std::exception& e)
                {
                    addFailure(k(i ? "캠2 연결을 확인하세요. " : "캠1 연결을 확인하세요. ") + juce::String::fromUTF8(e.what()));
                }
            }
        }
        catch (const std::exception& e) { addFailure(k("장치 연결을 확인하세요. ") + juce::String::fromUTF8(e.what())); }
        return result;
    });
}
juce::Result RecorderSession::startDeviceWork(std::function<DeviceResult()> work)
{
    lifecycle->set(RecorderLifecycle::configuring, true);
    try
    {
        if (beforeWorkerStart) beforeWorkerStart("configure");
        deviceWork = std::async(std::launch::async, std::move(work));
    }
    catch (const std::exception& e) { return configurationFailed(juce::String::fromUTF8(e.what())); }
    catch (...) { return configurationFailed(k("알 수 없는 장치 작업 시작 오류")); }
    return juce::Result::ok();
}
juce::Result RecorderSession::configurationFailed(const juce::String& reason)
{
    lifecycle->end(RecorderLifecycle::configuring); notice.clear();
    // A partially applied ASIO mapping must not become a record-ready configuration.
    const auto closed = audio.closeDevice(); device = audio.deviceInfo();
    error = k("장치 설정을 완료할 수 없습니다. ") + reason;
    if (closed.failed()) error += " / " + closed.getErrorMessage();
    const auto result = juce::Result::fail(error);
    if (onConfigured) onConfigured(result, current);
    return result;
}
void RecorderSession::presentLive()
{
    if (shuttingDown || configuring() || playback) return;
    for (unsigned i = 0; i < 2; ++i) if (cameraReady(i) && hosts[i] && !cameras[i]->presenter && !cameras[i]->displayFailed && !cameras[i]->previewRecovery.recovering())
    {
        try
        {
            cameras[i]->presenter = std::make_unique<PreviewPresenter>(static_cast<HWND>(hosts[i]), *cameras[i]->pool, cameras[i]->telemetry, 1920, 1080);
            cameras[i]->presenter->start();
        }
        catch (const std::exception& e)
        {
            auto& cam = *cameras[i];
            cam.previewRecovery.lost(e.what(), IoHealth::now(), [&] { cam.presenter.reset(); });
            cam.displayFailed = true;
            error = cam.previewRecovery.recovering() ? recorderFaultText(RecorderFault::gpuRemoved)
                : k("영상 표시를 시작할 수 없습니다. ") + juce::String::fromUTF8(e.what());
        }
    }
}
void RecorderSession::enterTimeline(bool on)
{
    if (!lifecycle->acceptsCommands()) return;
    timeline = on;
    if (recording()) return; // same two HWNDs and live presenters survive tab changes
    if (!on) { wantPlay = false; clearPlayback(); presentLive(); }
    else if (document.getProject().activeTimelineEnd() > 0) preparePlayback();
}
juce::Result RecorderSession::record()
{
    if (!readyToRecord()) return juce::Result::fail(k("녹화 장치와 프로젝트 저장 위치를 확인하세요."));
    if (lifecycle->snapshot() & RecorderLifecycle::exporting)
    {
        if (!recordAfterExport)
        {
            recordAfterExport = true; notice = k("내보내기를 안전하게 멈춘 뒤 녹화를 시작합니다.");
            const auto stop = exclusiveStops.at(RecorderLifecycle::exporting); stop();
        }
        return juce::Result::ok(); // endExclusive acknowledges the export checkpoint/join
    }
    if (!lifecycle->begin(RecorderLifecycle::recording)) return juce::Result::fail(recorderFaultText(RecorderFault::updateBusy));
    clearPlayback(); wantPlay = pendingLatest = false; presentLive(); error.clear(); notice.clear(); recordedMarkers.clear(); peaksPublished.clear();
    TakeController::Config c; c.projectDirectory = document.getFile().getParentDirectory(); c.takeId = juce::Uuid();
    configurePlacement(c);
    c.cameraSymbolicLink = current.cameraDeviceIds[0].toStdString(); c.cameraMode = cameras[0]->mode;
    c.projectFps = int(document.getProject().fps.numerator); c.externalCapture = true;
    c.cameraGeneration = cameras[0]->capture->generation();
    c.camera2.enabled = cameraReady(1);
    if (c.camera2.enabled)
    {
        c.camera2.symbolicLink = current.cameraDeviceIds[1].toStdString(); c.camera2.mode = cameras[1]->mode;
        c.camera2.generation = cameras[1]->capture->generation();
    }
    c.outputMapping = current.output.mono ? std::vector<int>{current.output.monoChannel}
                                         : std::vector<int>{current.output.left, current.output.right};
    for (unsigned i = 0; i < (c.camera2.enabled ? 2u : 1u); ++i)
    {
        const auto key = calibrationKey(current.cameraDeviceIds[i].toStdString(), cameras[i]->mode, c.exposure[i],
            device.name.toStdString(), device.sampleRate, device.bufferFrames, c.outputMapping, audio.calibrationInputMapping());
        for (const auto& profile : calibrationProfiles) if (profile.key == key) { c.calibration[i] = profile; break; }
    }
    if (current.cameraEnabled[1] && !c.camera2.enabled) notice = k("캠2 연결을 확인하세요. 캠1으로 녹화합니다.");
    const auto result = take.prepare(c); if (result.wasOk()) { autoStart = true; derivedWorker.setRecording(true); }
    else lifecycle->end(RecorderLifecycle::recording);
    return result;
}
void RecorderSession::configurePlacement(TakeController::Config& config)
{
    // The recording tab has no visible cursor: reserve the active end there.
    config.placementSample = timeline ? playhead() : document.getProject().activeTimelineEnd();
    config.editPlacement = [this](EditState& e)
    {
        for (auto& track : e.tracks) if (track.kind == TrackKind::mic && track.microphoneIndex >= 0 && track.microphoneIndex < 8)
        {
            const auto& name = current.microphoneNames[unsigned(track.microphoneIndex)];
            if (name.isNotEmpty()) track.name = name;
        }
        e.markers.insert(e.markers.end(), recordedMarkers.begin(), recordedMarkers.end());
    };
}
juce::Result RecorderSession::setCalibrationProfiles(std::vector<CalibrationProfile> profiles)
{
    if (!lifecycle->acceptsCommands()) return juce::Result::fail(recorderFaultText(RecorderFault::updateBusy));
    if (busy()) return juce::Result::fail(k("녹화와 저장이 끝난 뒤 보정을 변경하세요."));
    try { for (const auto& profile : profiles) profile.requireMatch(profile.key); }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
    calibrationProfiles = std::move(profiles); return juce::Result::ok();
}
CalibrationMatch RecorderSession::calibrationMatches(const UserSettings& settings) const
{
    const auto match = gocue::recorder::calibrationMatches(settings, calibrationProfiles);
    if (match != CalibrationMatch::matched) return match;
    const auto& actual = audio.deviceInfo();
    if (configuring() || (actual.sampleRate && (actual.name != settings.asioDeviceId
        || actual.sampleRate != settings.preferredSampleRate || actual.bufferFrames != unsigned(settings.bufferSize))))
        return CalibrationMatch::settingsChanged;
    for (unsigned i = 0; i < settings.cameraEnabled.size(); ++i) if (settings.cameraEnabled[i])
    {
        if (actual.sampleRate && calibrationKey(settings, i).inputMapping != audio.calibrationInputMapping()) return CalibrationMatch::settingsChanged;
        if (cameraReady(i) && cameras[i]->mode.text() != settings.cameraModes[i].toStdString()) return CalibrationMatch::settingsChanged;
    }
    return match;
}
juce::Result RecorderSession::stopRecording() { return take.stop(); }
void RecorderSession::clearPlayback()
{
    if (playback) cursor = playhead();
    playback.reset();
}
void RecorderSession::preparePlayback()
{
    if (!lifecycle->acceptsCommands()) return;
    if (configuring() || recording() || take.state() == TakeController::State::finalizing || planWork.valid() || playback || !device.sampleRate) return;
    if (device.sampleRate != document.getProject().Fs) { error = rateMismatchText(document.getProject().Fs, device.sampleRate); return; }
    const auto snapshot = document.snapshot(); const auto folder = document.getFile().getParentDirectory();
    if (!snapshot->activeTimelineEnd()) { wantPlay = pendingLatest = false; return; }
    notice = k("재생 준비 중");
    const auto generation = lifecycle->generation();
    planImportControl = std::make_shared<AudioImportControl>();
    try
    {
    planWork = std::async(std::launch::async, [this, snapshot, folder, generation, importControl = planImportControl]
    {
        auto plan = std::make_unique<PreparedPlan>(); plan->project = snapshot->projectId; plan->revision = snapshot->editRevision; plan->end = snapshot->activeTimelineEnd();
        plan->generation = generation;
        try
        {
            MediaIndex index;
            const bool hasImports = std::any_of(snapshot->media->assets.begin(), snapshot->media->assets.end(),
                [](const auto& asset) { return asset.kind == AssetKind::importAudio; });
            if (hasImports) plan->importedPlan = compileAudioRenderPlan(*snapshot)->timeline;
            std::set<Id> openedAudio;
            for (const auto& track : snapshot->tracks)
            {
                PlaybackAudioTrack audioTrack; audioTrack.trackId = track.trackId; audioTrack.mute = track.mute; audioTrack.solo = track.solo;
                for (const auto& clip : track.clips.items()) if (snapshot->isActive(clip))
                {
                    const auto* asset = snapshot->media->findAsset(clip.assetId); if (!asset) continue;
                    const auto key = snapshot->projectId + "/" + asset->assetId + "/" + juce::String(asset->mediaGeneration);
                    if (asset->kind == AssetKind::camera)
                    {
                        if (asset->relativePath.contains(".recording.") || asset->availableRanges.empty()) continue;
                        auto& source = videoIndexes[key];
                        if (!source)
                        {
                            auto video = std::make_shared<VideoIndex>(*index.openVideo(folder.getChildFile(asset->relativePath), snapshot->Fs));
                            video->epoch = std::make_shared<MediaEpoch>(); video->generation = std::uint64_t(asset->mediaGeneration);
                            video->epoch->value.store(video->generation); source = std::move(video);
                        }
                        for (const auto& range : asset->availableRanges)
                        {
                            const auto begin = std::max(clip.sourceIn, range.start);
                            const auto end = std::min({clip.sourceIn + clip.lengthSamples, range.start + range.length, source->length});
                            if (begin >= end) continue;
                            auto mapping = mappingFor(clip, *asset); mapping.sourceIn = begin;
                            mapping.timelineStartSample += begin - clip.sourceIn; mapping.lengthSamples = end - begin; mapping.gaps.clear();
                            // Preserve the original edit boundaries: an unavailable
                            // head/tail is a source gap, never a subframe clip seam.
                            plan->videos.push_back({mapping, track.kind == TrackKind::cam2 ? 1u : 0u, source,
                                begin == clip.sourceIn, end == clip.sourceIn + clip.lengthSamples});
                        }
                    }
                    else if (asset->kind == AssetKind::mic)
                    {
                        auto& source = wavIndexes[key];
                        if (!source) source = indexRecordedAudio(*asset, folder, snapshot->Fs, track.trackId);
                        auto mapping = mappingFor(clip, *asset);
                        // WavSource contains only committed chunks; its reader supplies silence
                        // for absent ranges, including a failed take's trailing gap.
                        mapping.gaps.clear(); audioTrack.clips.push_back({mapping, source});
                        if (hasImports && openedAudio.insert(asset->assetId).second)
                            plan->audioSources.push_back({asset->assetId, wavAudioSource(source, true)});
                    }
                    else if (asset->kind == AssetKind::importAudio && openedAudio.insert(asset->assetId).second)
                    {
                        auto found = importedIndexes.find(key);
                        if (found == importedIndexes.end())
                        {
                            const auto info = AudioImport::loadInfo(folder, *asset); CachedImportedAudio cache;
                            checkResult(ImportedAudioCache::build(folder, *asset, info, snapshot->Fs, *importControl, cache));
                            found = importedIndexes.emplace(key, std::move(cache)).first;
                        }
                        plan->audioSources.push_back({asset->assetId, importedAudioSource(*asset, found->second)});
                    }
                }
                if (track.kind == TrackKind::mic || track.kind == TrackKind::importAudio) plan->tracks.push_back(std::move(audioTrack));
            }
        }
        catch (const std::exception& e) { plan->error = juce::String::fromUTF8(e.what()); }
        return plan;
    });
    }
    catch (const std::exception& e) { playbackPreparationFailed(juce::String::fromUTF8(e.what())); }
    catch (...) { playbackPreparationFailed(k("알 수 없는 재생 준비 오류")); }
}
void RecorderSession::playbackPreparationFailed(const juce::String& reason)
{
    clearPlayback(); wantPlay = pendingLatest = false; notice.clear();
    error = k("재생 준비를 완료할 수 없습니다. ") + reason;
}
std::unique_ptr<RecorderSession::PreparedPlan> RecorderSession::collectPreparedPlan()
{
    // Exceptions stored by std::async must never escape the message timer, even
    // during shutdown. get() also retires the failed future so shutdown can join.
    try { return planWork.get(); }
    catch (const std::exception& e) { playbackPreparationFailed(juce::String::fromUTF8(e.what())); }
    catch (...) { playbackPreparationFailed(k("알 수 없는 재생 준비 오류")); }
    return {};
}
std::shared_ptr<const WavSource> RecorderSession::indexRecordedAudio(const MediaAsset& asset, const juce::File& folder, unsigned Fs, const Id& track)
{
    return MediaIndex::recordedAudio(asset, folder, Fs, track);
}
void RecorderSession::play(bool latest)
{
    if (!lifecycle->acceptsCommands()) return;
    if ((recording() && !(latest && take.state() == TakeController::State::stopping)) || configuring()) return;
    playbackButtonQpc = qpcNow(); firstPlaybackVideoQpc = firstPlaybackAudioQpc = firstPlaybackAudibleQpc = 0;
    wantPlay = true;
    if (latest)
    {
        pendingLatest = true; clearPlayback(); cursor = take.placementSample();
        if (take.state() == TakeController::State::idle && !document.getProject().media->takes.empty()) cursor = document.getProject().media->takes.back().placementSample;
    }
    timeline = true;
    if (playhead() >= document.getProject().activeTimelineEnd())
    {
        wantPlay = pendingLatest = false;
        if (playback) playback->transport.stop();
        return;
    }
    if (playback) { playback->transport.play(); pendingLatest = false; }
    else preparePlayback();
}
juce::Result RecorderSession::pause()
{
    wantPlay = false;
    try { if (playback) playback->transport.pause(); return juce::Result::ok(); }
    catch (const std::exception& e) { error = k("재생을 일시 정지할 수 없습니다. ") + juce::String::fromUTF8(e.what()); }
    catch (...) { error = k("재생을 일시 정지할 수 없습니다. 알 수 없는 오류"); }
    clearPlayback(); // detach output when the callback cannot accept another command
    return juce::Result::fail(error);
}
void RecorderSession::stopPlayback()
{ wantPlay = false; if (playback) { playback->transport.scrub(playhead(), true, qpcNow()); playback->transport.stop(); } }
void RecorderSession::goToStart() { if (!lifecycle->acceptsCommands()) return; wantPlay = false; cursor = 0; if (playback) playback->transport.goToStart(); else preparePlayback(); }
void RecorderSession::scrub(Sample sample, bool released)
{
    if (!lifecycle->acceptsCommands()) return;
    if (recording()) return; wantPlay = false;
    cursor = std::clamp(sample, Sample{0}, Sample(document.getProject().Fs) * 24 * 60 * 60);
    if (playback) playback->transport.scrub(cursor, released, qpcNow()); else preparePlayback();
}
bool RecorderSession::playing() const
{ if (!playback) return wantPlay; const auto s = playback->transport.snapshot().state; return wantPlay || s == TransportState::playing || s == TransportState::scheduled; }
Sample RecorderSession::playhead() const
{ return playback ? playback->transport.playhead(qpcNow()) : cursor; }
Sample RecorderSession::elapsed() const
{ return recording() && !configuring() && audio.startSample() >= 0 ? std::max(Sample{0}, audio.acceptedEnd() - audio.startSample()) : take.logicalLength(); }
void RecorderSession::setMonitoring(std::uint8_t mask) { audio.setInputMonitoring(mask != 0, mask); }
void RecorderSession::updateMicrophoneSettings(const UserSettings& settings)
{ current.microphoneNames = settings.microphoneNames; current.microphoneArmed = settings.microphoneArmed; }
void RecorderSession::addMarker()
{
    addMarker(k("마커"), recording() ? take.placementSample() + elapsed() : playhead());
}
void RecorderSession::addMarker(const juce::String& name, Sample at)
{
    Marker m; m.name = name; m.sample = at;
    if (recording()) recordedMarkers.push_back(m);
    else document.addMarker(std::move(m));
}
void RecorderSession::refreshPlaybackPlan()
{ const bool resume = playing(); clearPlayback(); wantPlay = resume; if (timeline) preparePlayback(); }
void RecorderSession::projectChanged()
{
    lifecycle->invalidate();
    ++derivedGeneration;
    derivedWorker.invalidate();
    { const std::lock_guard<std::mutex> lock(derivedMutex); derivedResults.clear(); }
    clearPlayback(); take.reset(); cursor = document.getProject().activeTimelineEnd(); wantPlay = false; pendingLatest = false; peaksPublished.clear(); error.clear(); notice.clear();
    if (planImportControl) planImportControl->cancelled.store(true);
    if (planWork.valid()) planWork.wait(); // project replacement is disabled while plan preparation is pending
    videoIndexes.clear(); wavIndexes.clear(); importedIndexes.clear(); derivedKeys.clear(); derivedProject = document.getProject().projectId;
    scheduleDerived();
}
void RecorderSession::scheduleDerived()
{
    if (document.getFile() == juce::File() || recording()) return;
    const auto p = document.snapshot(); const auto folder = document.getFile().getParentDirectory();
    const auto generation = derivedGeneration.load();
    for (const auto& asset : p->media->assets) if (asset.kind == AssetKind::importAudio)
    {
        const auto key = p->projectId + "/import-peaks/" + asset.assetId + "/" + juce::String(asset.mediaGeneration);
        if (derivedKeys.count(key)) continue;
        const auto accepted = derivedWorker.enqueue(key, [this, asset, folder, p, generation](const auto& yield)
        {
            const auto cancelled = [&] { return generation != derivedGeneration.load() || yield(); };
            if (cancelled()) return;
            AudioImportControl control; control.onProgress = [&](auto, double) { control.cancelled.store(cancelled()); };
            CachedImportedAudio cache;
            const auto info = AudioImport::loadInfo(folder, asset);
            const auto result = ImportedAudioCache::build(folder, asset, info, p->Fs, control, cache);
            if (result.failed() || cancelled()) return;
            auto peaks = ImportedAudioCache::peakSnapshot(cache);
            const std::lock_guard<std::mutex> lock(derivedMutex);
            if (generation == derivedGeneration.load()) derivedResults.push_back({asset.assetId, {}, std::move(peaks), 0, generation, p->projectId, asset.mediaGeneration});
        });
        if (accepted) derivedKeys.insert(key);
    }
    for (const auto& takeItem : p->media->takes)
    {
        auto key = p->projectId + "/peaks/" + takeItem.takeId;
        for (const auto& id : takeItem.microphoneAssetIds)
            if (const auto* asset = p->media->findAsset(id)) key += "/" + id + "/" + juce::String(asset->mediaGeneration);
        if (derivedKeys.count(key)) continue;
        const auto accepted = derivedWorker.enqueue(key, [this, takeItem, folder, p, generation](const auto& yield)
        {
            if (generation != derivedGeneration.load() || yield()) return;
            const auto file = folder.getChildFile("cache/" + takeItem.takeId + ".peaks.json");
            if (!file.existsAsFile()) return;
            const auto peaks = PeakCache::read(file);
            const std::lock_guard<std::mutex> lock(derivedMutex);
            if (generation != derivedGeneration.load()) return;
            for (unsigned i = 0; i < takeItem.microphoneAssetIds.size(); ++i)
                if (const auto* asset = p->media->findAsset(takeItem.microphoneAssetIds[i]))
                    derivedResults.push_back({asset->assetId, {}, peaks, i, generation, p->projectId, asset->mediaGeneration});
        });
        if (accepted) derivedKeys.insert(key);
    }
    for (const auto& asset : p->media->assets) if (asset.kind == AssetKind::camera && !asset.relativePath.contains(".recording.") && !asset.availableRanges.empty())
    {
        const auto key = p->projectId + "/thumb/" + asset.assetId + "/" + juce::String(asset.mediaGeneration);
        if (derivedKeys.count(key)) continue;
        const auto accepted = derivedWorker.enqueue(key, [this, asset, folder, rate = p->Fs, project = p->projectId, generation](const auto& yield)
        {
            const auto cancelled = [&] { return generation != derivedGeneration.load() || yield(); };
            auto frames = ThumbnailCache::decode(folder.getChildFile(asset.relativePath), rate, cancelled);
            if (!frames.empty() && !cancelled())
            { const std::lock_guard<std::mutex> lock(derivedMutex); derivedResults.push_back({asset.assetId, std::move(frames), {}, 0, generation, project, asset.mediaGeneration}); }
        });
        if (accepted) derivedKeys.insert(key);
    }
}
void RecorderSession::tick()
{
    if (ready(deviceWork))
    {
        DeviceResult result; bool collected = false;
        try { result = deviceWork.get(); collected = true; }
        catch (const std::exception& e) { for (auto& cam : cameras) cam.reset(); configurationFailed(juce::String::fromUTF8(e.what())); }
        catch (...) { for (auto& cam : cameras) cam.reset(); configurationFailed(k("알 수 없는 장치 작업 오류")); }
        if (collected)
        {
        current = result.settings; device = audio.deviceInfo();
        lifecycle->end(RecorderLifecycle::configuring);
        adoptDeviceSampleRate(document, device.sampleRate);
        notice.clear(); if (result.result.failed()) error = result.result.getErrorMessage();
        if (onConfigured) onConfigured(result.result, current);
        }
    }
    if (configuring()) return;
    if (recordAfterExport && !(lifecycle->snapshot() & RecorderLifecycle::exporting))
    { recordAfterExport = false; const auto result = record(); if (result.failed()) error = result.getErrorMessage(); }
    take.tick();
    lifecycle->set(RecorderLifecycle::recording, recording());
    lifecycle->set(RecorderLifecycle::finalizing, take.state() == TakeController::State::finalizing);
    if (shuttingDown)
    {
        if (ready(planWork)) collectPreparedPlan(); // retire late/failed preparations
        if (permitRelease && take.shutdownComplete() && !planWork.valid() && !releaseWork.valid() && !resourcesReleased)
        {
            clearPlayback(); // detaches ASIO client before renderer/video join
            for (auto& cam : cameras) if (cam && cam->capture) cam->capture->requestStop();
            try { if (beforeWorkerStart) beforeWorkerStart("release"); releaseWork = std::async(std::launch::async, [this] { for (auto& cam : cameras) cam.reset(); }); }
            catch (const std::exception& e) { error = k("장치 해제 작업을 시작할 수 없습니다. ") + juce::String::fromUTF8(e.what()); finishDeviceRelease(); }
            catch (...) { error = k("장치 해제 작업을 시작할 수 없습니다. 알 수 없는 오류"); finishDeviceRelease(); }
        }
        // The ASIO driver is closed on the thread that created it (message thread), after the cameras.
        if (ready(releaseWork))
        {
            try { releaseWork.get(); }
            catch (const std::exception& e) { error = k("장치 해제 작업 오류: ") + juce::String::fromUTF8(e.what()); }
            catch (...) { error = k("알 수 없는 장치 해제 작업 오류"); }
            finishDeviceRelease();
        }
        return;
    }
    if (take.warning().isNotEmpty()) notice = take.warning();
    if (audio.processingDelayed() || take.processingDelayed()) notice = recorderFaultText(RecorderFault::processingDelay);
    else if (notice == recorderFaultText(RecorderFault::processingDelay)) notice.clear();
    if (recording() && audio.error() == RecorderAudioEngine::Error::writeFailed) error = k("저장 장치에 쓸 수 없어 녹화를 멈췄습니다.");
    if (autoStart && take.state() == TakeController::State::armed)
    { const auto r = take.start(); autoStart = false; if (r.failed()) error = r.getErrorMessage(); }
    if (take.state() == TakeController::State::partialFailure) autoStart = false;
    const auto& meta = take.placementMetadata();
    if (meta.ready && peaksPublished.isEmpty())
    {
        if (!playing()) cursor = meta.timelineSample + (meta.Nstop - meta.N0);
        const auto& p = document.getProject();
        if (!p.media->takes.empty())
        {
            const auto& t = p.media->takes.back(); peaksPublished = t.takeId;
            for (unsigned i = 0; i < t.microphoneAssetIds.size(); ++i) if (onPeaks && meta.waveform) onPeaks(t.microphoneAssetIds[i], meta.waveform, i);
            recordedMarkers.clear();
        }
    }
    derivedWorker.setRecording(recording());
    for (unsigned i = 0; i < 2; ++i) if (current.cameraEnabled[i] && cameras[i] && !cameras[i]->failed && cameras[i]->capture)
    {
        if (cameras[i]->capture->failureDetected() || cameras[i]->capture->finished())
        {
            cameras[i]->capture->requestStop(); cameras[i]->failed = true;
            take.cameraFailed(i, cameras[i]->capture->generation());
            error = recorderFaultText(i ? RecorderFault::camera2Disconnected : RecorderFault::camera1Disconnected);
            cameras[i]->presenter.reset();
            cameras[i]->previewRecovery.reset();
        }
        else if (cameras[i]->presenter && cameras[i]->presenter->finished())
        {
            auto& cam = *cameras[i]; cam.presenter->stop(); const auto reason = cam.presenter->error();
            cam.previewRecovery.lost(reason, IoHealth::now(), [&] { cam.presenter.reset(); });
            cam.displayFailed = true;
            error = cam.previewRecovery.recovering() ? recorderFaultText(RecorderFault::gpuRemoved)
                : k("영상 표시 장치 연결을 확인하세요. ") + juce::String(reason);
        }
        auto& cam = *cameras[i];
        if (cameraReady(i) && hosts[i] && !playback && cam.previewRecovery.retry(IoHealth::now(), [&]
        {
            try { cam.presenter = std::make_unique<PreviewPresenter>(static_cast<HWND>(hosts[i]), *cam.pool, cam.telemetry, 1920, 1080); cam.presenter->start(); return true; }
            catch (...) { cam.presenter.reset(); return false; }
        })) { cam.displayFailed = false; if (error == recorderFaultText(RecorderFault::gpuRemoved)) error.clear(); }
    }
    if (take.state() == TakeController::State::partialFailure && error.isEmpty())
    {
        error = audio.error() == RecorderAudioEngine::Error::writeFailed ? k("저장 장치에 쓸 수 없어 녹화를 멈췄습니다.")
            : audio.error() != RecorderAudioEngine::Error::none ? k("오디오 장치 오류로 녹화를 멈췄습니다.") : k("일반 MP4 마무리 실패 · 재시도");
        if (take.error().isNotEmpty()) error += " " + take.error();
    }
    if (ready(planWork))
    {
        auto plan = collectPreparedPlan();
        if (plan && lifecycle->accepts(plan->generation) && plan->project == document.getProject().projectId && plan->revision == document.getProject().editRevision && timeline && !recording())
        {
            if (plan->error.isNotEmpty()) playbackPreparationFailed(plan->error);
            else try
            {
                for (auto& cam : cameras) if (cam) cam->presenter.reset();
                playback = std::make_unique<Playback>(device.sampleRate, device.bufferFrames, plan->end, audio);
                if (plan->importedPlan) playback->renderer.setPlan(std::move(plan->importedPlan), std::move(plan->audioSources));
                else playback->renderer.setPlan(std::move(plan->tracks), plan->end);
                playback->video.prepare(std::move(plan->videos));
                for (unsigned i = 0; i < 2; ++i) if (hosts[i]) playback->video.attachPlaybackView(i, hosts[i]);
                playback->output.start(playback->transport); playback->transport.seek(std::max(cursor, Sample{0}));
                if (wantPlay) playback->transport.play(); pendingLatest = false; notice.clear();
            }
            catch (const std::exception& e) { playbackPreparationFailed(juce::String::fromUTF8(e.what())); }
            catch (...) { playbackPreparationFailed(k("알 수 없는 재생 준비 오류")); }
        }
    }
    if (playback)
    {
        playback->transport.service(playback->renderer, playback->video, playback->output, qpcNow());
        const auto state = playback->transport.snapshot();
        if (playback->transport.status().failed()) { error = k("재생을 중단했습니다. ") + playback->transport.status().getErrorMessage(); wantPlay = false; }
        if (state.firstBlockQpc >= playbackButtonQpc && playbackButtonQpc)
        {
            if (!firstPlaybackAudioQpc) firstPlaybackAudioQpc = state.firstBlockQpc;
            if (!firstPlaybackAudibleQpc) firstPlaybackAudibleQpc = state.firstAudibleQpc;
            const auto frame = playback->video.lastPresentation(0);
            if (!firstPlaybackVideoQpc && frame.generation == state.generation && frame.qpc >= playbackButtonQpc) firstPlaybackVideoQpc = frame.qpc;
        }
        if (state.state == TransportState::paused || state.state == TransportState::failed) wantPlay = false;
    }
    else if (timeline && !recording() && (pendingLatest || error.isEmpty())) preparePlayback();
    else presentLive();
    if (!busy()) scheduleDerived();
    std::deque<Derived> completed;
    { const std::lock_guard<std::mutex> lock(derivedMutex); completed.swap(derivedResults); }
    for (auto& item : completed)
    {
        const auto& currentProject = document.getProject();
        const auto* asset = currentProject.media->findAsset(item.asset);
        if (!asset || item.requestGeneration != derivedGeneration.load() || item.project != currentProject.projectId
            || item.mediaGeneration != asset->mediaGeneration) continue;
        if (!item.thumbs.empty() && onThumbnails) onThumbnails(item.asset, std::move(item.thumbs));
        if (item.peaks.sampleRate && onLoadedPeaks) onLoadedPeaks(item.asset, std::move(item.peaks), item.channel);
    }
}
void RecorderSession::finishDeviceRelease()
{
    // Rare synchronous fallback after a launch/get failure; no worker still owns cameras.
    for (auto& cam : cameras) cam.reset();
    const auto result = audio.closeDevice(); resourcesReleased = result.wasOk();
    if (result.failed()) error = result.getErrorMessage();
}
void RecorderSession::requestShutdown()
{
    if (shuttingDown) return;
    lifecycle->blockCommands(); shuttingDown = true; autoStart = wantPlay = pendingLatest = recordAfterExport = false;
    if (planImportControl) planImportControl->cancelled.store(true);
    ++derivedGeneration; derivedWorker.invalidate();
    take.requestShutdown();
    clearPlayback(); // detach output client and invalidate pending playback before file commits
    const auto stops = exclusiveStops; for (const auto& stop : stops) if (stop.second) stop.second();
}
bool RecorderSession::shutdownComplete() const { return resourcesReleased; }
bool RecorderSession::readyForShutdownCommit() const
{
    return shuttingDown && !configuring() && take.shutdownComplete() && !planWork.valid() && !lifecycle->captureBusy()
        && !(lifecycle->snapshot() & (RecorderLifecycle::exporting | RecorderLifecycle::dubbing | RecorderLifecycle::recovering));
}
void RecorderSession::releaseForShutdown() { if (readyForShutdownCommit()) permitRelease = true; }
void RecorderSession::resumeFromSleep()
{
    lifecycle->invalidate(); autoStart = wantPlay = false; stopPlayback();
    if (!configuring()) audio.deviceDiscontinuity();
    error = recorderFaultText(RecorderFault::resume);
}
juce::Result RecorderSession::beginExclusive(RecorderLifecycle::Activity activity, std::function<void()> stop)
{
    if ((activity != RecorderLifecycle::exporting && activity != RecorderLifecycle::dubbing && activity != RecorderLifecycle::recovering)
        || busy() || !lifecycle->begin(activity)) return juce::Result::fail(recorderFaultText(RecorderFault::updateBusy));
    exclusiveStops[activity] = std::move(stop); stopPlayback(); lifecycle->invalidate(); return juce::Result::ok();
}
void RecorderSession::endExclusive(RecorderLifecycle::Activity activity) { exclusiveStops.erase(activity); lifecycle->end(activity); }
}
