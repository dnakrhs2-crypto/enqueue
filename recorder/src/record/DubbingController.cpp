#include "DubbingController.h"
#include "capture/MfCameraCapture.h"
#include "playback/ImportedAudioCache.h"
#include "playback/TimelineTransport.h"
#include "sync/ClockMath.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <thread>

namespace gocue::recorder
{
namespace
{
void check(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
void need(bool b, const char* s) { if (!b) throw std::invalid_argument(s); }
void briefWait() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
template<class Mapper> auto preparedClock(const Mapper& clock) -> decltype(clock.snapshot())
{
    // AtomicSnapshot intentionally returns unavailable when publication overlaps
    // all three RT read attempts. This is a worker/control thread, so retry the
    // current publication for at most 8ms instead of using an old epoch snapshot.
    for (unsigned i = 0; i < 8; ++i) { if (auto s = clock.snapshot()) return s; briefWait(); }
    return {};
}
Sample sum(Sample a, Sample b)
{ const auto s = clock_math::add(a, b); if (!s) throw std::overflow_error("Dubbing sample overflow"); return *s; }
void writeDurable(const juce::File& f, const juce::var& value)
{
    CaptureTelemetry::writeJson(f, value);
    DurableFile file; check(file.open(f, DurableFile::OpenMode::appendOrCreate)); check(file.flushData()); check(file.close());
}
class SelectedAudio final : public IPlaybackBlockProvider
{
public:
    SelectedAudio(const RecorderProject& project, const juce::File& directory, const Id& track)
    {
        plan = RenderPlanCompiler::compile(project);
        const auto lane = std::find_if(plan->tracks.begin(), plan->tracks.end(), [&](const auto& t) { return t.trackId == track; });
        need(lane != plan->tracks.end() && lane->kind == TrackKind::importAudio, "더빙할 완성 오디오 트랙을 선택하세요.");
        spans = lane->spans; rate = project.Fs;
        for (const auto& fade : plan->microfadeBoundaries) if (fade.trackId == track) fades.push_back(fade);
        AudioImportControl control;
        for (const auto& span : spans) if (!span.isGap() && !readers.count(span.assetId))
        {
            const auto& asset = *project.media->findAsset(span.assetId); CachedImportedAudio cache;
            check(ImportedAudioCache::build(directory, asset, AudioImport::loadInfo(directory, asset), rate, control, cache));
            juce::WavAudioFormat wav; auto reader = std::unique_ptr<juce::AudioFormatReader>(wav.createReaderFor(cache.pcmFile.createInputStream().release(), true));
            need(reader && reader->sampleRate == rate && reader->numChannels >= 1 && reader->numChannels <= 2, "완성 오디오 PCM 캐시를 열 수 없습니다.");
            readers.emplace(span.assetId, std::move(reader));
        }
    }
    void render(float* stereo, unsigned frames, std::int64_t first, unsigned Fs) override
    {
        need(Fs == rate && first >= 0, "더빙 오디오 시간 기준이 바뀌었습니다."); const auto end = sum(first, frames);
        std::fill_n(stereo, size_t(frames) * 2, 0.0f);
        scratch.setSize(2, int(frames), false, false, true);
        gains.assign(frames,1.0f);
        for (const auto& fade : fades)
        {
            for (auto at = (std::max)(first,fade.timelineSample - fade.beforeSamples); at < (std::min)(end,fade.timelineSample); ++at)
                gains[size_t(at - first)] = (std::min)(gains[size_t(at - first)],float(fade.timelineSample - at) / float(fade.beforeSamples));
            for (auto at = (std::max)(first,fade.timelineSample); at < (std::min)(end,fade.timelineSample + fade.afterSamples); ++at)
                gains[size_t(at - first)] = (std::min)(gains[size_t(at - first)],float(at - fade.timelineSample + 1) / float(fade.afterSamples));
        }
        for (const auto& span : spans)
        {
            const auto a = (std::max)(first, span.timeline.start), b = (std::min)(end, span.timeline.start + span.timeline.length);
            if (span.isGap() || b <= a) continue;
            auto& reader = *readers.at(span.assetId); const auto n = int(b - a); scratch.clear();
            float* channels[] {scratch.getWritePointer(0), scratch.getWritePointer(1)};
            need(reader.read(channels, 2, span.sourceIn + a - span.timeline.start, n), "더빙 오디오 PCM 읽기 실패");
            for (int i = 0; i < n; ++i)
            {
                const auto at = a + i; const auto gain = gains[size_t(at - first)];
                const auto offset = size_t(at - first) * 2;
                stereo[offset] = channels[0][i] * gain;
                stereo[offset + 1] = channels[reader.numChannels == 1 ? 0 : 1][i] * gain;
            }
        }
    }
private:
    unsigned rate = 0;
    std::shared_ptr<const CompiledRenderPlan> plan;
    std::vector<RenderSpan> spans;
    std::vector<MicrofadeBoundary> fades;
    std::vector<float> gains;
    std::map<Id, std::unique_ptr<juce::AudioFormatReader>> readers;
    juce::AudioBuffer<float> scratch;
};
struct CameraOrigin
{
    std::atomic<Sample> sample{-1};
    std::uint64_t masterEpoch = 0, cameraEpoch = 0; // published by video start release
};
class DubCameraTime final : public CameraTimeMapper
{
public:
    DubCameraTime(CameraClockMapper& c, CameraOrigin& o, unsigned fs) : camera(c), origin(o), rate(fs) {}
    std::int64_t map(const FrameStamp& stamp) override
    { return read([&](CameraSampleTimeMapper& mapper) { return mapper.map(stamp); }); }
    std::int64_t now(std::int64_t qpc) const override
    { return read([&](const CameraSampleTimeMapper& mapper) { return mapper.now(qpc); }); }
private:
    template<class F> Sample read(F f) const
    {
        for (unsigned i = 0;; ++i)
        {
            try { CameraSampleTimeMapper mapper(camera, origin.sample.load(), rate, origin.masterEpoch, origin.cameraEpoch); return f(mapper); }
            catch (...)
            {
                const auto a = camera.masterClock().snapshot(); const auto v = camera.snapshot();
                if (i == 7 || (a && a->epoch != origin.masterEpoch) || (v && v->epoch != origin.cameraEpoch)) throw;
                briefWait();
            }
        }
    }
    CameraClockMapper& camera; CameraOrigin& origin; unsigned rate;
};
}
struct DubbingController::Impl final : IAudioOutputClient
{
    RecorderDocument& document; RecorderAudioEngine& audio; TimelineTransport* transport; VideoFactory factory;
    AudioFactory audioFactory;
    Config config; Placement placed; RecorderDocument::Snapshot frozen;
    State current = State::idle; std::atomic<Failure> failureCode{Failure::none}; juce::String message;
    RecorderAudioEngine::DeviceInfo device; std::vector<JournalDeviceMapping> micMap;
    std::vector<int> micLanes; Take take; std::vector<MediaAsset> assets;
    std::array<std::unique_ptr<CameraClockMapper>, 2> cameras;
    std::array<CameraOrigin, 2> origins;
    std::array<std::unique_ptr<ITakeVideoStream>, 2> videos;
    std::array<std::unique_ptr<MfCameraCapture>, 2> captures;
    std::array<std::shared_ptr<CaptureTelemetry>, 2> captureStats;
    std::array<std::shared_ptr<VideoSurfacePool>, 2> previews;
    std::array<std::atomic<ITakeVideoStream*>, 2> sinks{};
    std::array<std::uint64_t, 2> discontinuities{}, typeChanges{}; // sole camera producer
    std::array<std::atomic<unsigned>, 2> inFlight{};
    std::array<std::atomic<std::uint64_t>, 2> receivedFrames{};
    std::array<std::atomic<Sample>, 2> lastCaptureSample{};
    std::array<FrameStamp, 2> lastFrame;
    std::array<std::optional<ClockSnapshot>, 2> lastMaster;
    std::array<std::optional<CameraClockSnapshot>, 2> lastCamera;
    std::unique_ptr<MfRuntime> runtime;
    std::unique_ptr<PlaybackPcmQueue> pcm;
    std::unique_ptr<IPlaybackBlockProvider> outputSource;
    std::thread renderWorker;
    std::atomic<bool> renderStop{false}, renderReady{false}, scheduled{false};
    std::atomic<Sample> submissionStop{-1}, submittedThrough{-1};
    Sample submitted = 0; // RT only, initialized before scheduled release
    std::uint64_t underruns = 0; // RT, read after detach
    BlockStamp previous{}; bool havePrevious = false;
    std::future<juce::Result> work; std::atomic<bool> preparedAudio{false}; bool saving = false;
    bool ownsLocks = false;
    juce::Uuid editId; std::int64_t prepareQpc = 0, stopQpc = 0;
    double placementMs = 0;
    juce::var audioReport; std::array<juce::var, 2> videoReports;
    RecorderDocument::Snapshot saved;
    Impl(RecorderDocument& d, RecorderAudioEngine& a, TimelineTransport* t, VideoFactory f, AudioFactory af)
        : document(d), audio(a), transport(t), factory(std::move(f)), audioFactory(std::move(af))
    {
        if (!factory) factory = [](unsigned i, std::unique_ptr<CameraTimeMapper> m) { return TakeController::createVideoStream(std::move(m), "cam" + juce::String(i + 1)); };
        if (!audioFactory) audioFactory = DubbingController::prepareReferenceAudio;
    }
    ~Impl()
    {
        if (work.valid()) work.wait();
        if (ownsLocks && preparedAudio) audio.abort(RecorderAudioEngine::Error::cancelled);
        if (ownsLocks) audio.setDubbingOutputClient(nullptr); renderStop = true;
        if (renderWorker.joinable()) renderWorker.join();
        for (auto& capture : captures) if (capture) capture->stop(); detachVideo();
        if (preparedAudio)
        {
            audio.abort(RecorderAudioEngine::Error::cancelled); audio.finishCapture(editId);
            for (auto& video : videos) if (video) { video->endAt((std::max)(Sample{0}, audio.acceptedEnd() - placed.O0)); video->audioDone(); video->finish(); }
            audio.finishJournal(false);
        }
        if (ownsLocks) { document.setRecordingStructureLock(false); if (transport) transport->setDubbingLocked(false); }
    }
    juce::File folder() const { return config.projectDirectory.getChildFile("media/takes/" + config.takeId.toDashedString()); }
    void detachVideo()
    {
        for (auto& sink : sinks) sink.store(nullptr);
        for (auto& count : inFlight) while (count.load()) briefWait();
    }
    void fail(Failure why) noexcept
    {
        auto none = Failure::none; failureCode.compare_exchange_strong(none, why);
        if (preparedAudio) audio.abort(why == Failure::asioReset ? RecorderAudioEngine::Error::asioReset
            : why == Failure::cancelled ? RecorderAudioEngine::Error::cancelled : RecorderAudioEngine::Error::clockDiscontinuity);
    }
    void processOutput(const BlockStamp& stamp, float* l, float* r) noexcept override
    {
        std::fill_n(l, stamp.numSamples, 0.0f); std::fill_n(r, stamp.numSamples, 0.0f);
        if (!scheduled.load(std::memory_order_acquire)) return;
        if (failureCode.load() != Failure::none || audio.error() != RecorderAudioEngine::Error::none) return;
        const bool reset = havePrevious && (stamp.resets != previous.resets || stamp.resyncs != previous.resyncs);
        const bool discontinuity = stamp.sampleRate != device.sampleRate || stamp.numSamples != device.bufferFrames
            || (havePrevious && (stamp.samplePosition != previous.samplePosition + previous.numSamples
                || stamp.callbackQpc <= previous.callbackQpc || stamp.xruns != previous.xruns || stamp.latencyChanges != previous.latencyChanges));
        if (reset || discontinuity) { fail(reset ? Failure::asioReset : Failure::clockDiscontinuity); return; }
        previous = stamp; havePrevious = true;
        const auto first = (std::max)(stamp.samplePosition, placed.outputSubmissionSample);
        const auto end = (std::min)(stamp.samplePosition + stamp.numSamples, submissionStop.load());
        if (end <= first) return;
        if (!audio.startCommitted() || first != placed.outputSubmissionSample + submitted)
        { fail(Failure::clockDiscontinuity); return; }
        const auto n = unsigned(end - first), offset = unsigned(first - stamp.samplePosition);
        if (!pcm->consume(config.Pstart + submitted, 1, l + offset, r + offset, n))
        { ++underruns; fail(Failure::playbackUnderrun); return; }
        submitted += n; submittedThrough.store(end, std::memory_order_release);
    }
    void offer(unsigned i, const VideoSurface& frame) noexcept
    {
        if (i >= 2) return; inFlight[i].fetch_add(1);
        if (auto* video = sinks[i].load())
        {
            try
            {
                if (captureStats[i])
                {
                    const auto d = captureStats[i]->count(LossReason::sourceDiscontinuity), t = captureStats[i]->count(LossReason::sourceTypeChanged);
                    if (d != discontinuities[i] || t != typeChanges[i]) cameras[i]->reset();
                    discontinuities[i] = d; typeChanges[i] = t;
                }
                cameras[i]->observe(frame.stamp);
                ++receivedFrames[i];
                const auto master = audio.masterClock().snapshot(); const auto camera = cameras[i]->snapshot();
                if (master && camera) if (const auto mapped = cameras[i]->captureSample(frame.stamp, *master))
                { lastCaptureSample[i] = mapped->sample; lastFrame[i] = frame.stamp; lastMaster[i] = master; lastCamera[i] = camera; }
                const auto snapshot = cameras[i]->snapshot();
                if (scheduled.load() && snapshot && snapshot->epoch != origins[i].cameraEpoch)
                    video->sourceFailed((std::max)(Sample{0}, audio.acceptedEnd() - placed.O0));
                else video->offer(frame);
            }
            catch (...) { video->sourceFailed((std::max)(Sample{0}, audio.acceptedEnd() - placed.O0)); }
        }
        inFlight[i].fetch_sub(1);
    }
    void render()
    {
        try
        {
            const auto block = device.bufferFrames; std::vector<float> stereo(size_t(block) * 2), l(block), r(block);
            Sample at = config.Pstart; const auto end = sum(config.Pstart, placed.spanSamples);
            const auto readyLength = (std::min)(placed.spanSamples, (std::max)(Sample(block), Sample(device.sampleRate / 4)));
            while (!renderStop.load() && at < end)
            {
                const auto n = unsigned((std::min)(Sample(block), end - at)); outputSource->render(stereo.data(), n, at, device.sampleRate);
                for (unsigned i = 0; i < n; ++i) { l[i] = stereo[i * 2]; r[i] = stereo[i * 2 + 1]; }
                while (!renderStop.load() && !pcm->push(at, 1, l.data(), r.data(), n)) briefWait();
                if (renderStop.load()) return; at += n;
                if (at - config.Pstart >= readyLength) renderReady.store(true, std::memory_order_release);
            }
        }
        catch (...) { fail(Failure::preparation); }
    }
    juce::Result prepareWorker()
    {
        try
        {
            check(folder().getChildFile("index").createDirectory());
            outputSource = audioFactory(*frozen, config.projectDirectory, config.audioTrackId);
            auto reference = audioFactory(*frozen, config.projectDirectory, config.audioTrackId);
            for (unsigned i = 0; i < config.cameras.size(); ++i)
                videos[i] = factory(i, std::make_unique<DubCameraTime>(*cameras[i], origins[i], device.sampleRate));
            RecorderAudioEngine::TakeConfig ac; ac.projectDirectory = config.projectDirectory; ac.takeId = config.takeId;
            ac.placementSample = config.Pstart; ac.microphoneAssetIds = take.microphoneAssetIds;
            for (unsigned i = 0; i < config.cameras.size(); ++i)
                ac.additionalFiles.push_back({assets[i].assetId, assets[i].relativePath,
                    "media/takes/" + config.takeId.toDashedString() + "/cam" + juce::String(i + 1) + ".mp4"});
            ac.referencePackets = [this](const AVPacket& p)
            {
                for (unsigned i = 0; i < config.cameras.size(); ++i)
                    try { if (!videos[i]->failed()) videos[i]->audioPacket(p); }
                    catch (...) { videos[i]->sourceFailed((std::max)(Sample{0}, audio.acceptedEnd() - placed.O0)); }
            };
            check(audio.prepareDubbing(std::move(ac), config.recordMicrophones, std::move(reference), config.Pstart,
                                      config.cameras[0].calibration.inputResidualLatencySamples)); preparedAudio = true;
            for (unsigned i = 0; i < config.cameras.size(); ++i)
            {
                videos[i]->prepare(folder().getChildFile("cam" + juce::String(i + 1) + ".mp4"), NvencProfile{int(frozen->fps.numerator)}, config.cameras[i].mode.fps, *audio.referenceContext());
                sinks[i].store(videos[i].get());
                if (!config.synthetic)
                {
                    if (!runtime) runtime = std::make_unique<MfRuntime>();
                    captureStats[i] = std::make_shared<CaptureTelemetry>(config.cameras[i].mode.fps);
                    captures[i] = std::make_unique<MfCameraCapture>(captureStats[i], *previews[i], [this, i](const auto& f) { offer(i, f); });
                    captures[i]->start(config.cameras[i].symbolicLink, config.cameras[i].mode, config.cameras[i].mode.subtype == CaptureSubtype::mjpeg);
                }
            }
            renderWorker = std::thread([this] { render(); }); writeDurable(folder().getChildFile("take.json"), manifest());
            return juce::Result::ok();
        }
        catch (const std::exception& e)
        {
            fail(Failure::preparation); detachVideo();
            for (auto& capture : captures) if (capture) capture->stop();
            if (preparedAudio)
            {
                audio.finishCapture(editId);
                for (auto& video : videos) if (video) { video->endAt(0); video->audioDone(); video->finish(); }
                audio.finishJournal(false); preparedAudio = false;
            }
            return juce::Result::fail(juce::String::fromUTF8(e.what()));
        }
    }
    void ranges(MediaAsset& a, Sample available)
    {
        a.logicalLength = placed.recordedSamples; available = (std::clamp)(available, Sample{0}, placed.recordedSamples);
        a.availableRanges.clear(); a.gaps.clear();
        if (available) a.availableRanges.push_back({0, available});
        if (available < a.logicalLength) a.gaps.push_back({available, a.logicalLength - available});
    }
    void chunks(MediaAsset& asset, unsigned mic, Sample available)
    {
        asset.chunks.clear(); const auto step = Sample(device.sampleRate) * WavTrackWriter::chunkSeconds;
        for (Sample at = 0; at < available; at += step)
            asset.chunks.push_back({WavTrackWriter::chunkPath(config.takeId, mic, std::uint64_t(at / step) + 1), {at, (std::min)(step, available - at)}});
        asset.relativePath = asset.chunks.empty() ? WavTrackWriter::chunkPath(config.takeId, mic, 1) : juce::String{};
    }
    void placeStopped()
    {
        if (!stopQpc) stopQpc = qpcNow();
        placed.Ostop = audio.stopSample(); placed.recordedSamples = placed.Ostop >= placed.O0 && placed.O0 >= 0 ? placed.Ostop - placed.O0 : 0;
        if (config.retakeStack.isEmpty()) placed.spanSamples = placed.recordedSamples;
        if (placed.recordedSamples > 0)
        {
            take.N0 = take.O0 = placed.O0; take.logicalLength = placed.recordedSamples; take.state = TakeState::finalising;
            for (auto& a : assets) ranges(a, placed.recordedSamples);
            for (size_t i = 0; i < micLanes.size(); ++i) chunks(assets[config.cameras.size() + i], unsigned(micLanes[i] + 1), placed.recordedSamples);
            const auto result = document.placeDubbingTake(take, assets, micLanes, {config.Pstart, placed.spanSamples}, config.retakeStack);
            if (result.failed()) { message = result.getErrorMessage(); fail(Failure::storage); }
            else
            {
                editId = juce::Uuid(document.lastEditTransaction());
                for (const auto& t : document.getProject().tracks) for (const auto& c : t.clips.items()) if (c.assetId == take.cam1AssetId)
                { placed.stackId = c.takeStackId; placed.versionId = c.versionId; }
            }
        }
        else fail(Failure::clockDiscontinuity);
        placementMs = 1000.0 * double(qpcNow() - stopQpc) / double(qpcFrequency());
        current = State::finalizing;
        work = std::async(std::launch::async, [this]
        {
            try
            {
                const auto result = audio.finishCapture(editId); if (result.failed() && failureCode == Failure::none) fail(Failure::storage);
                audioReport = audio.telemetry();
                double period = 0; for (const auto& c : config.cameras) period = (std::max)(period, c.mode.fps.periodMs());
                const auto deadline = qpcNow() + std::int64_t(period * double(qpcFrequency()) / 1000);
                while (qpcNow() < deadline) briefWait(); detachVideo();
                for (unsigned i = 0; i < config.cameras.size(); ++i)
                {
                    videos[i]->endAt(placed.recordedSamples); videos[i]->audioDone(); videos[i]->finish(); videoReports[i] = videos[i]->report();
                    jsonSet(videoReports[i], "clockMapping", "Round-04 CameraClockMapper with common O0 output origin; physical calibration unverified");
                    if (videos[i]->failed()) fail(Failure::storage);
                    const auto path = folder().getChildFile("cam" + juce::String(i + 1) + ".mp4");
                    ranges(assets[i], path.existsAsFile() ? videos[i]->availableSamples() : 0);
                    if (path.existsAsFile()) assets[i].relativePath = path.getRelativePathFrom(config.projectDirectory).replaceCharacter('\\', '/');
                }
                const auto written = Sample(audioReport["wav"]["writtenSamplesPerMic"]);
                for (size_t i = 0; i < micLanes.size(); ++i)
                { auto& asset = assets[config.cameras.size() + i]; ranges(asset, written); chunks(asset, unsigned(micLanes[i] + 1), (std::min)(written, placed.recordedSamples)); }
                for (auto& a : assets) ++a.mediaGeneration;
                return juce::Result::ok();
            }
            catch (const std::exception& e) { fail(Failure::storage); return juce::Result::fail(juce::String::fromUTF8(e.what())); }
        });
    }
    juce::var manifest() const
    {
        auto v = jsonObject(); jsonSet(v, "schemaVersion", 1); jsonSet(v, "mode", "dub");
        jsonSet(v, "takeId", config.takeId.toDashedString()); jsonSet(v, "state", TakeController::stateName(current));
        jsonSet(v, "Pstart", placed.Pstart); jsonSet(v, "placementSample", placed.Pstart); jsonSet(v, "O0", placed.O0); jsonSet(v, "Ostop", placed.Ostop);
        jsonSet(v, "outputSubmissionSample", placed.outputSubmissionSample); jsonSet(v, "recordedSamples", placed.recordedSamples);
        jsonSet(v, "spanSamples", placed.spanSamples); jsonSet(v, "Fs", int(device.sampleRate));
        jsonSet(v, "stackId", placed.stackId); jsonSet(v, "versionId", placed.versionId); jsonSet(v, "referenceAudioTrack", config.audioTrackId);
        jsonSet(v, "recordMicrophones", config.recordMicrophones); jsonSet(v, "failureCode", int(failureCode.load())); jsonSet(v, "error", errorText());
        jsonSet(v, "audio", audioReport); juce::Array<juce::var> video, profiles;
        for (unsigned i = 0; i < config.cameras.size(); ++i)
        {
            auto stream = videoReports[i].isObject() ? videoReports[i] : jsonObject();
            jsonSet(stream, "receivedFrames", jsonInt(receivedFrames[i].load()));
            jsonSet(stream, "lastMappedCaptureSample", lastCaptureSample[i].load());
            video.add(stream); auto p = jsonObject();
            jsonSet(p, "cameraResidualLatency100ns", config.cameras[i].calibration.cameraResidualLatency100ns);
            jsonSet(p, "outputResidualSamples", config.cameras[i].calibration.outputResidualLatencySamples);
            jsonSet(p, "inputResidualSamples", config.cameras[i].calibration.inputResidualLatencySamples); profiles.add(p);
        }
        jsonSet(v, "videos", video); jsonSet(v, "calibration", profiles);
        jsonSet(v, "expectedVideoFrames", TakeController::frameCount(placed.recordedSamples, device.sampleRate, frozen->fps));
        jsonSet(v, "stopToPlacementMs", placementMs);
        jsonSet(v, "clockPolicy", "Round-04 outputOriginSample; Nv=S(q-Lcam); Pvideo=Pstart+(Nv-O0). Driver buffer reference and physical latency remain unverified.");
        jsonSet(v, "referencePolicy", "Selected completed audio track only; mono duplicated, stereo preserved; input WAV is native input only.");
        return v;
    }
    juce::String errorText() const
    {
        if (message.isNotEmpty()) return message;
        switch (failureCode.load())
        {
            case Failure::none: return {};
            case Failure::playbackUnderrun: return juce::String::fromUTF8("완성 오디오 재생 데이터가 부족해 더빙을 중단했습니다.");
            case Failure::asioReset: return juce::String::fromUTF8("ASIO 장치가 재설정되어 더빙을 중단했습니다.");
            case Failure::clockDiscontinuity: return juce::String::fromUTF8("클록이 끊겨 더빙을 중단했습니다.");
            case Failure::preparation: return juce::String::fromUTF8("더빙 오디오 또는 카메라 준비에 실패했습니다.");
            case Failure::storage: return juce::String::fromUTF8("일부 영상 또는 오디오 저장에 실패했습니다. 저장된 구간을 확인하세요.");
            case Failure::cancelled: return juce::String::fromUTF8("더빙을 취소했습니다.");
        }
        return {};
    }
    void release()
    {
        scheduled = false; if (ownsLocks) audio.setDubbingOutputClient(nullptr); renderStop = true;
        if (renderWorker.joinable()) renderWorker.join();
        if (ownsLocks) { document.setRecordingStructureLock(false); if (transport) transport->setDubbingLocked(false); ownsLocks = false; }
    }
};
DubbingController::DubbingController(RecorderDocument& d, RecorderAudioEngine& a, TimelineTransport* t, VideoFactory f, AudioFactory af)
    : impl(std::make_unique<Impl>(d, a, t, std::move(f), std::move(af))) {}
DubbingController::~DubbingController() = default;
std::unique_ptr<IPlaybackBlockProvider> DubbingController::prepareReferenceAudio(const RecorderProject& p, const juce::File& dir, const Id& track)
{ return std::make_unique<SelectedAudio>(p, dir, track); }
juce::Result DubbingController::prepare(Config c)
{
    auto& s = *impl;
    if (locked() || s.work.valid()) return juce::Result::fail(juce::String::fromUTF8("더빙 작업이 진행 중입니다."));
    try
    {
        need(!s.document.isRecordingStructureLocked(), "다른 녹화가 진행 중입니다.");
        const auto& p = s.document.getProject(); check(p.validate()); const auto device = s.audio.deviceInfo();
        need(device.sampleRate == p.Fs && device.bufferFrames && c.projectDirectory != juce::File() && !c.takeId.isNull()
            && c.Pstart >= 0 && c.cameras.size() >= 1 && c.cameras.size() <= 2, "더빙 장치·프로젝트·카메라 설정을 확인하세요.");
        Sample end = 0; bool found = false;
        for (const auto& t : p.tracks) if (t.trackId == c.audioTrackId && t.kind == TrackKind::importAudio)
        { found = true; for (const auto& clip : t.clips.items()) if (p.isActive(clip)) end = (std::max)(end, clip.timelineEnd()); }
        need(found && c.Pstart < end, "시작 위치에 재생할 완성 오디오 구간이 없습니다.");
        need(!c.recordMicrophones || !s.audio.armedMicrophones().empty(), "마이크도 녹음하려면 입력을 선택하고 녹음을 준비하세요.");
        if (c.retakeStack.isNotEmpty())
        {
            const auto* stack = TakeStackEdits::find(p, c.retakeStack);
            need(stack && c.Pstart == stack->anchorSample && c.spanSamples == stack->spanSamples, "리테이크 구간이 바뀌었습니다.");
        }
        const auto span = c.spanSamples > 0 ? c.spanSamples : end - c.Pstart;
        need(span > 0 && sum(c.Pstart, span) <= end, "더빙 끝이 선택한 오디오 끝을 벗어납니다.");
        for (const auto& cam : c.cameras)
        {
            need(std::abs(double(cam.calibration.cameraResidualLatency100ns)) <= 100000000
                && std::abs(double(cam.calibration.outputResidualLatencySamples)) <= double(device.sampleRate) * 10,
                "카메라 또는 출력 보정 값이 허용 범위를 벗어났습니다.");
            need(cam.mode.width == 1920 && cam.mode.height == 1080 && cam.mode.fps.numerator && cam.mode.fps.denominator
                 && (c.synthetic || !cam.symbolicLink.empty()), "1080p 카메라 native 모드를 선택하세요.");
            need(cam.calibration.outputResidualLatencySamples == c.cameras[0].calibration.outputResidualLatencySamples
                && cam.calibration.inputResidualLatencySamples == c.cameras[0].calibration.inputResidualLatencySamples,
                "두 카메라의 ASIO 입출력 보정 값이 다릅니다.");
        }
        for (auto& cap : s.captures) if (cap) { cap->stop(); cap.reset(); } s.detachVideo();
        for (auto& video : s.videos) video.reset();
        s.device = device; s.config = std::move(c); s.frozen = s.document.snapshot(); s.placed = {}; s.placed.Pstart = s.config.Pstart; s.placed.spanSamples = span;
        s.failureCode = Failure::none; s.message.clear(); s.preparedAudio = false; s.saving = false; s.stopQpc = 0; s.placementMs = 0;
        s.renderStop = false; s.renderReady = false; s.scheduled = false; s.havePrevious = false; s.underruns = 0;
        s.submissionStop = -1; s.submittedThrough = -1; s.submitted = 0; s.audioReport = juce::var(); s.videoReports = {};
        s.pcm = std::make_unique<PlaybackPcmQueue>(device.sampleRate, device.bufferFrames);
        s.take = {}; s.take.takeId = s.config.takeId.toString(); s.take.mode = TakeMode::dub; s.take.placementSample = s.config.Pstart;
        s.assets.clear(); s.micMap = s.config.recordMicrophones ? s.audio.microphoneMapping() : std::vector<JournalDeviceMapping>{}; s.micLanes.clear();
        for (unsigned i = 0; i < s.config.cameras.size(); ++i)
        {
            const auto& cam = s.config.cameras[i];
            s.cameras[i] = std::make_unique<CameraClockMapper>(s.audio.masterClock(), qpcFrequency(), cam.mode.fps, cam.calibration.cameraResidualLatency100ns);
            s.origins[i].sample = -1; s.previews[i] = std::make_shared<VideoSurfacePool>(1920, 1080);
            s.receivedFrames[i] = 0; s.lastCaptureSample[i] = 0;
            s.discontinuities[i] = s.typeChanges[i] = 0; s.captureStats[i].reset();
            s.lastMaster[i].reset(); s.lastCamera[i].reset();
            MediaAsset a; a.kind = AssetKind::camera; a.contentIdentity = a.assetId;
            a.relativePath = "media/takes/" + s.config.takeId.toDashedString() + "/cam" + juce::String(i + 1) + ".recording.mp4";
            a.originalFormat.codec = "h264"; a.originalFormat.width = 1920; a.originalFormat.height = 1080; a.originalFormat.fps = p.fps;
            a.sourceUnitsNumerator = p.fps.numerator; a.sourceUnitsDenominator = std::uint64_t(device.sampleRate) * p.fps.denominator;
            if (i == 0) s.take.cam1AssetId = a.assetId; else s.take.cam2AssetId = a.assetId; s.assets.push_back(a);
            s.take.capture.cameraDeviceIds[i] = cam.symbolicLink; s.take.capture.cameraModes[i] = cam.mode.text();
            s.take.capture.cameraOffsetSamples[i] = rescaleRound(cam.calibration.cameraResidualLatency100ns, device.sampleRate, 10000000);
        }
        for (const auto& m : s.micMap)
        {
            MediaAsset a; a.kind = AssetKind::mic; a.contentIdentity = a.assetId; a.originalFormat.codec = "pcm_s24le";
            a.originalFormat.sampleRate = device.sampleRate; a.originalFormat.channels = 1; a.originalFormat.bitsPerSample = 24;
            s.micLanes.push_back(m.mic - 1); s.take.microphoneAssetIds.push_back(a.assetId); s.take.capture.physicalInputs.push_back(m.physicalIndex); s.assets.push_back(a);
        }
        s.take.capture.asioDeviceId = device.name; s.take.capture.inputOffsetSamples = device.inputLatency + s.config.cameras[0].calibration.inputResidualLatencySamples;
        s.take.capture.outputOffsetSamples = device.outputLatency + s.config.cameras[0].calibration.outputResidualLatencySamples;
        s.take.capture.calibrationIdentity = "round04-buffer-reference-unverified";
        s.take.capture.calibrationDate = s.config.cameras[0].calibration.measuredUtc;
        s.audio.setInputMonitoring(false); s.audio.setDubbingOutputClient(&s);
        s.document.setRecordingStructureLock(true); if (s.transport) s.transport->setDubbingLocked(true);
        s.ownsLocks = true;
        s.current = State::preparing; s.prepareQpc = qpcNow(); s.work = std::async(std::launch::async, [&s] { return s.prepareWorker(); }); return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
juce::Result DubbingController::start(Sample submit)
{
    auto& s = *impl;
    if (s.current != State::armed || s.scheduled.load()) return juce::Result::fail(juce::String::fromUTF8("더빙 준비가 끝나지 않았습니다."));
    try
    {
        const auto clock = preparedClock(s.audio.masterClock()); need(clock && clock->valid && s.audio.clockReady(), "ASIO 클록이 준비되지 않았습니다.");
        if (submit < 0) submit = sum(s.audio.currentSample(), (std::max)(Sample(s.device.sampleRate / 4), Sample(s.device.bufferFrames) * 4));
        const OutputBufferStamp buffer{submit, s.config.Pstart, s.device.bufferFrames, clock->epoch};
        const auto origin = outputOriginSample(s.config.Pstart, s.device.outputLatency, buffer, s.config.cameras[0].calibration.outputResidualLatencySamples);
        need(origin && *origin >= s.audio.currentSample() && submit >= s.audio.currentSample(), "더빙 시작 원점이 과거이거나 범위를 넘었습니다.");
        for (unsigned i = 0; i < s.config.cameras.size(); ++i)
        {
            const auto camera = preparedClock(*s.cameras[i]); need(camera && camera->valid && s.videos[i]->ready(), "카메라 클록이 준비되지 않았습니다.");
            s.origins[i].sample = *origin; s.origins[i].masterEpoch = clock->epoch; s.origins[i].cameraEpoch = camera->epoch;
        }
        s.placed.O0 = *origin; s.placed.outputSubmissionSample = submit;
        s.submissionStop = sum(submit, s.placed.spanSamples);
        check(s.audio.startAt(*origin)); check(s.audio.stopDubbingAt(sum(*origin, s.placed.spanSamples)));
        for (unsigned i = 0; i < s.config.cameras.size(); ++i)
            s.videos[i]->startAt(s.audio.clockMapping(), *origin, s.device.sampleRate, [&s]
            { return (std::max)(Sample{0}, s.audio.acceptedEnd() - s.placed.O0); });
        s.scheduled.store(true, std::memory_order_release); return juce::Result::ok();
    }
    catch (const std::exception& e) { s.message = juce::String::fromUTF8(e.what()); s.fail(Failure::preparation); return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
juce::Result DubbingController::stop(Sample stopAt)
{
    auto& s = *impl;
    if (!s.scheduled.load() || (s.current != State::recording && s.current != State::armed && s.current != State::stopping)) return juce::Result::fail(juce::String::fromUTF8("진행 중인 더빙이 없습니다."));
    try
    {
        if (stopAt < 0) stopAt = (std::min)(sum(s.placed.O0, s.placed.spanSamples),
            sum(s.audio.currentSample(), (std::max)(Sample(s.device.bufferFrames) * 2, s.placed.O0 - s.placed.outputSubmissionSample + Sample(s.device.bufferFrames))));
        const auto outputStop = sum(s.placed.outputSubmissionSample, stopAt - s.placed.O0);
        need(outputStop >= s.audio.currentSample(), "이미 출력한 구간으로 정지를 되돌릴 수 없습니다.");
        check(s.audio.stopDubbingAt(stopAt)); s.submissionStop = outputStop; s.stopQpc = qpcNow(); s.current = State::stopping;
        return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
juce::Result DubbingController::retake()
{ return retake(impl->config.recordMicrophones); }
juce::Result DubbingController::retake(bool microphones)
{
    auto c = impl->config; const auto* stack = TakeStackEdits::find(impl->document.getProject(), impl->placed.stackId);
    if (!stack || locked()) return juce::Result::fail(juce::String::fromUTF8("다시 녹화할 테이크 구간이 없습니다."));
    c.Pstart = stack->anchorSample; c.spanSamples = stack->spanSamples; c.retakeStack = stack->stackId; c.takeId = juce::Uuid();
    c.recordMicrophones = microphones; return prepare(std::move(c));
}
void DubbingController::tick()
{
    auto& s = *impl;
    if (s.work.valid())
    {
        if (s.work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
        const auto result = s.work.get(); if (result.failed()) { s.message = result.getErrorMessage(); s.fail(Failure::storage); }
        if (s.current == State::preparing && result.failed()) { s.current = State::partialFailure; s.release(); return; }
        if (s.current == State::finalizing)
        {
            if (s.saving)
            {
                if (result.failed() && s.document.getProject().media->findTake(s.take.takeId)) s.document.updateTakeState(s.take.takeId, TakeState::partial);
                s.document.checkpointFinished(s.saved, s.config.projectDirectory.getChildFile("project.recorder"), result);
                s.preparedAudio = false; s.current = s.failureCode == Failure::none ? State::done : State::partialFailure; s.release(); return;
            }
            if (s.document.getProject().media->findTake(s.take.takeId))
            {
                for (const auto& a : s.assets) { const auto update = s.document.updateMediaAsset(a); if (update.failed()) { s.message = update.getErrorMessage(); s.fail(Failure::storage); } }
                const auto update = s.document.updateTakeState(s.take.takeId, s.failureCode == Failure::none ? TakeState::complete : TakeState::partial);
                if (update.failed()) { s.message = update.getErrorMessage(); s.fail(Failure::storage); }
            }
            s.saved = s.document.snapshot(); s.saving = true;
            s.work = std::async(std::launch::async, [&s]
            {
                try
                {
                    auto manifest = s.manifest(); jsonSet(manifest, "state", s.failureCode == Failure::none ? "done" : "partialFailure");
                    writeDurable(s.folder().getChildFile("take.json"), manifest);
                    const auto projectFile = s.config.projectDirectory.getChildFile("project.recorder");
                    check(RecorderSerializer::writeCheckpoint(projectFile, *s.saved));
                    DurableFile durable; check(durable.open(projectFile)); check(durable.flushData()); check(durable.close());
                    check(s.audio.finishJournal(s.failureCode == Failure::none)); return juce::Result::ok();
                }
                catch (const std::exception& e) { s.audio.finishJournal(false); s.fail(Failure::storage); return juce::Result::fail(juce::String::fromUTF8(e.what())); }
            }); return;
        }
    }
    if (!locked()) return;
    s.audio.pollDeviceEvents();
    if (s.scheduled.load())
        if (const auto master = s.audio.masterClock().snapshot(); master && master->epoch != s.origins[0].masterEpoch)
            s.fail(Failure::clockDiscontinuity);
    if (s.audio.error() != RecorderAudioEngine::Error::none)
        s.fail(s.audio.error() == RecorderAudioEngine::Error::asioReset ? Failure::asioReset : Failure::clockDiscontinuity);
    if (s.current == State::preparing)
    {
        const auto master = s.audio.masterClock().snapshot();
        bool ready = s.renderReady.load() && s.audio.clockReady() && master && master->valid;
        for (unsigned i = 0; i < s.config.cameras.size(); ++i)
        { const auto c = s.cameras[i]->snapshot(); ready = ready && s.videos[i] && s.videos[i]->ready() && c && c->valid; }
        if (ready) s.current = State::armed;
        else if (qpcNow() - s.prepareQpc > qpcFrequency() * 30) s.fail(Failure::preparation);
    }
    for (unsigned i = 0; i < s.config.cameras.size(); ++i) if (s.captures[i] && s.captures[i]->finished()) cameraFailed(i);
    if (s.audio.referenceFailed()) s.fail(Failure::storage);
    if (s.current == State::armed && s.audio.startSample() >= 0) s.current = State::recording;
    if (s.failureCode != Failure::none || (s.audio.stopSample() >= 0 && s.submittedThrough.load() >= s.submissionStop.load()))
    {
        // An abort may have arrived before the preparation worker created the
        // audio session. Apply it now as well, before draining an empty take.
        if (s.failureCode != Failure::none) s.fail(s.failureCode.load());
        s.scheduled = false; s.current = State::stopping; s.placeStopped();
    }
}
void DubbingController::abort(Failure why) noexcept { if (locked()) impl->fail(why); }
DubbingController::State DubbingController::state() const noexcept { return impl->current; }
bool DubbingController::locked() const noexcept { return state() != State::idle && state() != State::done && state() != State::partialFailure; }
DubbingController::Failure DubbingController::failure() const noexcept { return impl->failureCode.load(); }
juce::String DubbingController::error() const { return impl->errorText(); }
juce::String DubbingController::statusText() const
{
    switch (state())
    {
        case State::idle: return juce::String::fromUTF8("더빙 대기"); case State::preparing: return juce::String::fromUTF8("더빙 준비 중"); case State::armed: return juce::String::fromUTF8("더빙 준비됨");
        case State::recording: return juce::String::fromUTF8("완성 오디오 재생 · 영상 녹화 중"); case State::stopping: return juce::String::fromUTF8("더빙 정지 중");
        case State::finalizing: return juce::String::fromUTF8("더빙 마무리 중"); case State::done: return juce::String::fromUTF8("더빙 완료"); case State::partialFailure: return juce::String::fromUTF8("더빙 일부 저장 · 동기 실패 확인 필요");
    }
    return {};
}
const DubbingController::Placement& DubbingController::placement() const noexcept { return impl->placed; }
const Id& DubbingController::selectedAudioTrack() const noexcept { return impl->config.audioTrackId; }
void DubbingController::offer(unsigned i, const VideoSurface& f) noexcept { impl->offer(i, f); }
void DubbingController::cameraFailed(unsigned i) noexcept
{ if (i < 2) if (auto* v = impl->sinks[i].load()) v->sourceFailed((std::max)(Sample{0}, impl->audio.acceptedEnd() - impl->placed.O0)); }
std::shared_ptr<VideoSurfacePool> DubbingController::previewPool(unsigned i) const { return i < 2 ? impl->previews[i] : nullptr; }
juce::var DubbingController::report() const
{
    if (locked()) throw std::logic_error("더빙 마무리 후 보고서를 읽으세요.");
    auto report = impl->manifest(); jsonSet(report, "playbackUnderruns", jsonInt(impl->underruns)); return report;
}
juce::var DubbingController::calibrationOffsetReport() const
{
    if (locked()) throw std::logic_error("Read offset report after finalization");
    const auto& s = *impl; auto report = jsonObject(); juce::Array<juce::var> entries; bool passed = true;
    for (unsigned i = 0; i < s.config.cameras.size(); ++i)
    {
        auto entry = jsonObject();
        if (!s.lastMaster[i] || !s.lastCamera[i]) { passed = false; jsonSet(entry,"status","UNAVAILABLE"); entries.add(entry); continue; }
        const auto& master = *s.lastMaster[i]; const auto& camera = *s.lastCamera[i];
        const auto profile = s.config.cameras[i].calibration; auto changed = profile;
        changed.outputResidualLatencySamples += 97; changed.cameraResidualLatency100ns += 10000; // +1ms
        const OutputBufferStamp buffer{s.placed.outputSubmissionSample,s.placed.Pstart,s.device.bufferFrames,master.epoch};
        const auto baseO = outputOriginSample(s.placed.Pstart,s.device.outputLatency,buffer,profile.outputResidualLatencySamples);
        const auto newO = outputOriginSample(s.placed.Pstart,s.device.outputLatency,buffer,changed.outputResidualLatencySamples);
        const auto q = camera.timestampQpc(s.lastFrame[i]);
        const auto projectAt = [&](Sample latency100ns, Sample origin) -> std::optional<Sample>
        {
            if (!q) return {};
            const auto ticks = clock_math::rescale(latency100ns,std::uint64_t(master.qpcFrequency),10000000,clock_math::Rounding::nearest);
            const auto corrected = ticks ? clock_math::subtract(*q,*ticks) : std::nullopt;
            const auto nv = corrected ? master.mapToSample(*corrected) : std::nullopt;
            return nv ? projectVideoSample(s.placed.Pstart,*nv,origin) : std::nullopt;
        };
        const auto base = baseO ? projectAt(profile.cameraResidualLatency100ns,*baseO) : std::nullopt;
        const auto shiftedOutput = newO ? projectAt(profile.cameraResidualLatency100ns,*newO) : std::nullopt;
        const auto shiftedCamera = baseO ? projectAt(changed.cameraResidualLatency100ns,*baseO) : std::nullopt;
        const auto expectedCamera = rescaleRound(10000,s.device.sampleRate,10000000);
        const bool exact = base && shiftedOutput && shiftedCamera && baseO == s.placed.O0
            && *shiftedOutput == *base - 97 && std::abs(*shiftedCamera - *base + expectedCamera) <= 1;
        passed = passed && exact; jsonSet(entry,"status",exact ? "PASS" : "FAIL");
        jsonSet(entry,"anchorSample",s.placed.Pstart); jsonSet(entry,"sourceFrameId",jsonInt(s.lastFrame[i].frame));
        if (base) jsonSet(entry,"basePvideo",*base); if (shiftedOutput) jsonSet(entry,"outputResidualPlus97Pvideo",*shiftedOutput);
        if (shiftedCamera) jsonSet(entry,"cameraDelayPlus1msPvideo",*shiftedCamera);
        jsonSet(entry,"cameraNominalShiftSamples",expectedCamera); jsonSet(entry,"cameraIntegerRoundingTolerance",1);
        entries.add(entry);
    }
    jsonSet(report,"status",passed ? "PASS" : "FAIL"); jsonSet(report,"cameras",entries);
    jsonSet(report,"measurement","Same captured FrameStamp and frozen master/camera snapshots; profile output +97 samples and camera +1ms. Output shift exact; camera rounding <=1 sample. Not an optical/loopback measurement.");
    return report;
}
}
