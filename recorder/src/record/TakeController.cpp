#include "TakeController.h"
#include "Mp4TakeWriter.h"
#include "capture/MfCameraCapture.h"
#include "storage/StorageEncoding.h"
#include "media/ThumbnailCache.h"
#include "sync/CameraClockMapper.h"
#include "support/ThreadPriority.h"
#include "app/RecorderLifecycle.h"
#include "storage/IoHealth.h"
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
// Owner-thread fault seam: tests can fail thread creation without exhausting OS resources.
namespace exception_test { thread_local std::function<void(const char*)> beforeTakeWorker; }
namespace
{
void waitBriefly() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
void requireResult(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
juce::Result takeException(const char* context)
{
    try { throw; }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(context) + ": " + juce::String::fromUTF8(e.what())); }
    catch (...) { return juce::Result::fail(juce::String::fromUTF8(context) + ": unknown exception"); }
}
template<class F> auto launchTakeWorker(const char* phase, F&& work)
{
    if (exception_test::beforeTakeWorker) exception_test::beforeTakeWorker(phase);
    return std::async(std::launch::async, std::forward<F>(work));
}
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
    std::uint64_t count() const noexcept { const auto r = read.load(); return written.load() - r; }
    std::uint64_t byteCount() const noexcept { return bytes.load(); }
