#include "RecorderSession.h"
#include "ui/UiState.h"
#include <algorithm>
#include <chrono>

namespace gocue::recorder
{
namespace
{
juce::String k(const char* s) { return juce::String::fromUTF8(s); }
void checkResult(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
bool activeTake(TakeController::State s)
{ using S = TakeController::State; return s == S::preparing || s == S::armed || s == S::recording || s == S::stopping; }
template<class T> bool ready(std::future<T>& f) { return f.valid() && f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }
RenderClip mappingFor(const Clip& c, const MediaAsset& a)
{ RenderClip r; r.clipId = c.clipId; r.trackId = c.trackId; r.assetId = c.assetId; r.sourceIn = c.sourceIn; r.timelineStartSample = c.timelineStartSample; r.lengthSamples = c.lengthSamples; r.mediaGeneration = a.mediaGeneration; r.gaps = a.gaps; return r; }
}
struct RecorderSession::LiveCamera
{
    std::shared_ptr<CaptureTelemetry> telemetry;
    std::shared_ptr<VideoSurfacePool> pool;
    std::unique_ptr<MfCameraCapture> capture;
    std::unique_ptr<PreviewPresenter> presenter;
    CameraMode mode;
    std::unique_ptr<MfRuntime> runtime;
    bool failed = false, previewFailed = false;
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
struct RecorderSession::PreparedPlan
{
    std::vector<PlaybackVideoClip> videos;
    std::vector<PlaybackAudioTrack> tracks;
    Sample end = 0, revision = 0;
    Id project;
    juce::String error;
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
RecorderSession::RecorderSession(RecorderDocument& d) : document(d), take(d, audio) {}
RecorderSession::~RecorderSession()
{
    if (deviceWork.valid()) deviceWork.wait();
    if (planWork.valid()) planWork.wait();
    playback.reset();
    for (auto& cam : cameras) cam.reset(); // stop capture offers before TakeController destruction
}
void RecorderSession::setHosts(std::array<void*, 2> next) { hosts = next; }
bool RecorderSession::configuring() const { return deviceWork.valid(); }
bool RecorderSession::recording() const { return activeTake(take.state()); }
bool RecorderSession::busy() const { return configuring() || recording() || take.state() == TakeController::State::finalizing || planWork.valid(); }
bool RecorderSession::cameraReady(unsigned n) const
{ return !configuring() && n < 2 && current.cameraEnabled[n] && cameras[n] && !cameras[n]->failed && cameras[n]->capture
    && !cameras[n]->capture->failureDetected() && !cameras[n]->capture->finished(); }
bool RecorderSession::readyToRecord() const
{
    return !busy() && document.getFile() != juce::File() && cameraReady(0) && device.sampleRate
        && audio.clockReady() && validateAudioSettings(current, device, document.getProject()).wasOk();
}
juce::String RecorderSession::cameraCaption(unsigned n) const
{
    auto text = k(n ? "캠2" : "캠1");
    if (cameraReady(n)) text += " · 1080p " + juce::String(cameras[n]->mode.fps.value(), cameras[n]->mode.fps.denominator == 1 ? 0 : 2) + k(" 입력");
    return text + k(" / 프로젝트 ") + juce::String(document.getProject().fps.numerator);
}
juce::Result RecorderSession::configure(UserSettings settings)
{
    if (busy()) return juce::Result::fail(k("녹화와 저장이 끝난 뒤 설정을 변경하세요."));
    const auto valid = settings.validate(); if (valid.failed()) return valid;
    if (settings.cameraEnabled[0] && settings.cameraEnabled[1]
        && CameraCatalog::sameDevice(settings.cameraDeviceIds[0].toStdString(), settings.cameraDeviceIds[1].toStdString()))
        return juce::Result::fail(k("같은 카메라를 두 번 선택할 수 없습니다."));
    clearPlayback(); error.clear(); notice = k("장치를 연결하는 중입니다.");
    const auto fixedFs = document.getProject().media->assets.empty() ? 0u : document.getProject().Fs;
    deviceWork = std::async(std::launch::async, [this, settings, fixedFs]() mutable
    {
        DeviceResult result; result.settings = settings;
        try
        {
            ComApartment apartment;
            for (auto& cam : cameras) cam.reset();
            checkResult(audio.closeDevice());
            std::array<int, 8> map; map.fill(-1);
            for (std::size_t i = 0; i < settings.physicalInputs.size(); ++i) map[i] = settings.physicalInputs[i];
            checkResult(audio.setInputMap(map)); checkResult(audio.setOutputMap(settings.output));
            if (settings.asioDeviceId.isNotEmpty())
            {
                checkResult(audio.openDevice(settings.asioDeviceId, fixedFs ? fixedFs : settings.preferredSampleRate, settings.bufferSize));
                if (fixedFs && audio.deviceInfo().sampleRate != fixedFs)
                { audio.closeDevice(); throw std::runtime_error("프로젝트 샘플레이트가 고정되어 있습니다. ASIO 장치의 샘플레이트를 맞추세요."); }
                for (unsigned i = 0; i < 8; ++i) checkResult(audio.arm(i, map[i] >= 0 && settings.microphoneArmed[i]));
                result.settings.preferredSampleRate = audio.deviceInfo().sampleRate;
                result.settings.bufferSize = int(audio.deviceInfo().bufferFrames);
            }
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
                    result.result = juce::Result::fail(k(i ? "캠2 연결을 확인하세요. " : "캠1 연결을 확인하세요. ") + juce::String::fromUTF8(e.what()));
                }
            }
        }
        catch (const std::exception& e) { result.result = juce::Result::fail(k("장치 연결을 확인하세요. ") + juce::String::fromUTF8(e.what())); }
        return result;
    });
    return juce::Result::ok();
}
void RecorderSession::presentLive()
{
    if (configuring() || playback) return;
    for (unsigned i = 0; i < 2; ++i) if (cameraReady(i) && hosts[i] && !cameras[i]->previewFailed && !cameras[i]->presenter)
    {
        try
        {
            cameras[i]->presenter = std::make_unique<PreviewPresenter>(static_cast<HWND>(hosts[i]), *cameras[i]->pool, cameras[i]->telemetry, 1920, 1080);
            cameras[i]->presenter->start();
        }
        catch (const std::exception& e) { cameras[i]->presenter.reset(); cameras[i]->previewFailed = true; error = k("영상 표시를 시작할 수 없습니다. ") + juce::String::fromUTF8(e.what()); }
    }
}
void RecorderSession::enterTimeline(bool on)
{
    timeline = on;
    if (recording()) return; // same two HWNDs and live presenters survive tab changes
    if (!on) { wantPlay = false; clearPlayback(); presentLive(); }
    else if (document.getProject().activeTimelineEnd() > 0) preparePlayback();
}
juce::Result RecorderSession::record()
{
    if (!readyToRecord()) return juce::Result::fail(k("녹화 장치와 프로젝트 저장 위치를 확인하세요."));
    clearPlayback(); presentLive(); error.clear(); notice.clear(); recordedMarkers.clear(); peaksPublished.clear();
    TakeController::Config c; c.projectDirectory = document.getFile().getParentDirectory(); c.takeId = juce::Uuid();
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
            device.name.toStdString(), device.sampleRate, device.bufferFrames, c.outputMapping);
        for (const auto& profile : calibrationProfiles) if (profile.key == key) { c.calibration[i] = profile; break; }
    }
    if (current.cameraEnabled[1] && !c.camera2.enabled) notice = k("캠2 연결을 확인하세요. 캠1으로 녹화합니다.");
    const auto result = take.prepare(c); if (result.wasOk()) { autoStart = true; derivedWorker.setRecording(true); }
    return result;
}
juce::Result RecorderSession::setCalibrationProfiles(std::vector<CalibrationProfile> profiles)
{
    if (busy()) return juce::Result::fail(k("녹화와 저장이 끝난 뒤 보정을 변경하세요."));
    try { for (const auto& profile : profiles) profile.requireMatch(profile.key); }
    catch (const std::exception& e) { return juce::Result::fail(e.what()); }
    calibrationProfiles = std::move(profiles); return juce::Result::ok();
}
juce::Result RecorderSession::stopRecording() { return take.stop(); }
void RecorderSession::clearPlayback()
{
    if (playback) cursor = playhead();
    playback.reset();
}
void RecorderSession::preparePlayback()
{
    if (configuring() || recording() || take.state() == TakeController::State::finalizing || planWork.valid() || playback || !device.sampleRate) return;
    if (device.sampleRate != document.getProject().Fs) { error = k("프로젝트와 ASIO 샘플레이트가 다릅니다."); return; }
    const auto snapshot = document.snapshot(); const auto folder = document.getFile().getParentDirectory();
    if (!snapshot->activeTimelineEnd()) return;
    notice = k("재생 준비 중");
    planWork = std::async(std::launch::async, [this, snapshot, folder]
    {
        auto plan = std::make_unique<PreparedPlan>(); plan->project = snapshot->projectId; plan->revision = snapshot->editRevision; plan->end = snapshot->activeTimelineEnd();
        try
        {
            MediaIndex index;
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
                            plan->videos.push_back({mapping, track.kind == TrackKind::cam2 ? 1u : 0u, source});
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
                    }
                    else throw std::runtime_error("불러온 오디오의 타임라인 재생 연결은 준비 중입니다.");
                }
                if (track.kind == TrackKind::mic || track.kind == TrackKind::importAudio) plan->tracks.push_back(std::move(audioTrack));
            }
        }
        catch (const std::exception& e) { plan->error = juce::String::fromUTF8(e.what()); }
        return plan;
    });
}
std::shared_ptr<const WavSource> RecorderSession::indexRecordedAudio(const MediaAsset& asset, const juce::File& folder, unsigned Fs, const Id& track)
{
    if (asset.kind != AssetKind::mic || asset.mediaGeneration < 1 || asset.logicalLength < 0)
        throw std::invalid_argument("Invalid recorded audio asset");
    auto wav = std::make_shared<WavSource>(); wav->trackId = track; wav->sampleRate = Fs;
    wav->epoch = std::make_shared<MediaEpoch>(); wav->generation = std::uint64_t(asset.mediaGeneration); wav->epoch->value.store(wav->generation);
    for (const auto& chunk : asset.chunks)
        wav->chunks.push_back({folder.getChildFile(chunk.relativePath), chunk.sourceRange.start, chunk.sourceRange.length, 44, 44 + std::uint64_t(chunk.sourceRange.length) * 3});
    MediaIndex::validateWav(*wav);
    if (wav->length > asset.logicalLength) throw std::invalid_argument("WAV chunks exceed the recorded take");
    wav->length = asset.logicalLength; return wav;
}
void RecorderSession::play(bool latest)
{
    if ((recording() && !(latest && take.state() == TakeController::State::stopping)) || configuring()) return;
    playbackButtonQpc = qpcNow(); firstPlaybackVideoQpc = firstPlaybackAudioQpc = firstPlaybackAudibleQpc = 0;
    wantPlay = true;
    if (latest)
    {
        pendingLatest = true; clearPlayback(); cursor = take.placementSample();
        if (take.state() == TakeController::State::idle && !document.getProject().media->takes.empty()) cursor = document.getProject().media->takes.back().placementSample;
    }
    timeline = true;
    if (playback) { playback->transport.play(); pendingLatest = false; }
    else preparePlayback();
}
void RecorderSession::pause() { wantPlay = false; if (playback) playback->transport.pause(); }
void RecorderSession::stopPlayback()
{ wantPlay = false; if (playback) { playback->transport.scrub(playhead(), true, qpcNow()); playback->transport.stop(); } }
void RecorderSession::goToStart() { wantPlay = false; cursor = 0; if (playback) playback->transport.goToStart(); else preparePlayback(); }
void RecorderSession::scrub(Sample sample, bool released)
{
    if (recording()) return; wantPlay = false;
    cursor = std::clamp(sample, Sample{0}, document.getProject().activeTimelineEnd());
    if (playback) playback->transport.scrub(cursor, released, qpcNow()); else preparePlayback();
}
bool RecorderSession::playing() const
{ if (!playback) return wantPlay; const auto s = playback->transport.snapshot().state; return wantPlay || s == TransportState::playing || s == TransportState::scheduled; }
Sample RecorderSession::playhead() const
{ return playback ? TimelineTransport::audibleCursor(playback->transport.snapshot(), device.sampleRate, qpcFrequency(), qpcNow()) : cursor; }
Sample RecorderSession::elapsed() const
{ return recording() && !configuring() && audio.startSample() >= 0 ? std::max(Sample{0}, audio.acceptedEnd() - audio.startSample()) : take.logicalLength(); }
void RecorderSession::setMonitoring(std::uint8_t mask) { audio.setInputMonitoring(mask != 0, mask); }
void RecorderSession::updateMicrophoneSettings(const UserSettings& settings)
{ current.microphoneNames = settings.microphoneNames; current.microphoneArmed = settings.microphoneArmed; }
void RecorderSession::addMarker()
{
    Marker m; m.name = k("마커"); m.sample = recording() ? take.placementSample() + elapsed() : playhead();
    if (recording()) recordedMarkers.push_back(m);
    else document.addMarker(std::move(m));
}
void RecorderSession::refreshPlaybackPlan()
{ const bool resume = playing(); clearPlayback(); wantPlay = resume; if (timeline) preparePlayback(); }
void RecorderSession::projectChanged()
{
    clearPlayback(); take.reset(); cursor = 0; wantPlay = false; pendingLatest = false; peaksPublished.clear(); error.clear(); notice.clear();
    if (planWork.valid()) planWork.wait(); // project replacement is disabled while plan preparation is pending
    videoIndexes.clear(); wavIndexes.clear(); derivedKeys.clear(); derivedProject = document.getProject().projectId;
    scheduleDerived();
}
void RecorderSession::scheduleDerived()
{
    if (document.getFile() == juce::File() || recording()) return;
    const auto p = document.snapshot(); const auto folder = document.getFile().getParentDirectory();
    for (const auto& takeItem : p->media->takes)
    {
        const auto key = p->projectId + "/peaks/" + takeItem.takeId;
        if (derivedKeys.count(key)) continue;
        const auto accepted = derivedWorker.enqueue(key, [this, takeItem, folder](const auto& yield)
        {
            if (yield()) return;
            const auto file = folder.getChildFile("cache/" + takeItem.takeId + ".peaks.json");
            if (!file.existsAsFile()) return;
            const auto peaks = PeakCache::read(file);
            const std::lock_guard<std::mutex> lock(derivedMutex);
            for (unsigned i = 0; i < takeItem.microphoneAssetIds.size(); ++i)
                derivedResults.push_back({takeItem.microphoneAssetIds[i], {}, peaks, i});
        });
        if (accepted) derivedKeys.insert(key);
    }
    for (const auto& asset : p->media->assets) if (asset.kind == AssetKind::camera && !asset.relativePath.contains(".recording.") && !asset.availableRanges.empty())
    {
        const auto key = p->projectId + "/thumb/" + asset.assetId + "/" + juce::String(asset.mediaGeneration);
        if (derivedKeys.count(key)) continue;
        const auto accepted = derivedWorker.enqueue(key, [this, asset, folder, rate = p->Fs](const auto& yield)
        {
            auto frames = ThumbnailCache::decode(folder.getChildFile(asset.relativePath), rate, yield);
            if (!frames.empty()) { const std::lock_guard<std::mutex> lock(derivedMutex); derivedResults.push_back({asset.assetId, std::move(frames), {}, 0}); }
        });
        if (accepted) derivedKeys.insert(key);
    }
}
void RecorderSession::tick()
{
    if (ready(deviceWork))
    {
        auto result = deviceWork.get(); current = result.settings; device = audio.deviceInfo();
        notice.clear(); if (result.result.failed()) error = result.result.getErrorMessage();
        if (onConfigured) onConfigured(result.result, current);
    }
    if (configuring()) return;
    take.tick();
    if (take.warning().isNotEmpty()) notice = take.warning();
    if (recording() && audio.error() == RecorderAudioEngine::Error::writeFailed) error = k("저장 장치에 쓸 수 없어 녹화를 멈췄습니다.");
    if (autoStart && take.state() == TakeController::State::armed)
    { const auto r = take.start(); autoStart = false; if (r.failed()) error = r.getErrorMessage(); }
    if (take.state() == TakeController::State::partialFailure) autoStart = false;
    const auto& meta = take.placementMetadata();
    if (meta.ready && peaksPublished.isEmpty())
    {
        const auto& p = document.getProject();
        if (!p.media->takes.empty())
        {
            const auto& t = p.media->takes.back(); peaksPublished = t.takeId;
            for (unsigned i = 0; i < t.microphoneAssetIds.size(); ++i) if (onPeaks && meta.waveform) onPeaks(t.microphoneAssetIds[i], meta.waveform, i);
            document.performEdit(k("녹화 마이크 이름과 마커"), [this](EditState& e)
            {
                for (auto& track : e.tracks) if (track.kind == TrackKind::mic && track.microphoneIndex >= 0 && track.microphoneIndex < 8)
                {
                    const auto& name = current.microphoneNames[unsigned(track.microphoneIndex)];
                    if (name.isNotEmpty()) track.name = name;
                }
                e.markers.insert(e.markers.end(), recordedMarkers.begin(), recordedMarkers.end());
            }); recordedMarkers.clear();
        }
    }
    derivedWorker.setRecording(recording());
    for (unsigned i = 0; i < 2; ++i) if (cameras[i] && !cameras[i]->failed)
    {
        if (cameras[i]->capture->failureDetected() || cameras[i]->capture->finished())
        {
            take.cameraFailed(i, cameras[i]->capture->generation()); cameras[i]->failed = true;
            if (i == 0) error = k("캠1 연결이 끊겼습니다. 원본 녹음과 연결된 캠2는 계속됩니다.");
            else error = k("캠2 연결이 끊겼습니다. 캠1과 원본 녹음은 계속됩니다.");
            cameras[i]->presenter.reset();
        }
        else if (cameras[i]->presenter && cameras[i]->presenter->finished())
        {
            cameras[i]->presenter->stop(); error = k("영상 표시 장치 연결을 확인하세요. ") + juce::String(cameras[i]->presenter->error());
            cameras[i]->presenter.reset(); cameras[i]->previewFailed = true;
        }
    }
    if (take.state() == TakeController::State::partialFailure && error.isEmpty())
        error = audio.error() == RecorderAudioEngine::Error::writeFailed ? k("저장 장치에 쓸 수 없어 녹화를 멈췄습니다.")
            : audio.error() != RecorderAudioEngine::Error::none ? k("오디오 장치 오류로 녹화를 멈췄습니다.") : k("일반 MP4 마무리 실패 · 재시도");
    if (ready(planWork))
    {
        auto plan = planWork.get();
        if (plan->project == document.getProject().projectId && plan->revision == document.getProject().editRevision && timeline && !recording())
        {
            if (plan->error.isNotEmpty()) { error = plan->error; wantPlay = false; }
            else try
            {
                for (auto& cam : cameras) if (cam) cam->presenter.reset();
                playback = std::make_unique<Playback>(device.sampleRate, device.bufferFrames, plan->end, audio);
                playback->renderer.setPlan(std::move(plan->tracks), plan->end); playback->video.prepare(std::move(plan->videos));
                for (unsigned i = 0; i < 2; ++i) if (hosts[i]) playback->video.attachPlaybackView(i, hosts[i]);
                playback->output.start(playback->transport); playback->transport.seek(std::clamp(cursor, Sample{0}, plan->end));
                if (wantPlay) playback->transport.play(); pendingLatest = false; notice.clear();
            }
            catch (const std::exception& e) { clearPlayback(); error = k("재생 준비를 완료할 수 없습니다. ") + juce::String::fromUTF8(e.what()); wantPlay = false; }
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
    for (auto& item : completed) if (document.getProject().media->findAsset(item.asset))
    {
        if (!item.thumbs.empty() && onThumbnails) onThumbnails(item.asset, std::move(item.thumbs));
        if (item.peaks.sampleRate && onLoadedPeaks) onLoadedPeaks(item.asset, std::move(item.peaks), item.channel);
    }
}
}
