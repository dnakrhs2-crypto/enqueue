#include "VideoPlaybackEngine.h"
#include "video/PreviewPresenter.h"
#include "video/PresentPacing.h"
#include "record/Ffmpeg.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <limits>
#include <mutex>
#include <thread>

namespace gocue::recorder
{
struct PlaybackTexture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    HANDLE shared = nullptr; // legacy DXGI shared handle: NOT CloseHandle-owned
    UINT width = 0, height = 0;
    juce::String adapterName, driverVersion;
    UINT vendorId = 0, deviceId = 0;
    int decoderFramePoolSize = 0;
};
namespace
{
struct FormatInput
{
    AVFormatContext* value = nullptr;
    ~FormatInput() { avformat_close_input(&value); }
};
struct HardwareRef
{
    AVBufferRef* value = nullptr;
    ~HardwareRef() { av_buffer_unref(&value); }
};
struct ContextLock
{
    AVD3D11VADeviceContext& context;
    explicit ContextLock(AVD3D11VADeviceContext& c) : context(c) { context.lock(context.lock_ctx); }
    ~ContextLock() { context.unlock(context.lock_ctx); }
};
ComPtr<IDXGIAdapter1> nvidiaAdapter()
{
    ComPtr<IDXGIFactory1> factory; checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create playback DXGI factory");
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> gpu; const auto hr = factory->EnumAdapters1(i, &gpu);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        checkHr(hr, "Enumerate playback GPU"); DXGI_ADAPTER_DESC1 desc{}; checkHr(gpu->GetDesc1(&desc), "Read playback GPU");
        if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return gpu;
    }
    throw std::runtime_error("NVIDIA D3D11VA adapter unavailable; CPU/WARP fallback disabled");
}
class HardwareDecoder final : public IVideoFrameDecoder
{
public:
    explicit HardwareDecoder(std::shared_ptr<const VideoIndex> index) : source(std::move(index))
    {
        if (!source->current()) throw std::runtime_error("Stale video index");
        ffCheck(avformat_open_input(&input.value, source->file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Open indexed MP4 decoder");
        ffCheck(avformat_find_stream_info(input.value, nullptr), "Read H.264 decoder configuration");
        if (source->stream < 0 || static_cast<unsigned>(source->stream) >= input.value->nb_streams) throw std::runtime_error("MP4 stream changed");
        const auto* decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!decoder) throw std::runtime_error("FFmpeg H.264 decoder unavailable");
        bool supported = false;
        for (int i = 0; const auto* c = avcodec_get_hw_config(decoder, i); ++i)
            supported |= c->device_type == AV_HWDEVICE_TYPE_D3D11VA && (c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX);
        if (!supported) throw std::runtime_error("H.264 D3D11VA hardware configuration unavailable");
        hardware.value = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hardware.value) throw std::bad_alloc();
        auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(hardware.value->data);
        gpu = static_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
        const auto adapter = nvidiaAdapter(); D3D_FEATURE_LEVEL level{};
        checkHr(adapter->GetDesc1(&adapterDescription), "Read selected D3D11VA adapter");
        LARGE_INTEGER version{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) driverVersion = juce::String::toHexString(version.QuadPart);
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        checkHr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
            &gpu->device, &level, &gpu->device_context), "Create dedicated D3D11VA decoder device");
        ffCheck(av_hwdevice_ctx_init(hardware.value), "Initialize D3D11VA video device");
        codec.reset(avcodec_alloc_context3(decoder)); if (!codec) throw std::bad_alloc();
        ffCheck(avcodec_parameters_to_context(codec.get(), input.value->streams[source->stream]->codecpar), "Copy H.264 parameters");
        codec->hw_device_ctx = av_buffer_ref(hardware.value); if (!codec->hw_device_ctx) throw std::bad_alloc();
        codec->get_format = [](AVCodecContext*, const AVPixelFormat* formats)
        { for (; *formats != AV_PIX_FMT_NONE; ++formats) if (*formats == AV_PIX_FMT_D3D11) return *formats; return AV_PIX_FMT_NONE; };
        codec->thread_count = 1; // one camera worker; DPB belongs to this decoder
        ffCheck(avcodec_open2(codec.get(), decoder, nullptr), "Open accelerated H.264 decoder (no software fallback)");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = desc.OutputWidth = static_cast<UINT>(source->width);
        desc.InputHeight = desc.OutputHeight = static_cast<UINT>(source->height);
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        checkHr(gpu->video_device->CreateVideoProcessorEnumerator(&desc, &enumerator), "Create NV12 playback processor enumerator");
        checkHr(gpu->video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor), "Create GPU NV12 to BGRA processor");
    }
    void resetForSeek() override { lastFrame = noFrame; }
    PlaybackDecodeTiming decodeTiming() const override { return timing; }
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t target, const std::function<bool()>& cancelled) override
    {
        timing = {}; timing.targetPacket = target;
        const auto& wanted = source->packets.at(target);
        const auto idr = source->previousIdr(wanted.sample);
        if (lastFrame == noFrame || target <= lastFrame || (target - lastFrame > 8 && idr > lastFrame))
        {
            // A forward cursor jump within this GOP is cheaper to continue than
            // to flush and decode the same IDR prefix a second time.
            timing.fromIdr = true; timing.idrPacket = idr;
            const auto seekBegin = qpcNow();
            ffCheck(av_seek_frame(input.value, source->stream, source->packets[idr].pts, AVSEEK_FLAG_BACKWARD), "Seek preceding IDR");
            const auto flushBegin = qpcNow(); timing.idrSeekTicks = flushBegin - seekBegin;
            avcodec_flush_buffers(codec.get()); draining = false;
            timing.flushTicks = qpcNow() - flushBegin;
        }
        const auto decodeBegin = qpcNow();
        auto frame = ffFrame(); auto packet = ffPacket();
        for (;;)
        {
            if (cancelled() || !source->current()) return {};
            const auto received = avcodec_receive_frame(codec.get(), frame.get());
            if (received == 0)
            {
                ++timing.receivedFrames;
                if (frame->format != AV_PIX_FMT_D3D11) throw std::runtime_error("Decoder returned a non-D3D11 frame; fallback rejected");
                const auto pts = frame->best_effort_timestamp;
                if (pts > wanted.pts) throw std::runtime_error("Decoder skipped requested indexed frame");
                if (pts == wanted.pts)
                {
                    lastFrame = target; const auto convertBegin = qpcNow();
                    timing.decodeTicks = convertBegin - decodeBegin;
                    auto texture = convert(*frame, cancelled);
                    timing.convertTicks = qpcNow() - convertBegin;
                    return texture;
                }
                av_frame_unref(frame.get()); continue;
            }
            if (received != AVERROR(EAGAIN)) { ffCheck(received, "Receive requested H.264 frame"); throw std::runtime_error("H.264 ended before requested frame"); }
            if (draining) throw std::runtime_error("H.264 drain produced no requested frame");
            for (;;)
            {
                const auto read = av_read_frame(input.value, packet.get());
                if (read == AVERROR_EOF) { ffCheck(avcodec_send_packet(codec.get(), nullptr), "Drain H.264"); draining = true; break; }
                ffCheck(read, "Read indexed H.264 packet");
                if (packet->stream_index == source->stream)
                { ffCheck(avcodec_send_packet(codec.get(), packet.get()), "Decode H.264 packet"); av_packet_unref(packet.get()); break; }
                av_packet_unref(packet.get()); // MP4 reference AAC deliberately ignored
                if (cancelled()) return {};
            }
        }
    }