private:
    const unsigned capacity;
    const std::uint64_t byteLimit;
    std::vector<PacketPtr> packets;
    std::atomic<std::uint64_t> read{0}, written{0}, bytes{0};
};
class LiveTakeVideo final : public ITakeVideoStream
{
public:
    LiveTakeVideo() = default;
    explicit LiveTakeVideo(std::unique_ptr<CameraTimeMapper> timeMapper)
        : mapper(std::move(timeMapper)) {}
    void configureClock(const ClockMapper& master, std::int64_t latency) override
    { masterClock = &master; cameraLatency = latency; }
    void discontinuity() noexcept override
    { clockRevisions.fetch_add(1); }
    ~LiveTakeVideo() override
    {
        sourceFailure = 0; ending = true; audioEnded = true; aborting = true;
        if (encoderWorker.joinable()) encoderWorker.join();
        if (muxWorker.joinable()) muxWorker.join();
    }
    void prepare(const juce::File& output, NvencProfile p, Rational native, const AVCodecContext& audio) override
    {
        finalFile = output; profile = p; nativeRate = native;
        cameraClock = std::make_unique<CameraClockMapper>(masterClock ? *masterClock : timestampReference, qpcFrequency(), native, cameraLatency);
        thumbnailWorker.setRecording(true);
        pool = std::make_unique<NvencFramePool>(p.cpuSurfaces());
        videoPackets = std::make_unique<PacketQueue>(unsigned(p.fps * 3), std::uint64_t(p.maxRate()) * 3 / 8);
        audioPackets = std::make_unique<PacketQueue>(192, 2 * 1024 * 1024);
        std::promise<void> prepared; auto future = prepared.get_future();
        encoderWorker = std::thread([this, &audio, prepared = std::move(prepared)]() mutable { encode(audio, std::move(prepared)); });
        future.get();
    }
    void startAt(ClockMapping, std::int64_t n0, unsigned rate, std::function<std::int64_t()> length) override
    {
        if (!mapper)
        {
            const auto master = masterClock ? masterClock->snapshot() : std::nullopt;
            const auto camera = cameraClock->snapshot();
            if (!master || !master->valid || !camera || !camera->valid)
                throw std::runtime_error("Recording camera/master clock is not ready");
            mapper = std::make_unique<CameraSampleTimeMapper>(*cameraClock, n0, rate, master->epoch, *camera);
            masterEpoch = master->epoch;
        }
        else if (masterClock) if (const auto master = masterClock->snapshot()) masterEpoch = master->epoch;
        Fs = rate; acceptedLength = std::move(length);
        begun.store(true, std::memory_order_release);
    }
    void offer(const VideoSurface& frame) noexcept override
    {
        if (ending.load() || videoEnded.load() || sourceFailure.load() >= 0) return;
        if (pool && !pool->copy(frame, clockRevisions.load())) { overflow.fetch_add(1); sourceFailed(available.load()); }
        else gotFrame = true;
    }
    void audioPacket(const AVPacket& p) override
    {
        if (muxFailed.load()) throw std::runtime_error("Take mux failed");
        if (!audioPackets->push(p)) { failedFlag = true; throw std::runtime_error("Take AAC packet queue overflow"); }
    }
    bool ready() const noexcept override
    {
        const auto master = masterClock ? masterClock->snapshot() : std::nullopt;
        return gotFrame.load() && clockReady.load() && (!masterClock || (master && master->valid));
    }
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
    bool storageFailed() const noexcept override { return muxFailed.load(); }
    bool processingDelayed() const noexcept override { return ioHealth.delayed(); }
    std::int64_t availableSamples() const noexcept override
    { const auto failure = sourceFailure.load(); return failure < 0 ? available.load() : std::min(available.load(), failure); }
    bool thumbnailReady() const noexcept override { return thumbnail.load(); }
    juce::var report() const override
    {
        auto v = jsonObject(); jsonSet(v, "encoder", encoderReport); jsonSet(v, "mux", muxReport); jsonSet(v, "cfr", cfrReport);
        jsonSet(v, "inspection", inspection); jsonSet(v, "error", encoderError); jsonSet(v, "muxError", muxError);
        jsonSet(v, "surfaceOverflow", jsonInt(overflow.load())); jsonSet(v, "failed", failed()); jsonSet(v, "availableSamples", jsonInt(availableSamples()));
        jsonSet(v, "clockMapping", "Independent CameraClockMapper; Nv=S(q-Lcam), fixed N0/O0; recoverable camera reanchors; generation/native ASIO failures remain fatal");
        jsonSet(v, "Lcam100ns", cameraLatency); jsonSet(v, "masterEpoch", jsonInt(masterEpoch));
        jsonSet(v, "explicitDiscontinuity", clockRevisions.load() != 0);
        jsonSet(v, "reanchorRequests", jsonInt(clockRevisions.load()));
        jsonSet(v, "reanchors", jsonInt(reanchors));
        const auto clock = cameraClock ? cameraClock->snapshot() : std::nullopt;
        if (clock) { jsonSet(v, "cameraEpoch", jsonInt(clock->epoch)); jsonSet(v, "cameraEpochReason", cameraEpochReasonName(clock->reason)); jsonSet(v, "deviceGeneration", jsonInt(clock->generation)); jsonSet(v, "cameraClockSource", cameraClockSourceName(clock->quality.source)); }
        jsonSet(v, "surfaceCapacity", pool ? pool->capacity() : 0); jsonSet(v, "surfaceHighWater", pool ? int(pool->highWater()) : 0);
        jsonSet(v, "thumbnail", thumbnail.load() ? thumbnailPath().getFileName() : juce::String("unavailable")); return v;
    }
    TakeVideoQueues queues() const noexcept override
    {
        return {pool ? pool->occupied() : 0, pool ? pool->highWater() : 0, pool ? unsigned(pool->capacity()) : 0,
                videoPackets ? videoPackets->count() : 0, audioPackets ? audioPackets->count() : 0,
                videoPackets ? videoPackets->byteCount() : 0, overflow.load()};
    }
private:
    juce::File finalFile;
    NvencProfile profile;
    Rational nativeRate;
    std::unique_ptr<NvencFramePool> pool;
    std::unique_ptr<PacketQueue> videoPackets, audioPackets;
    std::unique_ptr<CameraTimeMapper> mapper;
    const ClockMapper* masterClock = nullptr;
    std::int64_t cameraLatency = 0;
    std::uint64_t masterEpoch = 0;
    std::atomic<std::uint64_t> clockRevisions{0};
    std::uint64_t observedClockRevision = 0, reanchors = 0; // encoder worker only
    ClockMapper timestampReference{qpcFrequency()}; // Readiness only for externally mapped dubbing streams.
    std::unique_ptr<CameraClockMapper> cameraClock;
    std::atomic<bool> clockReady{false};
    std::function<std::int64_t()> acceptedLength;
    unsigned Fs = 48000;
    std::atomic<bool> gotFrame{false}, begun{false}, ending{false}, videoEnded{false}, audioEnded{false}, failedFlag{false}, muxFailed{false}, aborting{false}, thumbnail{false};
    std::atomic<std::int64_t> finalLength{0}, sourceFailure{-1}, available{0};
    std::atomic<std::uint64_t> overflow{0};
    std::thread encoderWorker, muxWorker;
    juce::var encoderReport, muxReport, cfrReport, inspection;
    std::string encoderError, muxError;
    bool thumbnailQueued = false; // encoder worker only
    IoHealth ioHealth;
    ThumbnailCache thumbnailWorker; // first thumbnail disk I/O is below original media work
    juce::File thumbnailPath() const
    { return finalFile.getParentDirectory().getChildFile(finalFile.getFileNameWithoutExtension() == "cam1" ? "index/first-thumbnail.bmp" : "index/cam2-first-thumbnail.bmp"); }
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
        const auto file = thumbnailPath();
        thumbnailQueued = thumbnailWorker.enqueue("first", [this, file, data = std::move(data)](const auto& yield)
        {
            if (yield()) return;
            DurableFile output; requireResult(output.open(file, DurableFile::OpenMode::createNew)); requireResult(output.write(data.data(), data.size()));
            requireResult(output.flushData()); requireResult(output.close()); thumbnail = true;
        });
    }
    void mux(const AVCodecContext& video, const AVCodecContext& audio, std::promise<void> prepared)
    {
        ScopedRecorderPriority priority(RecorderThreadRole::encodeWrite);
        bool signalled = false;
        std::unique_ptr<Mp4TakeWriter> writer;
        try
        {
            writer = std::make_unique<Mp4TakeWriter>(finalFile, video, audio, &ioHealth);
            prepared.set_value(); signalled = true;
            for (;;)
            {
                const bool done = videoEnded.load(std::memory_order_acquire) && audioEnded.load(std::memory_order_acquire);
                bool consumed = false;
                if (const auto* p = videoPackets->peek()) { writer->video(*p); videoPackets->release(); consumed = true; }
                if (const auto* p = audioPackets->peek()) { writer->audio(*p); audioPackets->release(); consumed = true; }
                if (!consumed) { if (done) break; waitBriefly(); }
            }
            if (!aborting.load())
            {
                ScopedRecorderPriority finalizerPriority(RecorderThreadRole::background);
                writer->finalize(); inspection = Mp4TakeWriter::inspect(finalFile);
            }
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
        ScopedRecorderPriority priority(RecorderThreadRole::encodeWrite);
        bool signalled = false;
        VideoCfrScheduler scheduler(nativeRate, {unsigned(profile.fps), 1});
        std::unique_ptr<NvencEncoder> encoder;
        int preroll = -1;
        unsigned invalidReanchorFrames = 0;
        try
        {
            encoder = std::make_unique<NvencEncoder>(profile); encoder->open();
            std::promise<void> muxReady; auto future = muxReady.get_future();
            muxWorker = std::thread([this, &audio, &encoder, ready = std::move(muxReady)]() mutable { mux(encoder->context(), audio, std::move(ready)); });
            future.get(); prepared.set_value(); signalled = true;
            const auto traceFile = finalFile.getParentDirectory().getChildFile("index/" + finalFile.getFileNameWithoutExtension() + "-source-ids.csv");
            std::ofstream trace(std::filesystem::path(traceFile.getFullPathName().toWideCharPointer()), std::ios::binary);
            trace.exceptions(std::ios::badbit | std::ios::failbit); trace << "pts,sourceId,mfPts100ns,callbackQpc,mapped100ns\n";
            const PacketSink sink = [this](const AVPacket& p)
            {
                if (muxFailed.load() || !videoPackets->push(p)) throw std::runtime_error("Take video packet queue/mux failure");
                available.store(rescaleRound(p.pts + 1, Fs, unsigned(profile.fps)));
            };
            const auto observe = [&](int slot, bool mapping)
            {
                const auto& stamp = pool->stamp(slot);
                const auto revision = pool->clockRevision(slot);
                if (!stamp.frame || stamp.callback <= 0)
                {
                    // A pending notification waits for a valid image, with a
                    // bounded grace period (one native second of bad frames).
                    if (revision == observedClockRevision || ++invalidReanchorFrames >=
                        (nativeRate.numerator + nativeRate.denominator - 1) / nativeRate.denominator)
                        throw std::runtime_error("Camera source persistently supplied invalid stamps");
                    return false;
                }
                invalidReanchorFrames = 0;
                if (revision != observedClockRevision)
                {
                    if (!cameraClock->reanchor(stamp)) return false;
                    if (mapping) mapper->reanchor();
                    scheduler.resetDeliveryDelay();
                    observedClockRevision = revision; ++reanchors;
                }
                else cameraClock->observe(stamp);
                return true;
            };
            const auto accept = [&](int slot)
            {
                std::int64_t time, availableTime;
                try
                {
                    if (!observe(slot, true)) { pool->release(slot); return; }
                    time = mapper->map(pool->stamp(slot));
                    availableTime = mapper->now(qpcNow());
                }
                catch (...) { pool->release(slot); throw; }
                if (time < 0)
                { if (preroll >= 0) pool->release(preroll); preroll = slot; return; }
                if (preroll >= 0)
                {
                    scheduler.push({pool->stamp(preroll).frame, 0, preroll}); preroll = -1;
                    if (time == 0) { pool->release(slot); return; }
                }
                scheduler.push({pool->stamp(slot).frame, time, slot}, availableTime);
            };
            for (;;)
            {
                if (aborting.load()) break;
                if (muxFailed.load()) throw std::runtime_error("Take mux failed");
                if (!begun.load(std::memory_order_acquire))
                {
                    int slot = -1;
                    while (pool->pop(slot))
                    {
                        try { if (!observe(slot, false)) { pool->release(slot); continue; } }
                        catch (...) { pool->release(slot); throw; }
                        const auto clock = cameraClock->snapshot(); clockReady = clock && clock->valid;
                        if (preroll >= 0) pool->release(preroll); preroll = slot;
                    }
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
                    const bool drain = ending.load();
                    const auto chosen = scheduler.select(drain ? 0 : mapper->now(qpcNow()), drain);
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
        int abandoned = -1; while (pool->pop(abandoned)) pool->release(abandoned);
        if (overflow.load()) scheduler.noteLoss(CfrReason::encodeLoss, overflow.load());
        thumbnailWorker.setRecording(false);
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
    bool shutdownRequested = false;
    std::uint64_t lifecycleGeneration = 0;
    Id ownerProject;
    std::vector<State> transitions{State::idle};
    juce::String failure, warning;
    Take take;
    std::vector<MediaAsset> assets;
    std::vector<unsigned> logicalMics;
    std::vector<int> logicalIndices;
    struct Camera
    {
        std::unique_ptr<ITakeVideoStream> video;
        std::atomic<ITakeVideoStream*> sink{nullptr};
        std::atomic<unsigned> offers{0};
        std::atomic<bool> active{false}, disconnected{false}, referenceFailed{false};
        std::atomic<std::uint64_t> generation{0}, staleOffers{0};
        std::shared_ptr<VideoSurfacePool> preview;
        std::shared_ptr<CaptureTelemetry> telemetry;
        std::unique_ptr<MfCameraCapture> capture;
        std::uint64_t discontinuities = 0, typeChanges = 0; // decode producer only
        juce::var report;
    };
    std::array<Camera, 2> cameras;
    unsigned cameraCount = 1;
    juce::String preparationNotice;
    std::unique_ptr<MfRuntime> mfRuntime; // remains alive with the preview capture after finalization
    std::future<juce::Result> work;
    RecorderDocument::Snapshot savedSnapshot;
    bool saving = false, partial = false, preparedAudio = false;
    std::int64_t requestedN0 = -1, length = 0, placement = 0, stopQpc = 0, prepareQpc = 0, finalizationQpc = 0;
    std::atomic<std::int64_t> collectionOrigin{-1};
    std::uint64_t masterEpoch = 0;
    double placementMs = 0, finalizationMs = 0, mediaFinalizationMs = 0, stopToDoneMs = 0;
    juce::Uuid placementEdit;
    juce::var audioReport;
    RecorderAudioEngine::DeviceInfo deviceSnapshot;
    std::vector<JournalDeviceMapping> mappingSnapshot;
    std::array<float, 8> peakSnapshot{};
    PlacementMetadata placementMetadata;
    Impl(RecorderDocument& d, RecorderAudioEngine& a, VideoFactory f) : document(d), audio(a), factory(std::move(f))
    { if (!factory) factory = [] { return std::make_unique<LiveTakeVideo>(); }; }
    ~Impl()
    {
        if (work.valid()) work.wait();
        cleanupFailedWork();
        for (auto& c : cameras) c.video.reset();
        document.setRecordingStructureLock(false);
    }
    void cleanupFailedWork()
    {
        detachSink();
        for (auto& c : cameras) { c.active = false; if (c.capture) try { c.capture->stop(); } catch (...) { partial = true; } }
        if (!preparedAudio) return;
        audio.abort(RecorderAudioEngine::Error::cancelled);
        // Each cleanup is independent: a failed encoder must not retain the journal lock.
        try { audio.finishCapture(placementEdit); } catch (...) { partial = true; }
        try { finishVideos(std::max<std::int64_t>(0, length)); } catch (...) { partial = true; }
        try { audio.finishJournal(false); } catch (...) { partial = true; }
        preparedAudio = false;
    }
    void failedWorker(const juce::Result& result)
    {
        failure = result.getErrorMessage() + juce::String::fromUTF8(" 녹화를 정지하고 프로젝트를 저장하세요"); partial = true;
        cleanupFailedWork(); document.setRecordingStructureLock(false);
        if (ownerProject == document.getProject().projectId)
        {
            if (document.getProject().media->findTake(take.takeId)) document.updateTakeState(take.takeId, TakeState::partial);
            if (saving && savedSnapshot) document.checkpointFinished(savedSnapshot, config.projectDirectory.getChildFile("project.recorder"), result);
        }
        saving = false; transition(State::partialFailure);
    }
    void transition(State next) { if (current != next) { current = next; transitions.push_back(next); } }
    CameraMode mode(unsigned i) const { return i ? config.camera2.mode : config.cameraMode; }
    std::string link(unsigned i) const { return i ? config.camera2.symbolicLink : config.cameraSymbolicLink; }
    bool synthetic(unsigned i) const { return i ? config.camera2.synthetic : config.synthetic; }
    juce::String cameraName(unsigned i) const { return i ? "cam2" : "cam1"; }
    void detachSink()
    {
        for (auto& c : cameras) c.sink.store(nullptr);
        for (auto& c : cameras) while (c.offers.load()) waitBriefly();
    }
    void offer(unsigned i, const VideoSurface& frame) noexcept
    {
        if (i >= cameras.size()) return;
        auto& c = cameras[i]; c.offers.fetch_add(1);
        if (auto* sink = c.sink.load())
        {
            auto generation = c.generation.load();
            if (!generation) { c.generation.compare_exchange_strong(generation, frame.stamp.generation); generation = c.generation.load(); }
            if (generation == frame.stamp.generation)
            {
                if (c.telemetry)
                {
                    const auto d = c.telemetry->count(LossReason::sourceDiscontinuity), t = c.telemetry->count(LossReason::sourceTypeChanged);
                    if (d != c.discontinuities || t != c.typeChanges) sink->discontinuity();
                    c.discontinuities = d; c.typeChanges = t;
                }
                sink->offer(frame);
            }
            else
            {
                ++c.staleOffers;
                if (frame.stamp.generation > generation)
                { c.disconnected = true; sink->sourceFailed(sink->availableSamples()); }
            }
        }
        c.offers.fetch_sub(1);
    }
    void failCamera(unsigned i, std::uint64_t generation) noexcept
    {
        if (i >= cameras.size()) return;
        auto& c = cameras[i]; c.offers.fetch_add(1);
        if (auto* sink = c.sink.load()) if (!generation || c.generation.load() == generation)
        {
            c.disconnected = true;
            const auto origin = collectionOrigin.load();
            sink->sourceFailed(origin < 0 ? 0 : std::max<std::int64_t>(0, audio.acceptedEnd() - origin));
        }
        c.offers.fetch_sub(1);
    }
    void finishVideos(std::int64_t end)
    {
        // Both lanes receive the same boundary BEFORE either join can wait.
        for (auto& c : cameras) if (c.video) { c.video->endAt(end); c.video->audioDone(); }
        for (auto& c : cameras) if (c.video)
        {
            try { c.video->finish(); c.report = c.video->report(); }
            catch (const std::exception& e)
            {
                partial = true; failure = juce::String::fromUTF8(e.what()); c.video->sourceFailed(c.video->availableSamples());
                c.report = jsonObject(); jsonSet(c.report,"finalizerError",e.what());
            }
            catch (...) { partial = true; failure = "Unknown video finalizer exception"; c.video->sourceFailed(c.video->availableSamples()); }
        }
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
            jsonSet(m, "physicalIndex", mappings[i].physicalIndex); jsonSet(m, "activeIndex", mappings[i].activeIndex);
            jsonSet(m, "leftPhysical", mappings[i].physicalIndex); jsonSet(m, "rightPhysical", mappings[i].rightPhysicalIndex); jsonSet(m, "channels", mappings[i].channels()); microphones.add(m);
        }
        for (auto peak : peaks) peakValues.add(double(peak));
        jsonSet(v, "microphones", microphones); jsonSet(v, "peaks", peakValues);
        jsonSet(v, "firstThumbnail", cameras[0].video && cameras[0].video->thumbnailReady() ? "index/first-thumbnail.bmp" : "");
        jsonSet(v, "audio", audioReport); jsonSet(v, "video", cameras[0].report);
        juce::Array<juce::var> cameraReports;
        for (unsigned i = 0; i < cameraCount; ++i)
        {
            const auto& c = cameras[i]; auto r = jsonObject(); jsonSet(r, "slot", int(i + 1));
            jsonSet(r, "symbolicLink", synthetic(i) ? "synthetic" : link(i)); jsonSet(r, "nativeMode", mode(i).text());
            jsonSet(r, "generation", jsonInt(c.generation.load())); jsonSet(r, "staleOffers", jsonInt(c.staleOffers.load()));
            jsonSet(r, "disconnected", c.disconnected.load()); jsonSet(r, "referenceFailed", c.referenceFailed.load());
            jsonSet(r, "N0", requestedN0); jsonSet(r, "Nstop", requestedN0 < 0 ? -1 : requestedN0 + length);
            jsonSet(r, "Lcam100ns", config.calibration[i] ? config.calibration[i]->cameraResidualLatency100ns : 0);
            jsonSet(r, "calibrationKeyMatched", config.calibration[i].has_value());
            if (config.calibration[i]) jsonSet(r, "calibration", config.calibration[i]->toJson());
            jsonSet(r, "assetId", i ? take.cam2AssetId : take.cam1AssetId); jsonSet(r, "video", c.report);
            juce::Array<juce::var> gaps;
            if (i < assets.size()) for (const auto& gap : assets[i].gaps) { auto g = jsonObject(); jsonSet(g, "start", gap.start); jsonSet(g, "length", gap.length); gaps.add(g); }
            jsonSet(r, "gaps", gaps); cameraReports.add(r);
        }
        jsonSet(v, "cameras", cameraReports); jsonSet(v, "camera2Active", cameras[1].active.load());
        jsonSet(v, "clockMapping", "Independent camera fits / Lcam; shared live robust ASIO clock; physical timing requires measured profiles");
        jsonSet(v, "masterEpoch", jsonInt(masterEpoch));
        return v;
    }
    void initialiseAssets()
    {
        const auto device = audio.deviceInfo(); deviceSnapshot = device; mappingSnapshot = audio.microphoneMapping(); peakSnapshot.fill(0);
        take = {}; take.takeId = config.takeId.toString();
        take.capture.asioDeviceId = device.name; take.capture.calibrationIdentity = "live-master-camera-fit/full-key-profiles-or-unmeasured";
        assets.clear(); logicalMics = audio.armedMicrophones(); logicalIndices.clear();
        for (unsigned i = 0; i < cameraCount; ++i)
        {
            MediaAsset camera; camera.kind = AssetKind::camera;
            (i ? take.cam2AssetId : take.cam1AssetId) = camera.assetId;
            take.capture.cameraDeviceIds[i] = juce::String(synthetic(i) ? "synthetic" : link(i)); take.capture.cameraModes[i] = mode(i).text();
            if (config.calibration[i]) take.capture.cameraOffsetSamples[i] = rescaleRound(config.calibration[i]->cameraResidualLatency100ns, device.sampleRate, 10000000);
            camera.relativePath = "media/takes/" + config.takeId.toDashedString() + "/" + cameraName(i) + ".recording.mp4";
            camera.contentIdentity = camera.assetId; camera.originalFormat.codec = "h264";
            camera.originalFormat.width = 1920; camera.originalFormat.height = 1080; camera.originalFormat.fps = {unsigned(config.projectFps), 1};
            camera.sourceUnitsNumerator = unsigned(config.projectFps); camera.sourceUnitsDenominator = device.sampleRate; assets.push_back(camera);
        }
        for (std::size_t i = 0; i < logicalMics.size(); ++i)
        {
            const auto mapping = mappingSnapshot[i];
            MediaAsset mic; mic.kind = AssetKind::mic; mic.contentIdentity = mic.assetId;
            mic.originalFormat.codec = "pcm_s24le"; mic.originalFormat.sampleRate = device.sampleRate;
            mic.originalFormat.channels = int(mapping.channels()); mic.originalFormat.bitsPerSample = 24;
            take.microphoneAssetIds.push_back(mic.assetId); take.capture.physicalInputs.push_back(mapping.physicalIndex); take.capture.physicalInputsRight.push_back(mapping.rightPhysicalIndex);
            logicalIndices.push_back(int(logicalMics[i]) - 1); assets.push_back(mic);
        }
    }
    juce::Result prepareWorker()
    {
        try
        {
            detachSink();
            for (auto& c : cameras) { c.capture.reset(); c.video.reset(); c.preview.reset(); c.telemetry.reset(); c.discontinuities = c.typeChanges = 0; }
            cameraCount = config.camera2.enabled && (config.camera2.synthetic || !config.camera2.symbolicLink.empty()) ? 2u : 1u;
            for (unsigned i = 0; i < cameraCount; ++i)
            {
                auto& c = cameras[i];
                c.telemetry = i ? config.camera2.telemetry : config.cameraTelemetry;
                if (!config.externalCapture) c.preview = std::make_shared<VideoSurfacePool>(1920, 1080);
                if (!synthetic(i) && !config.externalCapture)
                {
                    try
                    {
                        if (!mfRuntime) mfRuntime = std::make_unique<MfRuntime>();
                        c.telemetry = std::make_shared<CaptureTelemetry>(mode(i).fps);
                        c.capture = std::make_unique<MfCameraCapture>(c.telemetry, *c.preview, [this, i](const VideoSurface& f) { offer(i, f); });
                        c.capture->start(link(i), mode(i), mode(i).subtype == CaptureSubtype::mjpeg, 1, {}, c.generation.load());
                        c.generation = c.capture->generation();
                    }
                    catch (const std::exception& e)
                    {
                        if (i == 0) throw;
                        c.capture.reset(); c.preview.reset(); cameraCount = 1;
                        preparationNotice = juce::String::fromUTF8("캠2 연결을 확인하세요. 캠1으로 녹화합니다. ") + juce::String::fromUTF8(e.what());
                    }
                }
            }
            // Resolve optional source availability before any cam2 asset, encoder,
            // journal planned-file entry or MP4 is created.
            initialiseAssets();
            requireResult(takeFolder().createDirectory()); requireResult(takeFolder().getChildFile("index").createDirectory());
            for (const char* path : {"media/imports", "cache", "recovery", "exports"}) requireResult(config.projectDirectory.getChildFile(path).createDirectory());
            RecorderAudioEngine::TakeConfig audioConfig; audioConfig.projectDirectory = config.projectDirectory; audioConfig.takeId = config.takeId;
            audioConfig.faults = config.faults;
            audioConfig.placementSample = placement; audioConfig.microphoneAssetIds = take.microphoneAssetIds;
            for (unsigned i = 0; i < cameraCount; ++i)
                audioConfig.additionalFiles.push_back({assets[i].assetId, assets[i].relativePath,
                    "media/takes/" + config.takeId.toDashedString() + "/" + cameraName(i) + ".mp4"});
            audioConfig.referencePackets = [this](const AVPacket& p)
            {
                for (auto& c : cameras) if (c.video && !c.referenceFailed.load())
                {
                    try { c.video->audioPacket(p); }
                    catch (...) { c.referenceFailed = true; c.video->sourceFailed(c.video->availableSamples()); }
                }
            };
            requireResult(audio.prepare(std::move(audioConfig))); preparedAudio = true;
            for (unsigned i = 0; i < cameraCount; ++i)
            {
                auto& c = cameras[i]; c.video = factory(); c.active = true;
                try
                {
                    c.video->configureClock(audio.masterClock(), config.calibration[i] ? config.calibration[i]->cameraResidualLatency100ns : 0);
                    c.video->prepare(takeFolder().getChildFile(cameraName(i) + ".mp4"), NvencProfile{config.projectFps}, mode(i).fps, *audio.referenceContext());
                    c.sink.store(c.video.get());
                }
                catch (const std::exception& e)
                {
                    if (i == 0) throw;
                    c.referenceFailed = true; c.video->sourceFailed(0); partial = true;
                    preparationNotice = juce::String::fromUTF8("캠2 녹화를 준비할 수 없습니다. 캠1과 원본 녹음은 계속됩니다. ") + juce::String::fromUTF8(e.what());
                }
            }
            writeJsonDurable(takeFolder().getChildFile("take.json"), manifest("preparing"));
            return juce::Result::ok();
        }
        catch (...)
        {
            const auto result = takeException("Take preparation failed"); cleanupFailedWork(); return result;
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
                             cameras[0].video && cameras[0].video->thumbnailReady() ? takeFolder().getChildFile("index/first-thumbnail.bmp") : juce::File()};
        placementMetadata.waveform = audio.peakCache();
        if (length <= 0)
        {
            partial = true; failure = "No nonempty callback-confirmed take range";
        }
        else
        {
            take.N0 = audio.startSample(); requestedN0 = take.N0; take.logicalLength = length;
            take.placementSample = placement; take.state = TakeState::finalising;
            for (auto& asset : assets) setRanges(asset, length);
            for (unsigned i = 0; i < cameraCount; ++i)
                if (cameras[i].video->failed()) setRanges(assets[i], cameras[i].video->availableSamples());
            for (std::size_t i = 0; i < logicalMics.size(); ++i) setChunks(assets[i + cameraCount], logicalMics[i], length);
            const auto result = document.placeRecordedTake(take, assets, logicalIndices);
            if (result.failed()) { partial = true; failure = result.getErrorMessage(); }
            else { take = *document.getProject().media->findTake(take.takeId); placementEdit = juce::Uuid(document.lastEditTransaction()); }
        }
        document.setRecordingStructureLock(false);
        placementMs = elapsedMs(stopQpc); finalizationQpc = qpcNow(); transition(State::finalizing);
        try
        {
        work = launchTakeWorker("finalize", [this]
        {
            const auto start = qpcNow();
            try
            {
                // Close video intake after at most one native period. Waiting for
                // WAV/AAC drain first can fill the bounded video pool with frames
                // beyond Nstop while CFR is already at its final output frame.
                double period = mode(0).fps.periodMs();
                if (cameraCount == 2) period = std::max(period, mode(1).fps.periodMs());
                const auto deadline = qpcNow() + std::int64_t(period * double(qpcFrequency()) / 1000.0);
                while (qpcNow() < deadline) waitBriefly();
                detachSink();
                for (auto& c : cameras) if (c.video) c.video->endAt(length);
                // Preview captures keep running; both encoders can drain while
                // TakeStopped and the raw/AAC tail are committed independently.
                const auto audioResult = audio.finishCapture(placementEdit);
                if (audioResult.failed()) partial = true;
                audioReport = audio.telemetry(); peakSnapshot = audio.peaks();
                finishVideos(length);
                partial = partial || audio.referenceFailed();
                for (unsigned i = 0; i < cameraCount; ++i)
                {
                    auto& c = cameras[i]; partial = partial || c.video->failed(); setRanges(assets[i], c.video->availableSamples());
                    const auto name = cameraName(i) + ".mp4";
                    if (takeFolder().getChildFile(name).existsAsFile()) assets[i].relativePath = "media/takes/" + config.takeId.toDashedString() + "/" + name;
                    else setRanges(assets[i], 0);
                }
                const auto written = std::int64_t(audioReport["wav"]["writtenSamplesPerMic"]);
                for (std::size_t i = 0; i < logicalMics.size(); ++i)
                { setRanges(assets[i + cameraCount], written); setChunks(assets[i + cameraCount], logicalMics[i], std::min(length, written)); }
                for (auto& asset : assets) ++asset.mediaGeneration;
                mediaFinalizationMs = elapsedMs(start); return audioResult;
            }
            catch (...) { partial = true; mediaFinalizationMs = elapsedMs(start); throw; }
        });
        }
        catch (...) { failedWorker(takeException("Take finalization could not start")); }
    }
};
TakeController::TakeController(RecorderDocument& d, RecorderAudioEngine& a, VideoFactory f) : impl(std::make_unique<Impl>(d, a, std::move(f))) {}
TakeController::~TakeController() = default;
juce::Result TakeController::reset()
{
    if (impl->work.valid() || (state() != State::idle && state() != State::done && state() != State::partialFailure))
        return juce::Result::fail("Take is still active");
    impl->detachSink(); ++impl->lifecycleGeneration; impl->shutdownRequested = false; impl->placementMetadata = {}; impl->length = 0; impl->placement = 0;
    impl->failure.clear(); impl->warning.clear(); impl->ownerProject.clear(); impl->current = State::idle;
    for (auto& c : impl->cameras) c.active = false;
    return juce::Result::ok();
}
juce::Result TakeController::prepare(Config config)
{
    auto& s = *impl;
    if (s.shutdownRequested) return juce::Result::fail("Take shutdown blocks new commands");
    if (s.current != State::idle && s.current != State::done && s.current != State::partialFailure) return juce::Result::fail("Take controller is busy");
    if (s.work.valid()) return juce::Result::fail("Previous worker completion must be consumed");
    const auto device = s.audio.deviceInfo();
    if (!device.sampleRate || config.projectDirectory == juce::File() || config.takeId.isNull()
        || (config.projectFps != 30 && config.projectFps != 60) || config.cameraMode.width != 1920 || config.cameraMode.height != 1080
        || !config.cameraMode.fps.numerator || !config.cameraMode.fps.denominator || (!config.synthetic && config.cameraSymbolicLink.empty()))
        return juce::Result::fail("Select a ready ASIO device and cam1 1080p native mode / project 30 or 60");
    if (config.camera2.enabled && (config.camera2.synthetic || !config.camera2.symbolicLink.empty()))
    {
        const auto& m = config.camera2.mode;
        if (m.width != 1920 || m.height != 1080 || !m.fps.numerator || !m.fps.denominator)
            return juce::Result::fail("Select a cam2 1080p native mode");
        if (!config.synthetic && !config.camera2.synthetic && CameraCatalog::sameDevice(config.cameraSymbolicLink, config.camera2.symbolicLink))
            return juce::Result::fail(juce::String::fromUTF8("같은 카메라를 두 번 선택할 수 없습니다."));
    }
    try
    {
        const unsigned count = config.camera2.enabled && (config.camera2.synthetic || !config.camera2.symbolicLink.empty()) ? 2 : 1;
        for (unsigned i = 0; i < count; ++i) if (config.calibration[i])
            config.calibration[i]->requireMatch(calibrationKey(i ? config.camera2.symbolicLink : config.cameraSymbolicLink,
                i ? config.camera2.mode : config.cameraMode, config.exposure[i], device.name.toStdString(),
                device.sampleRate, device.bufferFrames, config.outputMapping, s.audio.calibrationInputMapping()));
    }
    catch (const std::exception& e) { return juce::Result::fail(e.what()); }
    const auto timebase = s.document.setTimebase(device.sampleRate, {unsigned(config.projectFps), 1});
    if (timebase.failed()) return timebase;
    if (s.document.getProject().media->assets.empty())
    {
        // The first take fixes the project rate: persist it before TakeStarted so a crash during this take still recovers
        // (RecoveryScanner compares the journal rate with the checkpoint on disk). A failed save does not start recording.
        // Through the document so an attached journal worker writes state and cursor together (checkpointAndWait);
        // without a worker this is the serializer's own flushed write plus checkpointFinished.
        const auto saved = s.document.saveCheckpoint(config.projectDirectory.getChildFile("project.recorder"));
        if (saved.failed()) return juce::Result::fail(juce::String::fromUTF8("프로젝트를 저장할 수 없어 녹화를 시작하지 않습니다. ") + saved.getErrorMessage());
    }
    s.config = std::move(config); s.failure.clear(); s.warning.clear(); s.partial = false; s.saving = false; s.preparedAudio = false;
    s.ownerProject = s.document.getProject().projectId; ++s.lifecycleGeneration;
    s.audioReport = juce::var(); s.assets.clear(); s.take = Take{}; s.logicalMics.clear(); s.logicalIndices.clear();
    s.mappingSnapshot.clear(); s.deviceSnapshot = device; s.placementMetadata = {}; s.transitions = {State::idle}; s.current = State::idle;
    s.requestedN0 = -1; s.collectionOrigin = -1; s.masterEpoch = 0; s.length = 0; s.stopQpc = s.finalizationQpc = 0;
    s.placementMs = s.finalizationMs = s.mediaFinalizationMs = s.stopToDoneMs = 0; s.placementEdit = juce::Uuid();
    s.placement = s.document.getProject().activeTimelineEnd(); s.preparationNotice.clear();
    s.detachSink();
    for (auto& c : s.cameras) { c.active = false; c.disconnected = false; c.referenceFailed = false; c.staleOffers = 0; c.report = juce::var(); }
    s.cameras[0].generation = s.config.cameraGeneration; s.cameras[1].generation = s.config.camera2.generation;
    if (s.audio.armedMicrophones().empty()) s.warning = juce::String::fromUTF8("녹음 중인 마이크가 없습니다");
    try
    {
        s.document.setRecordingStructureLock(true); s.transition(State::preparing); s.prepareQpc = qpcNow();
        s.work = launchTakeWorker("prepare", [&s] { return s.prepareWorker(); }); return juce::Result::ok();
    }
    catch (...)
    {
        const auto result = takeException("Take preparation could not start");
        s.document.setRecordingStructureLock(false); s.transition(State::idle); s.ownerProject.clear();
        s.failure = result.getErrorMessage(); return result;
    }
}
juce::Result TakeController::start(std::int64_t N0)
{
    auto& s = *impl;
    if (s.shutdownRequested) return juce::Result::fail("Take shutdown blocks start");
    if (s.current != State::armed || s.requestedN0 >= 0) return juce::Result::fail("Take must be armed before start");
    const auto device = s.audio.deviceInfo(); if (N0 < 0) N0 = s.audio.currentSample() + std::max<std::int64_t>(device.sampleRate / 4, device.bufferFrames * 2);
    if (!s.audio.clockReady()) return juce::Result::fail("ASIO clock is not stable");
    const auto master = s.audio.masterClock().snapshot();
    s.masterEpoch = master ? master->epoch : 0;
    const auto result = s.audio.startAt(N0); if (result.failed()) return result;
    s.requestedN0 = N0; s.collectionOrigin = N0;
    const auto clock = s.audio.clockMapping();
    for (auto& c : s.cameras) if (c.active.load() && c.video && !c.video->failed())
        try { c.video->startAt(clock, N0, device.sampleRate, [&s]
        { return s.audio.startSample() >= 0 ? std::max<std::int64_t>(0, s.audio.acceptedEnd() - s.audio.startSample()) : 0; }); }
        catch (...) { c.video->sourceFailed(0); s.partial = true; }
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
    // A completed worker owns its old project files, never the newly adopted model.
    if (s.ownerProject.isNotEmpty() && s.ownerProject != s.document.getProject().projectId)
    {
        if (s.work.valid() && s.work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
        if (s.work.valid()) try { s.work.get(); } catch (...) { s.failure = takeException("Stale take worker failed").getErrorMessage(); }
        s.cleanupFailedWork();
        s.shutdownRequested = true; s.transition(State::partialFailure); s.ownerProject.clear(); return;
    }
    if (s.work.valid())
    {
        if (s.work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
        auto result = juce::Result::ok();
        try { result = s.work.get(); }
        catch (...) { s.failedWorker(takeException("Take worker failed")); return; }
        if (result.failed()) { s.failure = result.getErrorMessage(); s.partial = true; }
        if (s.current == State::preparing && result.failed())
        { s.document.setRecordingStructureLock(false); s.transition(State::partialFailure); return; }
        if (s.current == State::preparing && s.preparationNotice.isNotEmpty()) s.warning = s.preparationNotice;
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
            if (s.cameras[0].video && s.cameras[0].video->thumbnailReady()) s.placementMetadata.firstThumbnail = s.takeFolder().getChildFile("index/first-thumbnail.bmp");
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
            try
            {
            s.work = launchTakeWorker("save", [&s]
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
                catch (...) { throw; } // get() owns cleanup and checkpoint failure publication
            });
            }
            catch (...) { s.failedWorker(takeException("Take save could not start")); }
            return;
        }
    }
    if (s.current == State::preparing)
    {
        if (s.shutdownRequested && !s.work.valid())
        {
            s.audio.endAtConfirmedBoundary(); s.transition(State::stopping);
        }
    }
    if (s.shutdownRequested && (s.current == State::armed || s.current == State::recording || s.current == State::stopping))
    {
        s.audio.endAtConfirmedBoundary(); s.transition(State::stopping);
    }
    if (s.current == State::preparing)
    {
        for (unsigned i = 0; i < s.cameraCount; ++i)
            if (s.cameras[i].capture && (s.cameras[i].capture->failureDetected() || s.cameras[i].capture->finished())) cameraFailed(i);
        const bool primaryReady = s.audio.clockReady() && s.cameras[0].video && s.cameras[0].video->ready();
        if (primaryReady && s.cameraCount == 2 && !s.cameras[1].video->ready() && elapsedMs(s.prepareQpc) > 10000) cameraFailed(1);
        if (primaryReady && (s.cameraCount == 1 || s.cameras[1].video->ready() || s.cameras[1].video->failed())) s.transition(State::armed);
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
        if (s.masterEpoch)
            if (const auto master = s.audio.masterClock().snapshot(); master && master->epoch != s.masterEpoch)
                s.audio.abort(RecorderAudioEngine::Error::clockDiscontinuity);
        unsigned failedCameras = 0;
        for (unsigned i = 0; i < s.cameraCount; ++i)
        {
            auto& c = s.cameras[i];
            if (c.capture && (c.capture->failureDetected() || c.capture->finished())) cameraFailed(i);
            if (s.audio.referenceFailed()) c.video->sourceFailed(c.video->availableSamples());
            if (c.video->storageFailed()) s.audio.abort(RecorderAudioEngine::Error::writeFailed);
            if (c.video->failed())
            {
                s.partial = true; failedCameras |= 1u << i;
            }
        }
        if (failedCameras == 3) s.warning = juce::String::fromUTF8("두 카메라의 영상 녹화를 중단했습니다. 원본 녹음은 계속됩니다.");
        else if (failedCameras)
        {
            const auto i = failedCameras == 2 ? 1u : 0u;
            s.warning = juce::String::fromUTF8(i ? "캠2" : "캠1") + juce::String::fromUTF8(s.cameras[i].disconnected.load()
                ? " 연결이 끊겼습니다. " : " 영상 녹화를 중단했습니다. ");
            if (s.cameraCount == 2) s.warning += juce::String::fromUTF8(i ? "캠1과 " : "캠2와 ");
            s.warning += juce::String::fromUTF8("원본 녹음은 계속됩니다.");
        }
        if (s.current == State::armed && s.audio.startSample() >= 0) s.transition(State::recording);
        if (s.audio.error() != RecorderAudioEngine::Error::none)
        {
            using E = RecorderAudioEngine::Error;
            const auto fault = s.audio.error();
            s.failure = recorderFaultText(fault == E::writeFailed ? RecorderFault::storageWrite
                : fault == E::rawOverflow || fault == E::pcmOverflow ? RecorderFault::audioOverflow
                : fault == E::sampleRateChanged ? RecorderFault::audioRateChanged
                : fault == E::asioReset ? RecorderFault::audioReset : RecorderFault::audioInput);
            s.partial = true; s.transition(State::stopping);
        }
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
void TakeController::offer(const VideoSurface& frame) noexcept { impl->offer(0, frame); }
void TakeController::offer(unsigned camera, const VideoSurface& frame) noexcept { impl->offer(camera, frame); }
void TakeController::cameraFailed(unsigned camera, std::uint64_t generation) noexcept { impl->failCamera(camera, generation); }
void TakeController::cameraDiscontinuity(unsigned camera, std::uint64_t generation) noexcept
{
    if (camera >= 2) return;
    auto& c = impl->cameras[camera]; c.offers.fetch_add(1);
    if (auto* sink = c.sink.load()) if (!generation || generation == c.generation.load()) sink->discontinuity();
    c.offers.fetch_sub(1);
}
std::shared_ptr<VideoSurfacePool> TakeController::previewPool(unsigned camera) const
{ return camera < 2 && !(state() == State::preparing && impl->work.valid()) ? impl->cameras[camera].preview : nullptr; }
bool TakeController::cameraActive(unsigned camera) const noexcept
{ return camera < 2 && impl->cameras[camera].active.load(); }
bool TakeController::cameraDisconnected(unsigned camera) const noexcept
{ return camera < 2 && impl->cameras[camera].disconnected.load(); }
TakeVideoQueues TakeController::cameraQueues(unsigned camera) const noexcept
{
    if (camera >= 2) return {};
    auto& c = impl->cameras[camera]; c.offers.fetch_add(1);
    const auto* sink = c.sink.load(); const auto result = sink ? sink->queues() : TakeVideoQueues{};
    c.offers.fetch_sub(1); return result;
}
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
    jsonSet(v, "cam2", s.cameras[1].active.load() ? "active; independent encoder and file" : "disabled; no encoder or file"); jsonSet(v, "projectDirectory", s.config.projectDirectory.getFullPathName());
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
                                                                 const juce::String&)
{ return std::make_unique<LiveTakeVideo>(std::move(mapper)); }
void TakeController::requestShutdown()
{
    auto& s = *impl;
    if (s.shutdownRequested) return;
    s.shutdownRequested = true; s.stopQpc = qpcNow();
    // Preparation and finalization own session pointers until their result is ready.
    // tick performs the collection boundary as soon as that ownership returns.
}
bool TakeController::shutdownComplete() const noexcept
{ return !impl->work.valid() && (state() == State::idle || state() == State::done || state() == State::partialFailure); }
std::uint64_t TakeController::generation() const noexcept { return impl->lifecycleGeneration; }
bool TakeController::processingDelayed() const noexcept
{
    bool delayed = false;
    for (auto& c : impl->cameras)
    {
        c.offers.fetch_add(1); if (auto* video = c.sink.load()) delayed |= video->processingDelayed(); c.offers.fetch_sub(1);
    }
    return delayed;
}
}
