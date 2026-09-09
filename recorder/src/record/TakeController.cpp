#include "TakeController.h"
#include "Mp4TakeWriter.h"
#include "capture/MfCameraCapture.h"
#include "storage/StorageEncoding.h"
#include "media/ThumbnailCache.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <thread>

namespace gocue::recorder
{
namespace
{
void waitBriefly() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
void requireResult(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
double elapsedMs(std::int64_t start) { return 1000.0 * double(qpcNow() - start) / double(qpcFrequency()); }
void flushExisting(const juce::File& file)
{
    const auto handle = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Open completed metadata for flush");
    const auto flushed = FlushFileBuffers(handle); const auto error = GetLastError(); CloseHandle(handle);
    if (!flushed) checkHr(HRESULT_FROM_WIN32(error), "Flush completed metadata");
}
void writeJsonDurable(const juce::File& file, const juce::var& json)
{
    CaptureTelemetry::writeJson(file, json); flushExisting(file);
}
class PacketQueue
{
public:
    PacketQueue(unsigned count, std::uint64_t bytes) : capacity(count), byteLimit(bytes)
    { for (unsigned i = 0; i < count; ++i) packets.push_back(ffPacket()); }
    bool push(const AVPacket& p)
    {
        const auto w = written.load();
        if (p.size < 0 || w - read.load(std::memory_order_acquire) == capacity || bytes.load() + p.size > byteLimit) return false;
        ffCheck(av_packet_ref(packets[w % capacity].get(), &p), "Queue take packet");
        bytes.fetch_add(p.size); written.store(w + 1, std::memory_order_release); return true;
    }
    const AVPacket* peek() const
    { const auto r = read.load(); return r == written.load(std::memory_order_acquire) ? nullptr : packets[r % capacity].get(); }
    void release()
    {
        auto* p = packets[read.load() % capacity].get(); bytes.fetch_sub(p->size); av_packet_unref(p); read.fetch_add(1, std::memory_order_release);
    }
private:
    const unsigned capacity;
    const std::uint64_t byteLimit;
    std::vector<PacketPtr> packets;
    std::atomic<std::uint64_t> read{0}, written{0}, bytes{0};
};
// Retains round 02's MF PTS cadence, with the first callback QPC translated to
// ASIO sample coordinates. Frozen temporary clock snapshot, no exposure claim.
class TakeCameraMapper final : public CameraTimeMapper
{
public:
    TakeCameraMapper(ClockMapping c, std::int64_t start, unsigned rate) : clock(c), n0(start), Fs(rate), pts(qpcFrequency()) {}
    std::int64_t map(const FrameStamp& frame) override
    {
        if (!begun) { offset = rescaleRound(clock.mapToSample(frame.callback) - n0, 10000000, Fs); begun = true; }
        return pts.map(frame) + offset;
    }
    std::int64_t now(std::int64_t qpc) const override { return rescaleRound(clock.mapToSample(qpc) - n0, 10000000, Fs); }
private:
    ClockMapping clock;
    std::int64_t n0, offset = 0;
    unsigned Fs;
    MfPtsTimeMapper pts;
    bool begun = false;
};
class LiveTakeVideo final : public ITakeVideoStream
{
public:
    LiveTakeVideo() = default;
    LiveTakeVideo(std::unique_ptr<CameraTimeMapper> timeMapper, juce::String cameraName)
        : mapper(std::move(timeMapper)), streamName(std::move(cameraName)) {}
    ~LiveTakeVideo() override
    {
        sourceFailure = 0; ending = true; audioEnded = true; aborting = true;
        if (encoderWorker.joinable()) encoderWorker.join();
        if (muxWorker.joinable()) muxWorker.join();
    }
    void prepare(const juce::File& output, NvencProfile p, Rational native, const AVCodecContext& audio) override
    {
        finalFile = output; profile = p; nativeRate = native;
        pool = std::make_unique<NvencFramePool>(p.cpuSurfaces());
        videoPackets = std::make_unique<PacketQueue>(unsigned(p.fps * 3), std::uint64_t(p.maxRate()) * 3 / 8);
        audioPackets = std::make_unique<PacketQueue>(192, 2 * 1024 * 1024);
        std::promise<void> prepared; auto future = prepared.get_future();
        encoderWorker = std::thread([this, &audio, prepared = std::move(prepared)]() mutable { encode(audio, std::move(prepared)); });
        future.get();
    }
    void startAt(ClockMapping clock, std::int64_t n0, unsigned rate, std::function<std::int64_t()> length) override
    {
        if (!mapper) mapper = std::make_unique<TakeCameraMapper>(clock, n0, rate);
        Fs = rate; acceptedLength = std::move(length);
        begun.store(true, std::memory_order_release);
    }
    void offer(const VideoSurface& frame) noexcept override
    {
        if (ending.load() || videoEnded.load() || sourceFailure.load() >= 0) return;
        if (pool && !pool->copy(frame)) { overflow.fetch_add(1); failedFlag = true; }
        else gotFrame = true;
    }
    void audioPacket(const AVPacket& p) override
    {
        if (muxFailed.load()) throw std::runtime_error("Take mux failed");
        if (!audioPackets->push(p)) { failedFlag = true; throw std::runtime_error("Take AAC packet queue overflow"); }
    }
    bool ready() const noexcept override { return gotFrame.load(); }
    void sourceFailed(std::int64_t sample) noexcept override
    { std::int64_t unset = -1; sourceFailure.compare_exchange_strong(unset, std::max<std::int64_t>(0, sample)); failedFlag = true; }
    void endAt(std::int64_t length) noexcept override { finalLength = length; ending.store(true, std::memory_order_release); }
    void audioDone() noexcept override { audioEnded.store(true, std::memory_order_release); }
    void finish() override
    {
        if (encoderWorker.joinable()) encoderWorker.join();
        if (muxWorker.joinable()) muxWorker.join();
    }
    bool failed() const noexcept override { return failedFlag.load(); }
    std::int64_t availableSamples() const noexcept override
    { const auto failure = sourceFailure.load(); return failure < 0 ? available.load() : std::min(available.load(), failure); }
    bool thumbnailReady() const noexcept override { return thumbnail.load(); }
    juce::var report() const override
    {
        auto v = jsonObject(); jsonSet(v, "encoder", encoderReport); jsonSet(v, "mux", muxReport); jsonSet(v, "cfr", cfrReport);
        jsonSet(v, "inspection", inspection); jsonSet(v, "error", encoderError); jsonSet(v, "muxError", muxError);
        jsonSet(v, "surfaceOverflow", jsonInt(overflow.load())); jsonSet(v, "failed", failed()); jsonSet(v, "availableSamples", jsonInt(availableSamples()));
        jsonSet(v, "clockMapping", "Round-02 MF PTS cadence + first callback QPC mapped by replaceable IClockMapper OLS snapshot; uncalibrated");
        jsonSet(v, "thumbnail", thumbnail.load() ? thumbnailPath() : juce::String("unavailable")); return v;
    }
private:
    juce::File finalFile;
    NvencProfile profile;
    Rational nativeRate;
    std::unique_ptr<NvencFramePool> pool;
    std::unique_ptr<PacketQueue> videoPackets, audioPackets;
    std::unique_ptr<CameraTimeMapper> mapper;
    juce::String streamName = "cam1";
    std::function<std::int64_t()> acceptedLength;
    unsigned Fs = 48000;
    std::atomic<bool> gotFrame{false}, begun{false}, ending{false}, videoEnded{false}, audioEnded{false}, failedFlag{false}, muxFailed{false}, aborting{false}, thumbnail{false};
    std::atomic<std::int64_t> finalLength{0}, sourceFailure{-1}, available{0};
    std::atomic<std::uint64_t> overflow{0};
    std::thread encoderWorker, muxWorker;
    juce::var encoderReport, muxReport, cfrReport, inspection;
    std::string encoderError, muxError;
    juce::String thumbnailPath() const
    { return streamName == "cam1" ? juce::String("index/first-thumbnail.bmp") : "index/" + streamName + "-first-thumbnail.bmp"; }
    bool thumbnailQueued = false; // encoder worker only
    ThumbnailCache thumbnailWorker; // first thumbnail disk I/O is below original media work
    void saveThumbnail(const AVFrame& frame)
    {
        // Small, rebuildable 160x90 luminance BMP; never scan media on stop.
        constexpr unsigned width = 160, height = 90, stride = width * 3;
        std::vector<std::uint8_t> data(54 + stride * height, 0); data[0] = 'B'; data[1] = 'M';
        storageEncoding::put(data.data() + 2, std::uint32_t(data.size())); storageEncoding::put(data.data() + 10, 54u);
        storageEncoding::put(data.data() + 14, 40u); storageEncoding::put(data.data() + 18, width); storageEncoding::put(data.data() + 22, height);
        storageEncoding::put(data.data() + 26, std::uint16_t(1)); storageEncoding::put(data.data() + 28, std::uint16_t(24));
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
        {
            const int luma = frame.data[0][(std::size_t(y) * frame.height / height) * frame.linesize[0] + x * frame.width / width];
            const auto value = std::uint8_t(std::clamp((luma - 16) * 255 / 219, 0, 255));
            auto* p = data.data() + 54 + (height - 1 - y) * stride + x * 3; p[0] = p[1] = p[2] = value;
        }
        const auto file = finalFile.getParentDirectory().getChildFile(thumbnailPath());
        thumbnailQueued = thumbnailWorker.enqueue("first", [this, file, data = std::move(data)](const auto& yield)
        {
            if (yield()) return;
            DurableFile output; requireResult(output.open(file, DurableFile::OpenMode::createNew)); requireResult(output.write(data.data(), data.size()));
            requireResult(output.flushData()); requireResult(output.close()); thumbnail = true;
        });
    }
    void mux(const AVCodecContext& video, const AVCodecContext& audio, std::promise<void> prepared)
    {
        bool signalled = false;
        std::unique_ptr<Mp4TakeWriter> writer;
        try
        {
            writer = std::make_unique<Mp4TakeWriter>(finalFile, video, audio);
            prepared.set_value(); signalled = true;
            for (;;)
            {
                const bool done = videoEnded.load(std::memory_order_acquire) && audioEnded.load(std::memory_order_acquire);
                bool consumed = false;
                if (const auto* p = videoPackets->peek()) { writer->video(*p); videoPackets->release(); consumed = true; }
                if (const auto* p = audioPackets->peek()) { writer->audio(*p); audioPackets->release(); consumed = true; }
                if (!consumed) { if (done) break; waitBriefly(); }
            }
            if (!aborting.load()) { writer->finalize(); inspection = Mp4TakeWriter::inspect(finalFile); }
        }
        catch (const std::exception& e)
        {
            muxError = e.what(); failedFlag = true; muxFailed = true;
            if (!signalled) prepared.set_exception(std::current_exception());
        }
        if (writer) muxReport = writer->toJson();
    }
    void encode(const AVCodecContext& audio, std::promise<void> prepared)
    {
        bool signalled = false;
        VideoCfrScheduler scheduler(nativeRate, {unsigned(profile.fps), 1});
        std::unique_ptr<NvencEncoder> encoder;
        int preroll = -1;
        try
        {
            encoder = std::make_unique<NvencEncoder>(profile); encoder->open();
            std::promise<void> muxReady; auto future = muxReady.get_future();
            muxWorker = std::thread([this, &audio, &encoder, ready = std::move(muxReady)]() mutable { mux(encoder->context(), audio, std::move(ready)); });
            future.get(); prepared.set_value(); signalled = true;
            const auto traceFile = finalFile.getParentDirectory().getChildFile("index/" + streamName + "-source-ids.csv");
            std::ofstream trace(std::filesystem::path(traceFile.getFullPathName().toWideCharPointer()), std::ios::binary);
            trace.exceptions(std::ios::badbit | std::ios::failbit); trace << "pts,sourceId,mfPts100ns,callbackQpc,mapped100ns\n";
            const PacketSink sink = [this](const AVPacket& p)
            {
                if (muxFailed.load() || !videoPackets->push(p)) throw std::runtime_error("Take video packet queue/mux failure");
                available.store(rescaleRound(p.pts + 1, Fs, unsigned(profile.fps)));
            };
            const auto accept = [&](int slot)
            {
                const auto time = mapper->map(pool->stamp(slot));
                if (time < 0)
                { if (preroll >= 0) pool->release(preroll); preroll = slot; return; }
                if (preroll >= 0)
                {
                    scheduler.push({pool->stamp(preroll).frame, 0, preroll}); preroll = -1;
                    if (time == 0) { pool->release(slot); return; }
                }
                scheduler.push({pool->stamp(slot).frame, time, slot});
            };
            for (;;)
            {
                if (aborting.load()) break;
                if (muxFailed.load()) throw std::runtime_error("Take mux failed");
                if (!begun.load(std::memory_order_acquire))
                {
                    int slot = -1;
                    while (pool->pop(slot)) { if (preroll >= 0) pool->release(preroll); preroll = slot; }
                    if (ending.load()) break; waitBriefly(); continue;
                }
                if (preroll >= 0 && !scheduler.size())
                {
                    const auto slot = preroll; preroll = -1; accept(slot);
                }
                int slot = -1;
                while (pool->pop(slot)) accept(slot);
                auto length = std::max<std::int64_t>(0, acceptedLength());
                if (ending.load(std::memory_order_acquire)) length = finalLength.load();
                if (sourceFailure.load() >= 0) length = std::min(length, sourceFailure.load());
                const auto target = TakeController::frameCount(length, Fs, {unsigned(profile.fps), 1});
                while (scheduler.nextPts() < target)
                {
                    while (pool->pop(slot)) accept(slot);
                    const auto chosen = scheduler.select(mapper->now(qpcNow()), ending.load());
                    if (!chosen) break;
                    for (std::size_t i = 0; i < chosen->releasedCount; ++i) pool->release(chosen->released[i]);
                    const auto& stamp = pool->stamp(chosen->input.slot);
                    if (!thumbnailQueued) { try { saveThumbnail(pool->frame(chosen->input.slot)); } catch (...) {} }
                    trace << chosen->pts << ',' << stamp.frame << ',' << stamp.pts100ns << ',' << stamp.callback << ',' << chosen->input.time100ns << '\n';
                    encoder->submit(pool->frame(chosen->input.slot), chosen->pts, sink);
                }
                if (ending.load())
                {
                    if (scheduler.nextPts() < target) { failedFlag = true; sourceFailed(available.load()); }
                    break;
                }
                waitBriefly();
            }
            encoder->drain(sink); trace.flush(); trace.close();
        }
        catch (const std::exception& e)
        {
            encoderError = e.what(); failedFlag = true; sourceFailed(available.load());
            if (!signalled) prepared.set_exception(std::current_exception());
        }
        if (preroll >= 0) pool->release(preroll);
        std::size_t count = 0; const auto slots = scheduler.releaseAll(count);
        for (std::size_t i = 0; i < count; ++i) pool->release(slots[i]);
        cfrReport = scheduler.counters().toJson(); if (encoder) encoderReport = encoder->toJson();
        videoEnded.store(true, std::memory_order_release);
        if (!signalled) audioEnded = true;
        // Codec context remains alive until the mux has copied/drained all packets.
        if (muxWorker.joinable()) muxWorker.join();
    }
};
}
struct TakeController::Impl
{
    RecorderDocument& document;
    RecorderAudioEngine& audio;
    VideoFactory factory;
    Config config;
    State current = State::idle;
    std::vector<State> transitions{State::idle};
    juce::String failure, warning;
    Take take;
    std::vector<MediaAsset> assets;
    std::vector<unsigned> logicalMics;
    std::vector<int> logicalIndices;
    std::unique_ptr<ITakeVideoStream> video;
    std::atomic<ITakeVideoStream*> recordSink{nullptr};
    std::atomic<unsigned> offerInFlight{0};
    std::shared_ptr<VideoSurfacePool> preview, pendingPreview;
    std::shared_ptr<CaptureTelemetry> captureTelemetry;
    std::unique_ptr<MfCameraCapture> capture;
    std::unique_ptr<MfRuntime> mfRuntime; // remains alive with the preview capture after finalization
    std::future<juce::Result> work;
    RecorderDocument::Snapshot savedSnapshot;
    bool saving = false, partial = false, preparedAudio = false;
    std::int64_t requestedN0 = -1, length = 0, placement = 0, stopQpc = 0, prepareQpc = 0, finalizationQpc = 0;
    double placementMs = 0, finalizationMs = 0, mediaFinalizationMs = 0, stopToDoneMs = 0;
    juce::Uuid placementEdit;
    juce::var audioReport, videoReport;
    RecorderAudioEngine::DeviceInfo deviceSnapshot;
    std::vector<JournalDeviceMapping> mappingSnapshot;
    std::array<float, 8> peakSnapshot{};
    PlacementMetadata placementMetadata;
    Impl(RecorderDocument& d, RecorderAudioEngine& a, VideoFactory f) : document(d), audio(a), factory(std::move(f))
    { if (!factory) factory = [] { return std::make_unique<LiveTakeVideo>(); }; }
    ~Impl()
    {
        if (work.valid()) work.wait();
        detachSink();
        if (capture) capture->stop();
        if (preparedAudio)
        {
            audio.abort(RecorderAudioEngine::Error::cancelled);
            audio.finishCapture(placementEdit);
            if (video) { video->endAt(std::max<std::int64_t>(0, length)); video->audioDone(); video->finish(); }
            audio.finishJournal(false);
        }
        video.reset(); document.setRecordingStructureLock(false);
    }
    void transition(State next) { if (current != next) { current = next; transitions.push_back(next); } }
    void detachSink()
    { recordSink.store(nullptr); while (offerInFlight.load()) waitBriefly(); }
    void offer(const VideoSurface& frame) noexcept
    {
        offerInFlight.fetch_add(1);
        if (auto* sink = recordSink.load()) sink->offer(frame);
        offerInFlight.fetch_sub(1);
    }
    juce::File takeFolder() const { return config.projectDirectory.getChildFile("media/takes/" + config.takeId.toDashedString()); }
    juce::var manifest(const char* state) const
    {
        auto v = jsonObject(); jsonSet(v, "schemaVersion", 1); jsonSet(v, "takeId", config.takeId.toDashedString());
        jsonSet(v, "mode", "normal"); jsonSet(v, "state", state); jsonSet(v, "N0", requestedN0); jsonSet(v, "Nstop", requestedN0 < 0 ? -1 : requestedN0 + length);
        jsonSet(v, "placementSample", placement); jsonSet(v, "logicalLength", length); jsonSet(v, "Fs", int(deviceSnapshot.sampleRate));
        jsonSet(v, "fpsNumerator", config.projectFps); jsonSet(v, "fpsDenominator", 1);
        jsonSet(v, "placementEditId", placementEdit.toDashedString());
        jsonSet(v, "cameraDevice", config.synthetic ? "synthetic" : config.cameraSymbolicLink); jsonSet(v, "cameraMode", config.cameraMode.text());
        jsonSet(v, "cameraAssetId", take.cam1AssetId);
        juce::Array<juce::var> microphones, peakValues;
        const auto& mappings = mappingSnapshot; const auto& peaks = peakSnapshot;
        for (std::size_t i = 0; i < logicalMics.size(); ++i)
        {
            auto m = jsonObject(); jsonSet(m, "mic", int(logicalMics[i])); jsonSet(m, "assetId", take.microphoneAssetIds[i]);
            jsonSet(m, "physicalIndex", mappings[i].physicalIndex); jsonSet(m, "activeIndex", mappings[i].activeIndex); microphones.add(m);
        }
        for (auto peak : peaks) peakValues.add(double(peak));
        jsonSet(v, "microphones", microphones); jsonSet(v, "peaks", peakValues);
        jsonSet(v, "firstThumbnail", video && video->thumbnailReady() ? "index/first-thumbnail.bmp" : "");
        jsonSet(v, "audio", audioReport); jsonSet(v, "video", videoReport);
        jsonSet(v, "clockMapping", "Temporary IClockMapper OLS / first camera QPC + MF PTS; not physical sync calibration");
        return v;
    }
    void initialiseAssets()
    {
        const auto device = audio.deviceInfo(); deviceSnapshot = device; mappingSnapshot = audio.microphoneMapping(); peakSnapshot.fill(0);
        take = {}; take.takeId = config.takeId.toString(); take.cam1AssetId = newId();
        take.capture.asioDeviceId = device.name; take.capture.cameraDeviceIds[0] = juce::String(config.synthetic ? "synthetic" : config.cameraSymbolicLink);
        take.capture.cameraModes[0] = config.cameraMode.text();
        take.capture.calibrationIdentity = "temporary-linear-uncalibrated";
        assets.clear(); logicalMics = audio.armedMicrophones(); logicalIndices.clear();
        MediaAsset camera; camera.assetId = take.cam1AssetId; camera.kind = AssetKind::camera;
        camera.relativePath = "media/takes/" + config.takeId.toDashedString() + "/cam1.recording.mp4";
        camera.contentIdentity = camera.assetId; camera.originalFormat.codec = "h264";
        camera.originalFormat.width = 1920; camera.originalFormat.height = 1080; camera.originalFormat.fps = {unsigned(config.projectFps), 1};
        camera.sourceUnitsNumerator = unsigned(config.projectFps); camera.sourceUnitsDenominator = device.sampleRate; assets.push_back(camera);
        for (std::size_t i = 0; i < logicalMics.size(); ++i)
        {
            const auto mapping = mappingSnapshot[i];
            MediaAsset mic; mic.kind = AssetKind::mic; mic.contentIdentity = mic.assetId;
            mic.originalFormat.codec = "pcm_s24le"; mic.originalFormat.sampleRate = device.sampleRate;
            mic.originalFormat.channels = 1; mic.originalFormat.bitsPerSample = 24;
            take.microphoneAssetIds.push_back(mic.assetId); take.capture.physicalInputs.push_back(mapping.physicalIndex);
            logicalIndices.push_back(int(logicalMics[i]) - 1); assets.push_back(mic);
        }
    }
    juce::Result prepareWorker()
    {
        try
        {
            detachSink(); if (capture) { capture->stop(); capture.reset(); }
            video.reset();
            requireResult(takeFolder().createDirectory()); requireResult(takeFolder().getChildFile("index").createDirectory());
            for (const char* path : {"media/imports", "cache", "recovery", "exports"}) requireResult(config.projectDirectory.getChildFile(path).createDirectory());
            video = factory();
            RecorderAudioEngine::TakeConfig audioConfig; audioConfig.projectDirectory = config.projectDirectory; audioConfig.takeId = config.takeId;
            audioConfig.placementSample = placement;
            audioConfig.additionalFiles.push_back({take.cam1AssetId, assets[0].relativePath, "media/takes/" + config.takeId.toDashedString() + "/cam1.mp4"});
            audioConfig.microphoneAssetIds = take.microphoneAssetIds;
            audioConfig.referencePackets = [this](const AVPacket& p) { video->audioPacket(p); };
            requireResult(audio.prepare(std::move(audioConfig))); preparedAudio = true;
            video->prepare(takeFolder().getChildFile("cam1.mp4"), NvencProfile{config.projectFps}, config.cameraMode.fps, *audio.referenceContext());
            recordSink.store(video.get(), std::memory_order_release);
            if (!config.synthetic && !config.externalCapture)
            {
                if (!mfRuntime) mfRuntime = std::make_unique<MfRuntime>();
                captureTelemetry = std::make_shared<CaptureTelemetry>(config.cameraMode.fps);
                capture = std::make_unique<MfCameraCapture>(captureTelemetry, *pendingPreview, [this](const VideoSurface& f) { offer(f); });
                capture->start(config.cameraSymbolicLink, config.cameraMode, config.cameraMode.subtype == CaptureSubtype::mjpeg);
            }
            writeJsonDurable(takeFolder().getChildFile("take.json"), manifest("preparing"));
            return juce::Result::ok();
        }
        catch (const std::exception& e)
        {
            detachSink(); if (capture) capture->stop();
            if (preparedAudio)
            {
                audio.abort(RecorderAudioEngine::Error::cancelled); audio.finishCapture(placementEdit);
                if (video) { video->endAt(0); video->audioDone(); video->finish(); }
                audio.finishJournal(false); preparedAudio = false;
            }
            return juce::Result::fail(e.what());
        }
    }
    void setRanges(MediaAsset& asset, std::int64_t available)
    {
        available = std::clamp(available, std::int64_t(0), length);
        asset.logicalLength = length; asset.availableRanges.clear(); asset.gaps.clear();
        if (available) asset.availableRanges.push_back({0, available});
        if (available < length) asset.gaps.push_back({available, length - available});
    }
    void setChunks(MediaAsset& asset, unsigned mic, std::int64_t available)
    {
        asset.chunks.clear();
        const auto chunk = std::int64_t(deviceSnapshot.sampleRate) * WavTrackWriter::chunkSeconds;
        for (std::int64_t first = 0; first < available; first += chunk)
            asset.chunks.push_back({WavTrackWriter::chunkPath(config.takeId, mic, std::uint64_t(first / chunk) + 1), {first, std::min(chunk, available - first)}});
        asset.relativePath = asset.chunks.empty() ? WavTrackWriter::chunkPath(config.takeId, mic, 1) : juce::String();
    }
    void placeStopped()
    {
        if (!stopQpc) stopQpc = qpcNow();
        length = audio.stopSample() - audio.startSample();
        peakSnapshot = audio.peaks();
        placementMetadata = {length > 0, false, audio.startSample(), audio.stopSample(), placement, deviceSnapshot.sampleRate, peakSnapshot,
                             video && video->thumbnailReady() ? takeFolder().getChildFile("index/first-thumbnail.bmp") : juce::File()};
        placementMetadata.waveform = audio.peakCache();
        document.setRecordingStructureLock(false);
        if (length <= 0)
        {
            partial = true; failure = "No nonempty callback-confirmed take range";
        }
        else
        {
            take.N0 = audio.startSample(); requestedN0 = take.N0; take.logicalLength = length;
            take.placementSample = placement; take.state = TakeState::finalising;
            for (auto& asset : assets) setRanges(asset, length);
            for (std::size_t i = 0; i < logicalMics.size(); ++i) setChunks(assets[i + 1], logicalMics[i], length);
            const auto result = document.placeTake(take, assets, logicalIndices);
            if (result.failed()) { partial = true; failure = result.getErrorMessage(); }
            else { take = *document.getProject().media->findTake(take.takeId); placementEdit = juce::Uuid(document.lastEditTransaction()); }
        }
        placementMs = elapsedMs(stopQpc); finalizationQpc = qpcNow(); transition(State::finalizing);
        work = std::async(std::launch::async, [this]
        {
            const auto start = qpcNow();
            try
            {
                // Commit TakeStopped immediately after placement, then finish raw
                // capture. Preview remains live while its encode sink is detached.
                const auto audioResult = audio.finishCapture(placementEdit);
                if (audioResult.failed()) partial = true;
                audioReport = audio.telemetry();
                peakSnapshot = audio.peaks();
                // Permit at most one native period for an already exposed frame.
                const auto deadline = qpcNow() + std::int64_t(config.cameraMode.fps.periodMs() * double(qpcFrequency()) / 1000.0);
                while (qpcNow() < deadline) waitBriefly();
                detachSink(); video->endAt(length); video->audioDone(); video->finish(); videoReport = video->report();
                partial = partial || video->failed() || audio.referenceFailed();
                const auto validVideo = std::min(length, video->availableSamples());
                setRanges(assets[0], validVideo);
                if (takeFolder().getChildFile("cam1.mp4").existsAsFile()) assets[0].relativePath = "media/takes/" + config.takeId.toDashedString() + "/cam1.mp4";
                else setRanges(assets[0], 0); // incomplete payload is preserved for recovery, not advertised as playable
                const auto written = std::int64_t(audioReport["wav"]["writtenSamplesPerMic"]);
                for (std::size_t i = 0; i < logicalMics.size(); ++i)
                { setRanges(assets[i + 1], written); setChunks(assets[i + 1], logicalMics[i], std::min(length, written)); }
                for (auto& asset : assets) ++asset.mediaGeneration;
                mediaFinalizationMs = elapsedMs(start); return audioResult;
            }
            catch (const std::exception& e) { partial = true; mediaFinalizationMs = elapsedMs(start); return juce::Result::fail(e.what()); }
        });
    }
};
TakeController::TakeController(RecorderDocument& d, RecorderAudioEngine& a, VideoFactory f) : impl(std::make_unique<Impl>(d, a, std::move(f))) {}
TakeController::~TakeController() = default;
juce::Result TakeController::reset()
{
    if (impl->work.valid() || (state() != State::idle && state() != State::done && state() != State::partialFailure))
        return juce::Result::fail("Take is still active");
    impl->detachSink(); impl->placementMetadata = {}; impl->length = 0; impl->placement = 0;
    impl->failure.clear(); impl->warning.clear(); impl->current = State::idle;
    return juce::Result::ok();
}
juce::Result TakeController::prepare(Config config)
{
    auto& s = *impl;
    if (s.current != State::idle && s.current != State::done && s.current != State::partialFailure) return juce::Result::fail("Take controller is busy");
    if (s.work.valid()) return juce::Result::fail("Previous worker completion must be consumed");
    const auto device = s.audio.deviceInfo();
    if (!device.sampleRate || config.projectDirectory == juce::File() || config.takeId.isNull()
        || (config.projectFps != 30 && config.projectFps != 60) || config.cameraMode.width != 1920 || config.cameraMode.height != 1080
        || !config.cameraMode.fps.numerator || !config.cameraMode.fps.denominator || (!config.synthetic && config.cameraSymbolicLink.empty()))
        return juce::Result::fail("Select a ready ASIO device and cam1 1080p native mode / project 30 or 60");
    const auto timebase = s.document.setTimebase(device.sampleRate, {unsigned(config.projectFps), 1});
    if (timebase.failed()) return timebase;
    s.config = std::move(config); s.failure.clear(); s.warning.clear(); s.partial = false; s.saving = false; s.preparedAudio = false;
    s.audioReport = juce::var(); s.videoReport = juce::var(); s.placementMetadata = {}; s.transitions = {State::idle}; s.current = State::idle;
    s.requestedN0 = -1; s.length = 0; s.stopQpc = s.finalizationQpc = 0;
    s.placementMs = s.finalizationMs = s.mediaFinalizationMs = s.stopToDoneMs = 0; s.placementEdit = juce::Uuid();
    s.placement = s.document.getProject().activeTimelineEnd(); s.initialiseAssets();
    s.pendingPreview = std::make_shared<VideoSurfacePool>(1920, 1080);
    if (s.logicalMics.empty()) s.warning = juce::String::fromUTF8("녹음 중인 마이크가 없습니다");
    s.document.setRecordingStructureLock(true); s.transition(State::preparing); s.prepareQpc = qpcNow();
    s.work = std::async(std::launch::async, [&s] { return s.prepareWorker(); }); return juce::Result::ok();
}
juce::Result TakeController::start(std::int64_t N0)
{
    auto& s = *impl;
    if (s.current != State::armed || s.requestedN0 >= 0) return juce::Result::fail("Take must be armed before start");
    const auto device = s.audio.deviceInfo(); if (N0 < 0) N0 = s.audio.currentSample() + std::max<std::int64_t>(device.sampleRate / 4, device.bufferFrames * 2);
    if (!s.audio.clockReady()) return juce::Result::fail("ASIO clock is not stable");
    const auto result = s.audio.startAt(N0); if (result.failed()) return result;
    s.requestedN0 = N0;
    s.video->startAt(s.audio.clockMapping(), N0, device.sampleRate, [&s]
    { return s.audio.startSample() >= 0 ? std::max<std::int64_t>(0, s.audio.acceptedEnd() - s.audio.startSample()) : 0; });
    return juce::Result::ok();
}
juce::Result TakeController::stop(std::int64_t Nstop)
{
    auto& s = *impl;
    if (s.current != State::recording && !(s.current == State::armed && s.requestedN0 >= 0)) return juce::Result::fail("No recording to stop");
    if (Nstop < 0) Nstop = std::max(s.audio.currentSample(), s.requestedN0) + s.audio.deviceInfo().bufferFrames;
    const auto result = s.audio.stopAt(Nstop); if (result.failed()) return result;
    s.stopQpc = qpcNow(); s.transition(State::stopping); return juce::Result::ok();
}
void TakeController::tick()
{
    auto& s = *impl;
    if (s.work.valid())
    {
        if (s.work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
        const auto result = s.work.get();
        if (result.failed()) { s.failure = result.getErrorMessage(); s.partial = true; }
        if (s.current == State::preparing && result.failed())
        { s.document.setRecordingStructureLock(false); s.transition(State::partialFailure); return; }
        if (s.current == State::preparing) s.preview = s.pendingPreview;
        if (s.current == State::finalizing)
        {
            if (s.saving)
            {
                if (result.failed() && s.length > 0 && s.document.getProject().media->findTake(s.take.takeId)) s.document.updateTakeState(s.take.takeId, TakeState::partial);
                s.document.checkpointFinished(s.savedSnapshot, s.config.projectDirectory.getChildFile("project.recorder"), result);
                s.finalizationMs = elapsedMs(s.finalizationQpc); s.stopToDoneMs = elapsedMs(s.stopQpc);
                s.preparedAudio = false; s.transition(s.partial ? State::partialFailure : State::done); return;
            }
            s.placementMetadata.peaks = s.peakSnapshot; s.placementMetadata.peaksComplete = true;
            if (s.video && s.video->thumbnailReady()) s.placementMetadata.firstThumbnail = s.takeFolder().getChildFile("index/first-thumbnail.bmp");
            if (s.length > 0 && s.document.getProject().media->findTake(s.take.takeId))
            {
                for (auto asset : s.assets)
                {
                    const auto updated = s.document.updateMediaAsset(std::move(asset));
                    if (updated.failed()) { s.partial = true; s.failure = updated.getErrorMessage(); }
                }
                const auto updated = s.document.updateTakeState(s.take.takeId, s.partial ? TakeState::partial : TakeState::complete);
                if (updated.failed()) { s.partial = true; s.failure = updated.getErrorMessage(); }
            }
            s.savedSnapshot = s.document.snapshot(); s.saving = true;
            s.work = std::async(std::launch::async, [&s]
            {
                try
                {
                    // Derived cache is disposable; its failure cannot change original durability.
                    if (const auto peaks = s.audio.peakCache())
                        PeakCache::write(s.config.projectDirectory.getChildFile("cache/" + s.take.takeId + ".peaks.json"), peaks->snapshot());
                    writeJsonDurable(s.takeFolder().getChildFile("take.json"), s.manifest(s.partial ? "partialFailure" : "done"));
                    const auto file = s.config.projectDirectory.getChildFile("project.recorder");
                    requireResult(RecorderSerializer::writeCheckpoint(file, *s.savedSnapshot)); flushExisting(file);
                    requireResult(s.audio.finishJournal(!s.partial)); return juce::Result::ok();
                }
                catch (const std::exception& e) { s.audio.finishJournal(false); return juce::Result::fail(e.what()); }
            });
            return;
        }
    }
    if (s.current == State::preparing)
    {
        if (s.audio.clockReady() && s.video && s.video->ready()) s.transition(State::armed);
        else if (elapsedMs(s.prepareQpc) > 10000)
        {
            s.failure = "ASIO clock/camera did not become ready within 10 seconds"; s.partial = true;
            s.audio.abort(RecorderAudioEngine::Error::cancelled); s.document.setRecordingStructureLock(false);
            s.transition(State::stopping);
        }
    }
    if (s.current == State::armed || s.current == State::recording || s.current == State::stopping)
    {
        s.audio.pollDeviceEvents();
        if (s.capture && s.capture->finished()) cameraFailed();
        if (s.audio.referenceFailed()) cameraFailed();
        if (s.current == State::armed && s.audio.startSample() >= 0) s.transition(State::recording);
        if (s.audio.error() != RecorderAudioEngine::Error::none)
        { s.partial = true; s.transition(State::stopping); }
        if (s.audio.stopSample() >= 0 || (s.current == State::stopping && s.audio.error() != RecorderAudioEngine::Error::none))
            s.placeStopped();
    }
}
TakeController::State TakeController::state() const noexcept { return impl->current; }
bool TakeController::structureEditingLocked() const noexcept { return impl->document.isRecordingStructureLocked(); }
juce::String TakeController::statusText() const
{
    switch (state())
    {
        case State::idle: return juce::String::fromUTF8("대기");
        case State::preparing: return juce::String::fromUTF8("준비 중");
        case State::armed: return juce::String::fromUTF8("녹화 준비됨");
        case State::recording: return juce::String::fromUTF8("녹화 중");
        case State::stopping: return juce::String::fromUTF8("정지 중");
        case State::finalizing: return juce::String::fromUTF8(impl->saving ? "저장 중" : "마무리 중");
        case State::done: return juce::String::fromUTF8("완료");
        case State::partialFailure: return juce::String::fromUTF8("일부 자료 저장 · 확인 필요");
    }
    return {};
}
juce::String TakeController::warning() const { return impl->warning; }
juce::String TakeController::error() const { return impl->failure; }
std::int64_t TakeController::scheduledStart() const noexcept { return impl->requestedN0; }
std::int64_t TakeController::logicalLength() const noexcept { return impl->length; }
std::int64_t TakeController::placementSample() const noexcept { return impl->placement; }
const TakeController::PlacementMetadata& TakeController::placementMetadata() const noexcept { return impl->placementMetadata; }
void TakeController::offer(const VideoSurface& frame) noexcept { impl->offer(frame); }
void TakeController::cameraFailed() noexcept
{
    if (auto* sink = impl->recordSink.load()) sink->sourceFailed(std::max<std::int64_t>(0, impl->audio.acceptedEnd() - impl->requestedN0));
}
std::shared_ptr<VideoSurfacePool> TakeController::previewPool() const { return impl->preview; }
juce::var TakeController::report() const
{
    const auto& s = *impl;
    if (s.current != State::done && s.current != State::partialFailure) throw std::logic_error("Take report is only available after finalization");
    auto v = s.manifest(stateName(s.current)); juce::Array<juce::var> states;
    for (auto state : s.transitions) states.add(stateName(state));
    jsonSet(v, "states", states); jsonSet(v, "error", s.failure); jsonSet(v, "warning", s.warning);
    jsonSet(v, "stopToPlacementMs", s.placementMs); jsonSet(v, "finalizationMs", s.finalizationMs);
    jsonSet(v, "mediaFinalizationMs", s.mediaFinalizationMs); jsonSet(v, "stopToDoneMs", s.stopToDoneMs);
    jsonSet(v, "timingDefinition", "QPC wall time: stopToPlacement includes reserved Nstop wait; finalization is placement through media/project/journal flush acknowledgement; mediaFinalization is worker drain only");
    jsonSet(v, "expectedVideoFrames", frameCount(std::max<std::int64_t>(0, s.length), s.deviceSnapshot.sampleRate, {unsigned(s.config.projectFps), 1}));
    jsonSet(v, "cam2", "disabled; no encoder or file"); jsonSet(v, "projectDirectory", s.config.projectDirectory.getFullPathName());
    return v;
}
std::int64_t TakeController::frameCount(std::int64_t samples, unsigned Fs, FrameRate fps)
{
    if (samples < 0 || !Fs || Fs > 768000 || !fps.numerator || !fps.denominator || fps.numerator > 1000000 || fps.denominator > 1000000)
        throw std::invalid_argument("Invalid sample/CFR timebase");
    const auto denominator = std::uint64_t(Fs) * fps.denominator, n = std::uint64_t(samples);
    const auto quotient = n / denominator, remainder = n % denominator;
    const auto tail = remainder * fps.numerator;
    const auto extra = tail / denominator + (tail % denominator != 0);
    const auto maximum = std::uint64_t((std::numeric_limits<std::int64_t>::max)());
    if (quotient > (maximum - extra) / fps.numerator) throw std::overflow_error("CFR frame count exceeds int64");
    return std::int64_t(quotient * fps.numerator + extra);
}
const char* TakeController::stateName(State s) noexcept
{
    switch (s)
    {
        case State::idle: return "idle"; case State::preparing: return "preparing"; case State::armed: return "armed";
        case State::recording: return "recording"; case State::stopping: return "stopping"; case State::finalizing: return "finalizing";
        case State::done: return "done"; case State::partialFailure: return "partialFailure";
    }
    return "unknown";
}
std::unique_ptr<ITakeVideoStream> TakeController::createVideoStream(std::unique_ptr<CameraTimeMapper> mapper,
                                                                 const juce::String& cameraName)
{ return std::make_unique<LiveTakeVideo>(std::move(mapper), cameraName); }
}