private:
    std::shared_ptr<const PlaybackTexture> convert(const AVFrame& frame, const std::function<bool()>& cancelled)
    {
        if (frame.width != source->width || frame.height != source->height || frame.colorspace != AVCOL_SPC_BT709
            || frame.color_range != AVCOL_RANGE_MPEG) throw std::runtime_error("Expected Recorder BT.709 limited H.264 surface");
        auto output = std::make_shared<PlaybackTexture>(); output->width = frame.width; output->height = frame.height;
        output->adapterName = juce::String(adapterDescription.Description); output->vendorId = adapterDescription.VendorId;
        output->deviceId = adapterDescription.DeviceId; output->driverVersion = driverVersion;
        if (frame.hw_frames_ctx) output->decoderFramePoolSize = reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data)->initial_pool_size;
        D3D11_TEXTURE2D_DESC texture{}; texture.Width = output->width; texture.Height = output->height;
        texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1; texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.Usage = D3D11_USAGE_DEFAULT; texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        checkHr(gpu->device->CreateTexture2D(&texture, nullptr, &output->texture), "Create shared display texture");
        checkHr(output->texture.As(&output->mutex), "Get shared texture keyed mutex");
        ComPtr<IDXGIResource> resource; checkHr(output->texture.As(&resource), "Get playback shared resource");
        checkHr(resource->GetSharedHandle(&output->shared), "Share display texture");
        // New texture is owned by key zero. WAIT_TIMEOUT/WAIT_ABANDONED are not
        // FAILED HRESULTs, so require S_OK rather than only checking FAILED().
        if (output->mutex->AcquireSync(0, 0) != S_OK) throw std::runtime_error("Cannot acquire new playback texture");
        struct Unlock { IDXGIKeyedMutex* value; ~Unlock() { value->ReleaseSync(1); } } unlock{output->mutex.Get()};
        ComPtr<ID3D11Query> done; const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        checkHr(gpu->device->CreateQuery(&query, &done), "Create playback conversion fence");
        {
            ContextLock lock(*gpu);
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in{}; in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            in.Texture2D.ArraySlice = static_cast<UINT>(reinterpret_cast<std::intptr_t>(frame.data[1]));
            ComPtr<ID3D11VideoProcessorInputView> inputView;
            checkHr(gpu->video_device->CreateVideoProcessorInputView(reinterpret_cast<ID3D11Texture2D*>(frame.data[0]), enumerator.Get(), &in, &inputView), "View decoded NV12 array slice");
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{}; out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ComPtr<ID3D11VideoProcessorOutputView> outputView;
            checkHr(gpu->video_device->CreateVideoProcessorOutputView(output->texture.Get(), enumerator.Get(), &out, &outputView), "View shared BGRA output");
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColour{}; inputColour.YCbCr_Matrix = 1;
            inputColour.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColour{}; outputColour.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            gpu->video_context->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &inputColour);
            gpu->video_context->VideoProcessorSetOutputColorSpace(processor.Get(), &outputColour);
            gpu->video_context->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            gpu->video_context->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
            const RECT rect{0, 0, frame.width, frame.height};
            gpu->video_context->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &rect);
            gpu->video_context->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &rect);
            D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = inputView.Get();
            checkHr(gpu->video_context->VideoProcessorBlt(processor.Get(), outputView.Get(), 0, 1, &stream), "Convert decoded frame on GPU");
            gpu->device_context->End(done.Get()); gpu->device_context->Flush();
        }
        // Keep AVFrame/DPB slice alive until conversion completes, even if seek was
        // superseded. Never hold the FFmpeg context lock while waiting for GPU.
        const auto deadline = qpcNow() + qpcFrequency() * 2;
        for (;;)
        {
            BOOL complete = FALSE; HRESULT hr;
            { ContextLock lock(*gpu); hr = gpu->device_context->GetData(done.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH); }
            checkHr(hr, "Poll playback GPU completion");
            if (hr == S_OK && complete) break;
            checkHr(gpu->device->GetDeviceRemovedReason(), "Playback decode device removed");
            if (qpcNow() >= deadline) throw std::runtime_error("Playback GPU completion timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return cancelled() ? nullptr : output;
    }
    static constexpr std::size_t noFrame = static_cast<std::size_t>(-1);
    std::shared_ptr<const VideoIndex> source;
    FormatInput input;
    HardwareRef hardware;
    CodecPtr codec;
    AVD3D11VADeviceContext* gpu = nullptr;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    std::size_t lastFrame = noFrame;
    bool draining = false;
    PlaybackDecodeTiming timing;
    DXGI_ADAPTER_DESC1 adapterDescription{};
    juce::String driverVersion;
};
constexpr auto absent = static_cast<std::size_t>(-1);
std::size_t activeClip(const std::vector<PlaybackVideoClip>& clips, Sample sample)
{
    const auto it = std::upper_bound(clips.begin(), clips.end(), sample,
        [](Sample s, const auto& c) { return s < c.mapping.timelineStartSample; });
    if (it == clips.begin()) return absent;
    const auto& c = std::prev(it)->mapping;
    return sample < c.timelineStartSample + c.lengthSamples ? static_cast<std::size_t>(std::distance(clips.begin(), it) - 1) : absent;
}
std::pair<std::size_t, std::size_t> frameKey(const std::vector<PlaybackVideoClip>& clips, Sample sample)
{
    const auto clip = activeClip(clips, sample);
    if (clip == absent) return {absent, absent};
    const auto& c = clips[clip];
    return {clip, c.source->frameAt(c.mapping.sourceIn + sample - c.mapping.timelineStartSample)};
}
}

