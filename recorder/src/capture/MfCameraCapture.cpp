#include "MfCameraCapture.h"
#include "support/BoundedSpscQueue.h"
#include <future>
#include <chrono>

namespace gocue::recorder
{
namespace
{
struct SampleEnvelope
{
    IMFSample* sample = nullptr;
    FrameStamp stamp;
};
struct CallbackState
{
    explicit CallbackState(CaptureTelemetry& t) : telemetry(t)
    {
        sampleReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sampleReady) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Create prepared sample wake event");
    }
    ~CallbackState() { if (sampleReady) CloseHandle(sampleReady); }
    BoundedSpscQueue<SampleEnvelope, 2> queue;
    // Valid while accepting=true. The owner retains it until accepting=false + active=0.
    // Late callbacks return before touching it; no large telemetry destruction on MF threads.
    CaptureTelemetry& telemetry;
    HANDLE sampleReady = nullptr;
    std::atomic<bool> accepting{false}, flushed{false};
    std::atomic<unsigned> active{0};
    IMFSourceReader* reader = nullptr; // worker retains reader through rearm barrier + Flush
    std::uint64_t nextFrame = 0, generation = 0;
};
class ReaderCallback final : public IMFSourceReaderCallback
{
public:
    explicit ReaderCallback(std::shared_ptr<CallbackState> s) : state(std::move(s)) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFSourceReaderCallback)) return E_NOINTERFACE;
        *value = static_cast<IMFSourceReaderCallback*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return references.fetch_add(1) + 1; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const auto count = references.fetch_sub(1) - 1;
        if (!count) delete this;
        return count;
    }
    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG pts, IMFSample* sample) override
    {
        const auto entered = qpcNow();
        auto& s = *state;
        s.active.fetch_add(1); // seq_cst handshake with the worker's stop barrier
        if (!s.accepting.load()) { s.active.fetch_sub(1); return S_OK; }
        auto& t = s.telemetry;
        t.callbacks.fetch_add(1, std::memory_order_relaxed);
        // No allocation, locks, attribute lookup, buffer access, Release, JSON or GPU here.
        // MF-specific permitted COM calls: retained sample AddRef and async ReadSample.
        if (FAILED(status) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM
                                       | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)))
        {
            t.sourceStatus.store(FAILED(status) ? status : E_FAIL);
            if (FAILED(status) || (flags & MF_SOURCE_READERF_ERROR)) t.loss(LossReason::sourceError);
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) t.loss(LossReason::endOfStream);
            if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) t.loss(LossReason::sourceTypeChanged);
            s.accepting.store(false);
        }
        else
        {
            if (flags & MF_SOURCE_READERF_STREAMTICK) t.loss(LossReason::sourceStreamTick);
            if (sample)
            {
                const auto frame = ++s.nextFrame;
                if (frame == 1) t.firstCallbackQpc.store(entered);
                t.samples.fetch_add(1, std::memory_order_relaxed);
                if (auto* slot = s.queue.reserve())
                {
                    sample->AddRef();
                    *slot = {};
                    slot->sample = sample;
                    slot->stamp.frame = frame; slot->stamp.generation = s.generation;
                    slot->stamp.pts100ns = pts; slot->stamp.callback = entered; slot->stamp.enqueued = qpcNow();
                    s.queue.commit();
                    const auto size = s.queue.producerSize();
                    if (size > t.queueHighWater.load(std::memory_order_relaxed)) t.queueHighWater.store(size, std::memory_order_relaxed);
                    SetEvent(s.sampleReady); // signal only; all waits are on the consumer
                }
                else t.loss(LossReason::captureDecodeOverflow); // borrowed sample: no app Release required
            }
            if (s.accepting.load())
            {
                const auto hr = s.reader->ReadSample(firstVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
                if (FAILED(hr)) { t.sourceStatus.store(hr); t.loss(LossReason::sourceError); s.accepting.store(false); }
            }
        }
        if (!s.accepting.load()) SetEvent(s.sampleReady);
        s.active.fetch_sub(1);
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { state->flushed.store(true); return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
private:
    std::atomic<ULONG> references{1};
    std::shared_ptr<CallbackState> state;
};
CaptureOpenInfo configureReader(IMFSourceReader* reader, CameraMode requested, bool mfDecode)
{
    checkHr(reader->SetStreamSelection(allSourceStreams, FALSE), "Deselect streams");
    checkHr(reader->SetStreamSelection(firstVideoStream, TRUE), "Select camera stream");
    ComPtr<IMFMediaType> native;
    CameraMode actualNative;
    for (DWORD index = 0; ; ++index)
    {
        ComPtr<IMFMediaType> type;
        const auto hr = reader->GetNativeMediaType(firstVideoStream, index, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        checkHr(hr, "GetNativeMediaType(capture)");
        auto mode = CameraCatalog::readMode(type.Get(), index);
        if (mode && mode->sameSignal(requested)) { native = type; actualNative = *mode; break; }
    }
    if (!native) throw std::runtime_error("Selected native mode no longer exists: " + requested.text());
    ComPtr<IMFSourceReaderEx> extended;
    checkHr(reader->QueryInterface(IID_PPV_ARGS(&extended)), "IMFSourceReaderEx (pin native input)");
    DWORD flags = 0;
    checkHr(extended->SetNativeMediaType(firstVideoStream, native.Get(), &flags), "SetNativeMediaType");
    checkHr(reader->SetCurrentMediaType(firstVideoStream, nullptr, native.Get()), "Select native output");
    if (mfDecode)
    {
        if (requested.subtype != CaptureSubtype::mjpeg) throw std::invalid_argument("MF decoder comparison requires a native MJPEG mode");
        ComPtr<IMFMediaType> output;
        checkHr(MFCreateMediaType(&output), "Create NV12 output type");
        checkHr(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video output");
        checkHr(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Set NV12 output");
        checkHr(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, requested.width, requested.height), "Set output size");
        checkHr(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, requested.fps.numerator, requested.fps.denominator), "Set output FPS");
        checkHr(reader->SetCurrentMediaType(firstVideoStream, nullptr, output.Get()), "MF MJPEG -> NV12 negotiation");
        // Validate that an MJPEG decoder really was inserted; successful NV12 negotiation
        // alone would not prove the source did not switch to its native NV12 mode.
        GUID category{};
        ComPtr<IMFTransform> transform;
        checkHr(extended->GetTransformForStream(firstVideoStream, 0, &category, &transform), "Locate MF MJPEG decoder");
        if (category != MFT_CATEGORY_VIDEO_DECODER) throw std::runtime_error("MF comparison did not insert a video decoder");
        ComPtr<IMFMediaType> decoderInput;
        checkHr(transform->GetInputCurrentType(0, &decoderInput), "Read MF decoder input type");
        const auto input = CameraCatalog::readMode(decoderInput.Get());
        GUID inputSubtype{};
        checkHr(decoderInput->GetGUID(MF_MT_SUBTYPE, &inputSubtype), "MF decoder input subtype");
        if (inputSubtype != MFVideoFormat_MJPG || (input && !input->sameSignal(requested)))
            throw std::runtime_error("MF comparison decoder input is not the selected native MJPEG signal");
    }
    ComPtr<IMFMediaType> current;
    checkHr(reader->GetCurrentMediaType(firstVideoStream, &current), "Get actual output type");
    const auto actual = CameraCatalog::readMode(current.Get());
    auto expected = requested; if (mfDecode) expected.subtype = CaptureSubtype::nv12;
    if (!actual || !actual->sameSignal(expected)) throw std::runtime_error("SourceReader output differs from exact requested subtype/size/rational FPS");
    return {actualNative, *actual, mfDecode, 0, mfDecode ? "MF SourceReader MJPEG -> NV12 (pre-callback)" : "native / FFmpeg CPU MJPEG"};
}
}
struct MfCameraCapture::State
{
    std::shared_ptr<CaptureTelemetry> telemetry;
    VideoSurfacePool& pool;
    std::shared_ptr<CallbackState> callback;
    std::atomic<bool> stopRequested{false}, done{true};
    std::thread worker;
    std::string error, colour;
    std::function<void(const VideoSurface&)> recordSink;
    State(std::shared_ptr<CaptureTelemetry> t, VideoSurfacePool& p, std::function<void(const VideoSurface&)> sink)
        : telemetry(std::move(t)), pool(p), recordSink(std::move(sink)) {}
    void run(std::string link, CameraMode mode, bool mfDecode, int threads, std::promise<CaptureOpenInfo> opened)
    {
        bool openReported = false;
        try
        {
            ComApartment apartment;
            ComPtr<IMFMediaSource> source;
            ComPtr<IMFSourceReader> reader;
            ComPtr<ReaderCallback> cb;
            cb.Attach(new ReaderCallback(callback));
            // Shutdown stays on this worker on success AND partial-open/decoder failures.
            try
            {
                source = CameraCatalog::openSource(link);
                ComPtr<IMFAttributes> attributes;
                checkHr(MFCreateAttributes(&attributes, 7), "Capture attributes");
                checkHr(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb.Get()), "Set async callback");
                checkHr(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "Set low latency");
                checkHr(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, mfDecode ? FALSE : TRUE), "Set converter policy");
                checkHr(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE), "Disable software RGB processing");
                checkHr(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE), "Disable advanced video processing");
                checkHr(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE), "Use controlled CPU MF comparison");
                checkHr(MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader), "Create async source reader");
                auto info = configureReader(reader.Get(), mode, mfDecode);
                CaptureFrameDecoder decoder(info.outputMode, threads);
                info.decoderThreads = decoder.effectiveThreads();
                callback->reader = reader.Get(); callback->accepting.store(true);
                checkHr(reader->ReadSample(firstVideoStream, 0, nullptr, nullptr, nullptr, nullptr), "Initial ReadSample");
                opened.set_value(info); openReported = true;
                std::int64_t previousPts = 0, previousQpc = 0;
                std::uint64_t previousFrame = 0;
                while (!stopRequested.load() && callback->accepting.load())
                {
                    SampleEnvelope envelope;
                    if (!callback->queue.pop(envelope))
                    {
                        // Sleep(1) can round to the Windows timer quantum and miss the
                        // 2ms queue budget. The prepared auto-reset event wakes on commit.
                        if (WaitForSingleObject(callback->sampleReady, 25) == WAIT_FAILED)
                            checkHr(HRESULT_FROM_WIN32(GetLastError()), "Wait for retained sample");
                        continue;
                    }
                    ComPtr<IMFSample> sample; sample.Attach(envelope.sample);
                    auto& stamp = envelope.stamp;
                    stamp.worker = qpcNow();
                    UINT64 deviceTime = 0;
                    stamp.hasDeviceTimestamp = SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &deviceTime));
                    stamp.deviceTimestamp100ns = deviceTime;
                    if (!stamp.hasDeviceTimestamp) telemetry->missingDeviceTimestamp.fetch_add(1);
                    else
                    {
                        const auto ageMs = telemetry->ms(stamp.callback) - static_cast<double>(deviceTime) / 10000.0;
                        stamp.deviceTimestampValid = deviceTime != 0 && ageMs >= 0;
                        if (stamp.deviceTimestampValid) telemetry->duration(Timing::deviceToCallback, ageMs);
                        else telemetry->invalidDeviceTimestamp.fetch_add(1);
                    }
                    UINT32 discontinuity = 0;
                    if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_Discontinuity, &discontinuity)) && discontinuity)
                        telemetry->loss(stamp.frame == 1 ? LossReason::startupDiscontinuity : LossReason::sourceDiscontinuity);
                    const double wait = telemetry->ms(stamp.worker - stamp.enqueued);
                    if (classifyLate(0, 0, false, wait, mode.fps) == LateReason::queueWait) telemetry->lateQueue.fetch_add(1);
                    if (previousFrame)
                    {
                        telemetry->duration(Timing::callbackInterval, telemetry->ms(stamp.callback - previousQpc));
                        const auto timestampClass = classifyLate(previousPts, stamp.pts100ns, true, 0.0, mode.fps);
                        if (timestampClass == LateReason::timestampRegression) telemetry->loss(LossReason::timestampRegression);
                        // Do not count our own queue loss a second time as a source gap.
                        else if (stamp.frame == previousFrame + 1 && telemetry->afterWarmup(previousQpc)
                            && (timestampClass == LateReason::cadenceGap || telemetry->ms(stamp.callback - previousQpc) > mode.fps.periodMs() * 1.5))
                            telemetry->loss(LossReason::sourceCadenceGap);
                    }
                    previousFrame = stamp.frame; previousPts = stamp.pts100ns; previousQpc = stamp.callback;
                    if (wait > 2.0 * mode.fps.periodMs()) { telemetry->loss(LossReason::lateQueueDiscard); continue; }
                    const int index = pool.acquireWrite();
                    if (index == VideoSurfacePool::none) { telemetry->loss(LossReason::surfacePoolExhausted); continue; }
                    try
                    {
                        decoder.copySample(sample.Get());
                        sample.Reset(); // return scarce driver buffer BEFORE decode/colour work
                        decoder.decodeCopied(pool.surface(index), stamp);
                        colour = decoder.colourDecision();
                        if (mfDecode)
                        {
                            pool.surface(index).colourAssumed = true;
                            colour += "; MF-decoded NV12 matrix/range not verified with a physical grey chart";
                        }
                        if (pool.surface(index).colourAssumed) telemetry->colourAssumptions.fetch_add(1);
                        telemetry->recordWorker(stamp);
                        telemetry->decoded.fetch_add(1);
                        if (recordSink) recordSink(pool.surface(index)); // decode worker; one bounded copy, never the MF callback
                        if (pool.publish(index)) telemetry->loss(LossReason::previewMailboxOverwrite);
                        telemetry->latestReadyFrame.store(stamp.frame);
                    }
                    catch (...) { pool.release(index); telemetry->loss(LossReason::decoderError); throw; }
                }
            }
            catch (...)
            {
                if (!openReported) { opened.set_exception(std::current_exception()); openReported = true; }
                try { throw; } catch (const std::exception& e) { error = e.what(); }
            }
            callback->accepting.store(false);
            while (callback->active.load() != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // No active callback can rearm past this barrier. Async Flush cancels pending reads.
            if (reader)
            {
                const auto hr = reader->Flush(allSourceStreams);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                if (SUCCEEDED(hr))
                    while (!callback->flushed.load() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (FAILED(hr) || !callback->flushed.load())
                {
                    telemetry->loss(LossReason::sourceError);
                    if (error.empty()) error = "SourceReader Flush failed/timed out: " + hresultText(hr);
                }
            }
            if (source) source->Shutdown();
            reader.Reset();
            while (callback->active.load() != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            SampleEnvelope rest;
            while (callback->queue.pop(rest)) { rest.sample->Release(); telemetry->loss(LossReason::shutdownDiscard); }
            if (error.empty() && FAILED(telemetry->sourceStatus.load())) error = "SourceReader stopped: " + hresultText(telemetry->sourceStatus.load());
        }
        catch (...)
        {
            if (!openReported) opened.set_exception(std::current_exception());
            try { throw; } catch (const std::exception& e) { error = e.what(); }
        }
        done.store(true);
    }
};
MfCameraCapture::MfCameraCapture(std::shared_ptr<CaptureTelemetry> t, VideoSurfacePool& pool, std::function<void(const VideoSurface&)> sink)
    : state(std::make_unique<State>(std::move(t), pool, std::move(sink))) {}
MfCameraCapture::~MfCameraCapture() { stop(); }
CaptureOpenInfo MfCameraCapture::start(const std::string& link, CameraMode mode, bool mfDecode, int threads)
{
    if (state->worker.joinable()) throw std::logic_error("Capture instance is single-use");
    static std::atomic<std::uint64_t> generation{0};
    state->callback = std::make_shared<CallbackState>(*state->telemetry);
    state->callback->generation = ++generation;
    state->done.store(false);
    std::promise<CaptureOpenInfo> promise;
    auto future = promise.get_future();
    state->worker = std::thread(&State::run, state.get(), link, mode, mfDecode, threads, std::move(promise));
    return future.get();
}
void MfCameraCapture::stop()
{
    state->stopRequested.store(true);
    if (state->callback) SetEvent(state->callback->sampleReady);
    if (state->worker.joinable()) state->worker.join();
}
bool MfCameraCapture::finished() const noexcept { return state->done.load(); }
const std::string& MfCameraCapture::error() const noexcept { return state->error; }
const std::string& MfCameraCapture::colourDecision() const noexcept { return state->colour; }
}