// The playback view belongs to PreviewPresenter but lives in the round-11 module.
// Its immediate context is exclusive to its present thread, on the decoder's
// adapter. Shared BGRA/keyed-mutex interop never maps pixels to CPU memory.
class PreviewPresenter::PlaybackView
{
public:
    PlaybackView(HWND window, VideoPlaybackEngine& source, unsigned camera) : hwnd(window), engine(source), camera(camera)
    { thread = std::thread([this] { run(); }); }
    ~PlaybackView() { stopping.store(true); if (thread.joinable()) thread.join(); }
    juce::Result status() const
    { std::lock_guard<std::mutex> lock(mutex); return error.isEmpty() ? juce::Result::ok() : juce::Result::fail(error); }
private:
    void run();
    HWND hwnd;
    VideoPlaybackEngine& engine;
    unsigned camera;
    std::thread thread;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    juce::String error;
};
struct VideoPlaybackEngine::Impl
{
    struct Lane
    {
        std::vector<PlaybackVideoClip> clips;
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::thread worker;
        Sample target = 0;
        std::uint64_t request = 0, stale = 0, decoded = 0, prefetched = 0, late = 0;
        std::uint64_t generation = 0, lateGeneration = 0, decoderOpens = 0, cacheHits = 0, cacheMisses = 0;
        std::pair<std::size_t, std::size_t> lateKey{absent, absent};
        bool advancing = false;
        std::int64_t decoderCreateTicks = 0, presenterBeginQpc = 0, presenterEndQpc = 0;
        std::vector<std::shared_ptr<const PlaybackVideoFrame>> ready;
        PlaybackPresentation presented;
        PlaybackSeekTiming timing;
        juce::String error;
    };
    std::array<Lane, 2> lanes;
    std::array<std::unique_ptr<PreviewPresenter::PlaybackView>, 2> views;
    DecoderFactory factory;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> stopping{false};
    explicit Impl(DecoderFactory f) : factory(std::move(f))
    { if (!factory) factory = [](auto source) { return std::make_unique<HardwareDecoder>(std::move(source)); }; }
    void run(unsigned camera)
    {
        auto& lane = lanes[camera];
        std::uint64_t processed = 0, decoderGeneration = 0;
        std::map<std::size_t, std::unique_ptr<IVideoFrameDecoder>> decoders;
        try
        {
            ComApartment apartment;
            while (!stopping.load())
            {
                Sample target; std::uint64_t request, gen;
                std::vector<std::shared_ptr<const PlaybackVideoFrame>> cache;
                {
                    std::unique_lock<std::mutex> lock(lane.mutex);
                    lane.wake.wait(lock, [&] { return stopping.load() || lane.request != processed; });
                    if (stopping.load()) break;
                    target = lane.target; request = lane.request; gen = lane.generation; cache = lane.ready;
                    if (lane.timing.generation == gen && !lane.timing.workerQpc) lane.timing.workerQpc = qpcNow();
                }
                processed = request;
                const auto cancelled = [&] { return stopping.load() || generation.load() != gen; };
                if (decoderGeneration != gen)
                {
                    for (auto& entry : decoders) entry.second->resetForSeek();
                    decoderGeneration = gen;
                }
                const auto clip = activeClip(lane.clips, target);
                if (clip == absent)
                { std::lock_guard<std::mutex> lock(lane.mutex); if (!cancelled()) lane.ready.clear(); continue; }
                const auto next = clip + 1 < lane.clips.size() ? clip + 1 : absent;
                for (auto it = decoders.begin(); it != decoders.end();)
                    if (it->first != clip && it->first != next) it = decoders.erase(it); else ++it;
                const auto& current = lane.clips[clip]; const auto& mapping = current.mapping;
                const auto frame = current.source->frameAt(mapping.sourceIn + target - mapping.timelineStartSample);
                std::vector<std::pair<std::size_t, std::size_t>> wanted{{clip, frame}};
                if (next != absent) wanted.push_back({next, lane.clips[next].source->frameAt(lane.clips[next].mapping.sourceIn)});
                if (frame + 1 < current.source->packets.size()
                    && current.source->packets[frame + 1].sample < mapping.sourceIn + mapping.lengthSamples) wanted.push_back({clip, frame + 1});
                std::vector<std::shared_ptr<const PlaybackVideoFrame>> ready;
                for (const auto& [which, packet] : wanted)
                {
                    if (cancelled()) break;
                    // Exact target is published first. Superseded cursor work
                    // must not wait for optional next-clip/next-frame prefetch.
                    const bool exactTarget = ready.empty();
                    if (!exactTarget)
                    { std::lock_guard<std::mutex> lock(lane.mutex); if (lane.request != request) break; }
                    const auto& c = lane.clips[which]; const auto& p = c.source->packets[packet];
                    if (!c.source->current()) throw std::runtime_error("Video media generation replaced; prepare a new clip set");
                    const auto hit = std::find_if(cache.begin(), cache.end(), [&](const auto& f)
                        { return f->source == c.source && f->source->current() && f->clipId == c.mapping.clipId && f->pts == p.pts; });
                    bool measureSeek = false;
                    {
                        std::lock_guard<std::mutex> lock(lane.mutex);
                        measureSeek = exactTarget && lane.timing.generation == gen && !lane.timing.readyQpc;
                        if (measureSeek)
                        {
                            lane.timing.cacheKnown = true; lane.timing.cacheHit = hit != cache.end();
                            if (lane.timing.cacheHit) ++lane.cacheHits; else ++lane.cacheMisses;
                        }
                    }
                    std::shared_ptr<const PlaybackTexture> texture;
                    if (hit != cache.end()) texture = (*hit)->texture;
                    else
                    {
                        auto& decoder = decoders[which];
                        if (!decoder)
                        {
                            const auto begin = qpcNow();
                            { std::lock_guard<std::mutex> lock(lane.mutex); if (measureSeek && lane.timing.generation == gen) lane.timing.decoderBeginQpc = begin; }
                            decoder = factory(c.source);
                            if (!decoder) throw std::runtime_error("Playback decoder factory returned null");
                            const auto finish = qpcNow();
                            std::lock_guard<std::mutex> lock(lane.mutex);
                            ++lane.decoderOpens; lane.decoderCreateTicks += finish - begin;
                            if (measureSeek && lane.timing.generation == gen) lane.timing.decoderEndQpc = finish;
                        }
                        if (cancelled()) break;
                        { std::lock_guard<std::mutex> lock(lane.mutex); if (measureSeek && lane.timing.generation == gen) lane.timing.decodeBeginQpc = qpcNow(); }
                        texture = decoder->decodeFrame(packet, cancelled);
                        const auto finish = qpcNow();
                        std::lock_guard<std::mutex> lock(lane.mutex);
                        if (measureSeek && lane.timing.generation == gen)
                        { lane.timing.decodeEndQpc = finish; lane.timing.decode = decoder->decodeTiming(); }
                        if (!cancelled()) { ++lane.decoded; if (which == next) ++lane.prefetched; }
                    }
                    if (cancelled() || !c.source->current()) break;
                    auto f = std::make_shared<PlaybackVideoFrame>(); f->clipId = c.mapping.clipId; f->pts = p.pts;
                    f->begin = c.mapping.timelineStartSample + (std::max)(Sample{0}, p.sample - c.mapping.sourceIn);
                    f->end = (std::min)(c.mapping.timelineStartSample + c.mapping.lengthSamples,
                        c.mapping.timelineStartSample + p.endSample - c.mapping.sourceIn);
                    f->generation = gen; f->source = c.source; f->texture = std::move(texture); ready.push_back(f);
                    std::lock_guard<std::mutex> lock(lane.mutex);
                    if (cancelled()) break;
                    for (const auto& item : ready) if (!item->current(gen)) throw std::runtime_error("Media replaced before display-ready publish");
                    lane.ready = ready; // publish exact now, then <=3 total including next clip
                    if (measureSeek) lane.timing.readyQpc = qpcNow();
                }
                if (cancelled()) { std::lock_guard<std::mutex> lock(lane.mutex); ++lane.stale; }
            }
        }
        catch (const std::exception& e) { std::lock_guard<std::mutex> lock(lane.mutex); lane.error = juce::String::fromUTF8(e.what()); }
        decoders.clear(); // FFmpeg/D3D resources released on camera worker
    }
};
VideoPlaybackEngine::VideoPlaybackEngine(DecoderFactory f) : impl(std::make_unique<Impl>(std::move(f))) {}
VideoPlaybackEngine::~VideoPlaybackEngine() { stop(); }
void VideoPlaybackEngine::stop()
{
    for (auto& view : impl->views) view.reset();
    for (auto& lane : impl->lanes)
    {
        { std::lock_guard<std::mutex> lock(lane.mutex); impl->stopping.store(true); }
        lane.wake.notify_all();
    }
    for (auto& lane : impl->lanes) if (lane.worker.joinable()) lane.worker.join();
    for (auto& lane : impl->lanes) { std::lock_guard<std::mutex> lock(lane.mutex); lane.ready.clear(); }
}
void VideoPlaybackEngine::prepare(std::vector<PlaybackVideoClip> clips)
{
    stop();
    for (auto& lane : impl->lanes)
    {
        lane.clips.clear(); lane.error.clear(); lane.request = lane.generation = 0; lane.target = 0;
        lane.presented = {}; lane.timing = {}; lane.advancing = false;
        lane.stale = lane.decoded = lane.prefetched = lane.late = lane.lateGeneration = 0;
        lane.decoderOpens = lane.cacheHits = lane.cacheMisses = 0; lane.lateKey = {absent, absent};
        lane.decoderCreateTicks = lane.presenterBeginQpc = lane.presenterEndQpc = 0;
    }
    for (auto& clip : clips)
    {
        const auto& c = clip.mapping;
        if (clip.camera > 1 || !clip.source || !clip.source->current() || c.clipId.isEmpty() || c.timelineStartSample < 0
            || c.sourceIn < 0 || c.lengthSamples <= 0 || c.sourceIn > clip.source->length || c.lengthSamples > clip.source->length - c.sourceIn
            || c.mediaGeneration != static_cast<Sample>(clip.source->generation)
            || c.timelineStartSample > (std::numeric_limits<Sample>::max)() - c.lengthSamples)
            throw std::invalid_argument("Invalid video clip/source generation");
        impl->lanes[clip.camera].clips.push_back(std::move(clip));
    }
    for (auto& lane : impl->lanes)
    {
        std::sort(lane.clips.begin(), lane.clips.end(), [](const auto& a, const auto& b) { return a.mapping.timelineStartSample < b.mapping.timelineStartSample; });
        Sample end = 0;
        for (const auto& c : lane.clips)
        { if (c.mapping.timelineStartSample < end) throw std::invalid_argument("Video clips overlap"); end = c.mapping.timelineStartSample + c.mapping.lengthSamples; }
    }
    impl->generation.store(0); impl->stopping.store(false);
    for (unsigned camera = 0; camera < 2; ++camera) impl->lanes[camera].worker = std::thread([this, camera] { impl->run(camera); });
}
std::uint64_t VideoPlaybackEngine::seek(Sample sample) { const auto gen = impl->generation.load() + 1; seek(sample, gen); return gen; }
void VideoPlaybackEngine::seek(Sample sample, std::uint64_t gen)
{
    if (sample < 0 || !gen || gen <= impl->generation.load()) throw std::invalid_argument("Seek generation must increase");
    {
        // Pair target and generation under the same locks. A worker must never
        // adopt the new generation with the previous lane's target.
        std::scoped_lock lock(impl->lanes[0].mutex, impl->lanes[1].mutex);
        impl->generation.store(gen);
        for (auto& lane : impl->lanes)
        {
            lane.target = sample; lane.generation = gen; lane.advancing = false; ++lane.request;
            lane.timing = {}; lane.timing.generation = gen; lane.timing.target = sample; lane.timing.requestedQpc = qpcNow();
            // Keep at most three immutable cache entries. Only the camera worker
            // may rebind a validated file/clip/PTS match to this seek generation.
        }
    }
    for (auto& lane : impl->lanes) lane.wake.notify_one();
}
bool VideoPlaybackEngine::requestFrame(unsigned camera, Sample sample, std::uint64_t gen, bool advancing)
{
    if (camera > 1 || sample < 0) throw std::invalid_argument("Invalid camera/frame request");
    auto& lane = impl->lanes[camera];
    { std::lock_guard<std::mutex> lock(lane.mutex);
      if (gen != impl->generation.load()) { ++lane.stale; return false; }
      lane.advancing = advancing;
      const auto sameFrame = frameKey(lane.clips, lane.target) == frameKey(lane.clips, sample);
      lane.target = sample;
      if (sameFrame) return true; // audio samples within one frame need no new decode work
      ++lane.request; }
    lane.wake.notify_one(); return true;
}
bool VideoPlaybackEngine::ready(Sample sample, std::uint64_t gen) const
{
    if (gen != impl->generation.load()) return false;
    for (const auto& lane : impl->lanes)
    {
        std::lock_guard<std::mutex> lock(lane.mutex);
        if (lane.generation != gen) return false;
        if (activeClip(lane.clips, sample) == absent) continue;
        if (std::none_of(lane.ready.begin(), lane.ready.end(), [&](const auto& f) { return f->current(gen) && f->begin <= sample && sample < f->end; })) return false;
    }
    return gen == impl->generation.load();
}
PlaybackDisplaySelection VideoPlaybackEngine::displaySelection(unsigned camera, bool presentationTick) const
{
    auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex);
    PlaybackDisplaySelection selected; selected.generation = impl->generation.load();
    selected.gap = activeClip(lane.clips, lane.target) == absent;
    if (!selected.gap)
    {
        for (const auto& f : lane.ready) if (f->current(selected.generation) && f->begin <= lane.target && lane.target < f->end) { selected.frame = f; break; }
        if (!selected.frame && presentationTick && lane.advancing && lane.generation == selected.generation)
        {
            const auto key = frameKey(lane.clips, lane.target);
            if (lane.lateGeneration != selected.generation || lane.lateKey != key)
            { ++lane.late; lane.lateGeneration = selected.generation; lane.lateKey = key; }
        }
    }
    return selected;
}
void VideoPlaybackEngine::presented(unsigned camera, const PlaybackVideoFrame& frame, std::int64_t qpc)
{
    auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex);
    if (frame.current(impl->generation.load()))
    {
        // Preserve the FIRST successful receipt for a frame, even when repeated
        // present ticks occur before the control owner next polls it.
        if (lane.presented.generation != frame.generation || lane.presented.begin != frame.begin || lane.presented.pts != frame.pts)
            lane.presented = {frame.generation, frame.begin, frame.end, frame.pts, qpc};
        if (lane.timing.generation == frame.generation && !lane.timing.presentQpc
            && frame.begin <= lane.timing.target && lane.timing.target < frame.end) lane.timing.presentQpc = qpc;
    }
    else ++lane.stale;
}
PlaybackPresentation VideoPlaybackEngine::lastPresentation(unsigned camera) const
{ const auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex); return lane.presented; }
PlaybackSeekTiming VideoPlaybackEngine::seekTiming(unsigned camera) const
{ const auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex); return lane.timing; }
void VideoPlaybackEngine::presenterInitialised(unsigned camera, std::int64_t begin, std::int64_t end)
{
    auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex);
    lane.presenterBeginQpc = begin; lane.presenterEndQpc = end;
}
void VideoPlaybackEngine::attachPlaybackView(unsigned camera, void* hwnd)
{
    if (camera > 1 || !IsWindow(static_cast<HWND>(hwnd))) throw std::invalid_argument("Playback needs an existing left/right HWND host");
    impl->views[camera].reset(); impl->views[camera] = std::make_unique<PreviewPresenter::PlaybackView>(static_cast<HWND>(hwnd), *this, camera);
}
juce::Result VideoPlaybackEngine::status() const
{
    for (const auto& view : impl->views) if (view) { const auto result = view->status(); if (result.failed()) return result; }
    for (const auto& lane : impl->lanes) { std::lock_guard<std::mutex> lock(lane.mutex); if (lane.error.isNotEmpty()) return juce::Result::fail(lane.error); }
    return juce::Result::ok();
}
juce::var VideoPlaybackEngine::telemetry() const
{
    auto result = jsonObject(); juce::Array<juce::var> cameras;
    jsonSet(result, "decoder", "FFmpeg H.264 + AV_HWDEVICE_TYPE_D3D11VA; no CPU fallback");
    jsonSet(result, "interop", "Dedicated decoder/presenter devices; GPU NV12 to shared BGRA; keyed mutex + completion query; no CPU pixel readback");
    jsonSet(result, "displayReadyLimitPerCamera", 3); jsonSet(result, "decoderDpb", "FFmpeg-owned per decoder; current plus next clip decoder");
    jsonSet(result, "lateDefinition", "Unique missing containing frames at advancing presentation ticks; excludes seek preparation, gap/startup/UI queries and repeated ticks for the same frame");
    for (const auto& lane : impl->lanes)
    {
        std::lock_guard<std::mutex> lock(lane.mutex); auto c = jsonObject();
        jsonSet(c, "readyFrames", lane.ready.size()); jsonSet(c, "decoded", lane.decoded); jsonSet(c, "nextClipPrefetched", lane.prefetched);
        jsonSet(c, "staleDiscarded", lane.stale); jsonSet(c, "lateDisplaySelections", lane.late); jsonSet(c, "error", lane.error); cameras.add(c);
        jsonSet(c, "generation", lane.generation); jsonSet(c, "targetSample", lane.target);
        jsonSet(c, "decoderOpens", lane.decoderOpens); jsonSet(c, "decoderCreateMs", 1000.0 * lane.decoderCreateTicks / qpcFrequency());
        jsonSet(c, "seekCacheHits", lane.cacheHits); jsonSet(c, "seekCacheMisses", lane.cacheMisses);
        jsonSet(c, "presenterInitBeginQpc", lane.presenterBeginQpc); jsonSet(c, "presenterInitEndQpc", lane.presenterEndQpc);
        const auto& t = lane.timing; auto seek = jsonObject();
        jsonSet(seek, "generation", t.generation); jsonSet(seek, "sample", t.target);
        jsonSet(seek, "requestedQpc", t.requestedQpc); jsonSet(seek, "workerQpc", t.workerQpc);
        jsonSet(seek, "decoderBeginQpc", t.decoderBeginQpc); jsonSet(seek, "decoderEndQpc", t.decoderEndQpc);
        jsonSet(seek, "decodeBeginQpc", t.decodeBeginQpc); jsonSet(seek, "decodeEndQpc", t.decodeEndQpc);
        jsonSet(seek, "readyQpc", t.readyQpc); jsonSet(seek, "firstExactPresentQpc", t.presentQpc);
        jsonSet(seek, "frameCache", !t.cacheKnown ? "pending" : t.cacheHit ? "hit" : "miss");
        jsonSet(seek, "phase", t.presentQpc ? "presented" : t.readyQpc ? "display-ready" : t.decodeBeginQpc ? "target-decode" : t.decoderBeginQpc ? "decoder-create" : "queued");
        const auto ms = [](std::int64_t ticks) { return 1000.0 * ticks / qpcFrequency(); };
        jsonSet(seek, "idrSeekMs", ms(t.decode.idrSeekTicks)); jsonSet(seek, "decoderFlushMs", ms(t.decode.flushTicks));
        jsonSet(seek, "compressedDecodeMs", ms(t.decode.decodeTicks)); jsonSet(seek, "gpuConvertMs", ms(t.decode.convertTicks));
        jsonSet(seek, "fromIdr", t.decode.fromIdr); jsonSet(seek, "idrPacket", t.decode.idrPacket);
        jsonSet(seek, "targetPacket", t.decode.targetPacket); jsonSet(seek, "receivedFrames", t.decode.receivedFrames);
        jsonSet(c, "seek", seek);
        jsonSet(c, "lastPresentedGeneration", lane.presented.generation); jsonSet(c, "lastPresentedBegin", lane.presented.begin);
        jsonSet(c, "lastPresentedEnd", lane.presented.end); jsonSet(c, "lastPresentedQpc", lane.presented.qpc);
        for (const auto& frame : lane.ready) if (frame->texture)
        {
            const auto& texture = *frame->texture;
            jsonSet(c, "adapterDescription", texture.adapterName); jsonSet(c, "vendorId", texture.vendorId); jsonSet(c, "deviceId", texture.deviceId);
            jsonSet(c, "driverVersion", texture.driverVersion.isEmpty() ? juce::var() : juce::var(texture.driverVersion));
            jsonSet(c, "decoderFramePoolSize", texture.decoderFramePoolSize); break;
        }
    }
    jsonSet(result, "cameras", cameras); return result;
}

void PreviewPresenter::PlaybackView::run()
{
    try
    {
        ComApartment apartment;
        PlaybackDisplaySelection first;
        while (!stopping.load())
        {
            first = engine.displaySelection(camera);
            if (first.frame && first.frame->texture) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (stopping.load()) return;
        const auto initBegin = qpcNow();
        ComPtr<ID3D11Device> decoderDevice; first.frame->texture->texture->GetDevice(&decoderDevice);
        ComPtr<IDXGIDevice> dxgi; checkHr(decoderDevice.As(&dxgi), "Get playback adapter device");
        ComPtr<IDXGIAdapter> adapter; checkHr(dxgi->GetAdapter(&adapter), "Get playback adapter");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL level{};
        checkHr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context), "Create dedicated playback present device");
        ComPtr<IDXGIFactory2> factory; checkHr(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get playback swapchain factory");
        RECT client{}; GetClientRect(hwnd, &client);
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = (std::max)(1L, client.right); desc.Height = (std::max)(1L, client.bottom);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BufferCount = 2;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> swap; checkHr(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, &swap), "Create playback swapchain");
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGISwapChain2> swap2; checkHr(swap.As(&swap2), "Get playback latency object");
        checkHr(swap2->SetMaximumFrameLatency(1), "Set playback frame latency");
        struct Handle { HANDLE value; ~Handle() { if (value) CloseHandle(value); } } latency{swap2->GetFrameLatencyWaitableObject()};
        if (!latency.value) throw std::runtime_error("Playback latency handle unavailable");
        constexpr char shader[] = R"(
Texture2D<float4> picture : register(t0); SamplerState linearSampler : register(s0);
struct Vertex { float4 p : SV_Position; float2 uv : TEXCOORD0; };
Vertex vsMain(uint id : SV_VertexID) { Vertex v; v.uv = float2((id << 1) & 2, id & 2); v.p = float4(v.uv.x*2-1, 1-v.uv.y*2, 0, 1); return v; }
float4 psMain(Vertex v) : SV_Target { return picture.Sample(linearSampler, v.uv); }
)";
        auto compile = [&](const char* entry, const char* profile)
        {
            ComPtr<ID3DBlob> code, errors;
            checkHr(D3DCompile(shader, sizeof(shader) - 1, "RecorderPlayback", nullptr, nullptr, entry, profile,
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "Compile playback display shader"); return code;
        };
        const auto vsCode = compile("vsMain", "vs_5_0"), psCode = compile("psMain", "ps_5_0");
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
        checkHr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "Create playback VS");
        checkHr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "Create playback PS");
        D3D11_SAMPLER_DESC sampling{}; sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampling.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler; checkHr(device->CreateSamplerState(&sampling, &sampler), "Create playback sampler");
        D3D11_RASTERIZER_DESC rasterDesc{}; rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster; checkHr(device->CreateRasterizerState(&rasterDesc, &raster), "Create playback rasterizer");
        ComPtr<ID3D11RenderTargetView> target;
        auto makeTarget = [&]
        {
            ComPtr<ID3D11Texture2D> back; checkHr(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "Get playback backbuffer");
            checkHr(device->CreateRenderTargetView(back.Get(), nullptr, &target), "Create playback target");
        };
        makeTarget(); PresentPacing pacing; PlaybackPresentOpportunity opportunity;
        engine.presenterInitialised(camera, initBegin, qpcNow());
        std::shared_ptr<const PlaybackVideoFrame> displayed;
        first = {}; // do not retain the startup frame/decoder device forever
        decoderDevice.Reset(); dxgi.Reset();
        auto nextTick = qpcNow(); const auto period = qpcFrequency() / 60;
        while (!stopping.load())
        {
            if (qpcNow() < nextTick) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            nextTick = qpcNow() + period;
            if (pacing.needsVisibilityTest())
            { const auto hr = swap->Present(0, DXGI_PRESENT_TEST); checkHr(hr, "Test playback visibility"); pacing.visibilityTest(hr); if (pacing.needsVisibilityTest()) continue; }
            if (opportunity.needsWait())
            {
                const auto wait = WaitForSingleObject(latency.value, 0);
                if (wait == WAIT_TIMEOUT) continue;
                if (wait != WAIT_OBJECT_0) throw std::runtime_error("Playback latency wait failed");
                opportunity.acquired();
            }
            GetClientRect(hwnd, &client);
            if (client.right <= 0 || client.bottom <= 0) continue;
            if (desc.Width != static_cast<UINT>(client.right) || desc.Height != static_cast<UINT>(client.bottom))
            {
                context->OMSetRenderTargets(0, nullptr, nullptr); target.Reset();
                desc.Width = client.right; desc.Height = client.bottom;
                checkHr(swap->ResizeBuffers(0, desc.Width, desc.Height, DXGI_FORMAT_UNKNOWN, desc.Flags), "Resize playback host"); makeTarget();
            }
            const auto selection = engine.displaySelection(camera, true);
            if (selection.gap || (displayed && !displayed->current(selection.generation))) displayed.reset();
            if (selection.frame) displayed = selection.frame;
            const float black[]{0, 0, 0, 1}; context->ClearRenderTargetView(target.Get(), black);
            if (displayed && displayed->texture)
            {
                const auto& surface = *displayed->texture;
                ComPtr<ID3D11Texture2D> shared; checkHr(device->OpenSharedResource(surface.shared, IID_PPV_ARGS(&shared)), "Open playback GPU texture");
                ComPtr<IDXGIKeyedMutex> keyed; checkHr(shared.As(&keyed), "Get present keyed mutex");
                const auto acquire = keyed->AcquireSync(1, 0);
                if (acquire == WAIT_TIMEOUT) continue;
                if (acquire != S_OK) throw std::runtime_error("Playback texture synchronization failed/abandoned");
                struct Release { IDXGIKeyedMutex* p; ~Release() { p->ReleaseSync(1); } } release{keyed.Get()};
                ComPtr<ID3D11ShaderResourceView> view; checkHr(device->CreateShaderResourceView(shared.Get(), nullptr, &view), "View playback BGRA texture");
                const auto scale = (std::min)(static_cast<float>(desc.Width) / surface.width, static_cast<float>(desc.Height) / surface.height);
                const D3D11_VIEWPORT viewport{(desc.Width - surface.width * scale) / 2, (desc.Height - surface.height * scale) / 2,
                    surface.width * scale, surface.height * scale, 0, 1};
                context->RSSetViewports(1, &viewport); context->RSSetState(raster.Get());
                auto* renderTarget = target.Get(); context->OMSetRenderTargets(1, &renderTarget, nullptr);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
                auto* linear = sampler.Get(); context->PSSetSamplers(0, 1, &linear);
                auto* srv = view.Get(); context->PSSetShaderResources(0, 1, &srv); context->Draw(3, 0);
                srv = nullptr; context->PSSetShaderResources(0, 1, &srv); context->Flush();
            }
            // A generation changed during GPU work must not become a successful
            // exact-seek result. Recheck immediately before submission and receipt.
            const auto current = engine.displaySelection(camera);
            if (displayed && !displayed->current(current.generation)) continue;
            const auto submitted = qpcNow(); const auto hr = swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT); pacing.presented(hr);
            opportunity.submitted(hr == S_OK);
            if (hr == DXGI_STATUS_OCCLUDED || hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            checkHr(hr, "Playback Present");
            if (displayed && displayed->texture) engine.presented(camera, *displayed, submitted);
        }
        context->ClearState(); context->Flush();
    }
    catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mutex); error = juce::String::fromUTF8(e.what()); }
}
}
