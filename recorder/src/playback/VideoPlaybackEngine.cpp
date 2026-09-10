#include "VideoPlaybackEngine.h"
#include "video/PreviewPresenter.h"
#include "video/PresentPacing.h"
#include "record/Ffmpeg.h"
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}
#include <algorithm>
#include <condition_variable>
#include <map>
#include <limits>
#include <mutex>
#include <thread>
#include <set>
#include <future>

namespace gocue::recorder
{
PlaybackWakeEvent::PlaybackWakeEvent() : handle(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{ if (!handle) throw std::runtime_error("Create playback notification event failed"); }
PlaybackWakeEvent::~PlaybackWakeEvent() { CloseHandle(handle); }
void PlaybackWakeEvent::signal() const noexcept { SetEvent(handle); }
PlaybackDecodePlan playbackDecodePlan(const VideoIndex& source, std::size_t target,
                                      std::optional<std::size_t> lastDecoded)
{
    const auto idr = source.previousIdr(source.packets.at(target).sample);
    const bool reuse = lastDecoded && *lastDecoded < target && *lastDecoded + 1 >= idr;
    const auto first = reuse ? *lastDecoded + 1 : idr;
    return {first, target, target - first, !reuse};
}
PlaybackFrameCache::PlaybackFrameCache() : PlaybackFrameCache(Limits{}) {}
PlaybackFrameCache::PlaybackFrameCache(Limits l) : limits(l)
{ if (!l.frames || !l.bytes || !l.gops) throw std::invalid_argument("Empty video cache budget"); }
void PlaybackFrameCache::evict(std::size_t i)
{
    counters.bytes -= entries[i].bytes; entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
    counters.frames = entries.size(); ++counters.evictions;
}
void PlaybackFrameCache::clear() { while (!entries.empty()) evict(0); }
void PlaybackFrameCache::prune()
{
    for (std::size_t i = entries.size(); i-- > 0;) if (!entries[i].frame->source->current()) evict(i);
    for (;;)
    {
        std::set<std::pair<const VideoIndex*, std::size_t>> gops;
        for (const auto& e : entries) gops.emplace(e.frame->source.get(), e.gop);
        if (entries.size() <= limits.frames && counters.bytes <= limits.bytes && gops.size() <= limits.gops) break;
        evict(0);
    }
}
std::shared_ptr<const PlaybackVideoFrame> PlaybackFrameCache::find(const std::shared_ptr<const VideoIndex>& source, std::size_t packet)
{
    prune();
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (entries[i].frame->source == source && source->current() && entries[i].frame->pts == source->packets.at(packet).pts)
        {
            auto e = std::move(entries[i]); entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
            auto result = e.frame; entries.push_back(std::move(e)); ++counters.hits; return result;
        }
    ++counters.misses; return {};
}
void PlaybackFrameCache::insert(std::shared_ptr<const PlaybackVideoFrame> frame)
{
    prune();
    if (!frame || !frame->source || !frame->source->current()) return;
    const auto& s = *frame->source;
    if (s.width <= 0 || s.height <= 0 || std::uint64_t(s.width) * s.height > limits.bytes / 4) return;
    const auto bytes = std::size_t(s.width) * std::size_t(s.height) * 4;
    for (std::size_t i = entries.size(); i-- > 0;)
        if (entries[i].frame->source == frame->source && entries[i].frame->pts == frame->pts) evict(i);
    const auto packet = std::lower_bound(s.packets.begin(), s.packets.end(), frame->pts, [](const auto& p, auto pts) { return p.pts < pts; });
    if (packet == s.packets.end() || packet->pts != frame->pts) return;
    const auto gop = s.gopAt(packet->sample).first;
    while (!entries.empty() && (entries.size() >= limits.frames || counters.bytes > limits.bytes - bytes)) evict(0);
    entries.push_back({std::move(frame), bytes, gop}); counters.bytes += bytes; counters.frames = entries.size();
    prune(); counters.peakBytes = (std::max)(counters.peakBytes, counters.bytes);
}
// Counts application-owned BGRA allocations across decoder replacement, including
// frames still held by the presenter/cache. Driver/DPB allocations are separate.
struct PlaybackResources
{
    static constexpr std::size_t maxTextures = 32, maxBytes = 256 * 1024 * 1024;
    std::atomic<std::size_t> textures{0}, bytes{0}, peakTextures{0}, peakBytes{0};
    std::atomic<unsigned> decoders{0}, peakDecoders{0}, dpbSurfaces{0}, peakDpbSurfaces{0};
    template<class T> static void peak(std::atomic<T>& p, T n)
    { auto old = p.load(); while (old < n && !p.compare_exchange_weak(old, n)) {} }
    void acquire(std::size_t n)
    {
        const auto count = textures.fetch_add(1) + 1, total = bytes.fetch_add(n) + n;
        if (count > maxTextures || total > maxBytes)
        { --textures; bytes.fetch_sub(n); throw std::runtime_error("Playback BGRA memory/texture budget exceeded"); }
        peak(peakTextures, count); peak(peakBytes, total);
    }
};
struct PlaybackTexture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ID3D11RenderTargetView> target;
    HANDLE shared = nullptr; // legacy DXGI shared handle: NOT CloseHandle-owned
    UINT width = 0, height = 0;
    juce::String adapterName, driverVersion;
    UINT vendorId = 0, deviceId = 0;
    int decoderFramePoolSize = 0;
    std::shared_ptr<PlaybackResources> resources;
    std::size_t resourceBytes = 0;
    ~PlaybackTexture()
    {
        target.Reset(); mutex.Reset(); texture.Reset();
        if (resources) { --resources->textures; resources->bytes.fetch_sub(resourceBytes); }
    }
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
    explicit HardwareDecoder(std::shared_ptr<const VideoIndex> index, std::shared_ptr<PlaybackResources> budget)
        : source(std::move(index)), resources(std::move(budget))
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
        initialiseConversion();
        PlaybackResources::peak(resources->peakDecoders, ++resources->decoders);
    }
    ~HardwareDecoder() override { resources->dpbSurfaces.fetch_sub(dpbCapacity); --resources->decoders; }
    void resetForSeek() override { lastFrame.reset(); }
    PlaybackDecodeTiming decodeTiming() const override { return timing; }
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t target, const std::function<bool()>& cancelled) override
    {
        timing = {}; timing.targetPacket = target;
        const auto& wanted = source->packets.at(target);
        const auto plan = playbackDecodePlan(*source, target, lastFrame);
        const auto idr = source->previousIdr(wanted.sample);
        timing.idrPacket = idr; timing.plannedDecodeOnlyFrames = plan.decodeOnlyFrames;
        timing.seekBeginQpc = qpcNow();
        if (plan.fromIdr)
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
        timing.seekEndQpc = qpcNow(); // equal path endpoints also describe DPB reuse
        const auto decodeBegin = qpcNow();
        timing.prefixBeginQpc = decodeBegin;
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
                if (plan.convert(pts, *source))
                {
                    lastFrame = target; const auto convertBegin = qpcNow();
                    timing.prefixEndQpc = timing.convertBeginQpc = convertBegin;
                    timing.decodeTicks = convertBegin - decodeBegin;
                    if (cancelled()) return {};
                    auto texture = convert(*frame, cancelled);
                    timing.convertTicks = qpcNow() - convertBegin;
                    if (texture) ++timing.convertedFrames;
                    return texture;
                }
                ++timing.decodeOnlyFrames; // no copy, colour conversion, sharing or publication for prefix
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
    void initialiseConversion()
    {
        // The indexed Recorder source is progressive BT.709 limited NV12. A
        // deterministic shader avoids driver video-processor temporal processing.
        // Copy only the target DPB slice into a shader-readable NV12 texture.
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = source->width; desc.Height = source->height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_NV12;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        checkHr(gpu->device->CreateTexture2D(&desc, nullptr, &nv12), "Create reusable NV12 conversion surface");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1; view.Format = DXGI_FORMAT_R8_UNORM;
        checkHr(gpu->device->CreateShaderResourceView(nv12.Get(), &view, &luma), "Create NV12 luma view");
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        checkHr(gpu->device->CreateShaderResourceView(nv12.Get(), &view, &chroma), "Create NV12 chroma view");
        constexpr char shader[] = R"(
Texture2D<float> luma : register(t0); Texture2D<float2> chroma : register(t1);
SamplerState linearSampler : register(s0);
struct Vertex { float4 p : SV_Position; float2 uv : TEXCOORD0; };
Vertex vsMain(uint id : SV_VertexID) { Vertex v; v.uv = float2((id << 1) & 2, id & 2); v.p = float4(v.uv.x*2-1, 1-v.uv.y*2, 0, 1); return v; }
float4 psMain(Vertex v) : SV_Target {
    float y = (luma.Sample(linearSampler, v.uv) * 255.0 - 16.0) / 219.0;
    float2 c = (chroma.Sample(linearSampler, v.uv) * 255.0 - 128.0) / 224.0;
    return float4(saturate(float3(y + 1.5748*c.y, y - 0.187324*c.x - 0.468124*c.y, y + 1.8556*c.x)), 1);
})";
        const auto compile = [&](const char* entry, const char* profile)
        {
            ComPtr<ID3DBlob> code, errors;
            checkHr(D3DCompile(shader, sizeof(shader) - 1, "RecorderPlaybackConvert", nullptr, nullptr,
                entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "Compile NV12 conversion shader");
            return code;
        };
        const auto vs = compile("vsMain", "vs_5_0"), ps = compile("psMain", "ps_5_0");
        checkHr(gpu->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &convertVs), "Create conversion VS");
        checkHr(gpu->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &convertPs), "Create conversion PS");
        D3D11_SAMPLER_DESC sampler{}; sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        checkHr(gpu->device->CreateSamplerState(&sampler, &convertSampler), "Create conversion sampler");
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
        checkHr(gpu->device->CreateRasterizerState(&raster, &convertRaster), "Create conversion rasterizer");
        ComPtr<ID3D11Device5> device5;
        checkHr(gpu->device->QueryInterface(IID_PPV_ARGS(&device5)), "Playback requires D3D11 GPU completion events");
        checkHr(gpu->device_context->QueryInterface(IID_PPV_ARGS(&context4)), "Get playback fence context");
        checkHr(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create reusable playback fence");
    }
    std::shared_ptr<PlaybackTexture> acquireOutput()
    {
        // Never mutate a texture held by an immutable frame or the presenter.
        // Ready queue (4), retained LRU (4), in-flight work and presenter refs.
        // The shared per-camera budget also covers slots surviving old decoders.
        for (const auto& slot : outputs)
            if (slot.use_count() == 1)
            {
                const auto hr = slot->mutex->AcquireSync(1, 0);
                if (hr == S_OK) return slot;
                if (hr != WAIT_TIMEOUT) throw std::runtime_error("Playback reuse mutex failed/abandoned");
            }
        if (outputs.size() == 12) throw std::runtime_error("Playback conversion surface pool exhausted");
        auto output = std::make_shared<PlaybackTexture>(); output->width = source->width; output->height = source->height;
        const auto bytes = std::size_t(output->width) * output->height * 4;
        resources->acquire(bytes); output->resources = resources; output->resourceBytes = bytes;
        output->adapterName = juce::String(adapterDescription.Description); output->vendorId = adapterDescription.VendorId;
        output->deviceId = adapterDescription.DeviceId; output->driverVersion = driverVersion;
        D3D11_TEXTURE2D_DESC texture{}; texture.Width = output->width; texture.Height = output->height;
        texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1; texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.Usage = D3D11_USAGE_DEFAULT; texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        checkHr(gpu->device->CreateTexture2D(&texture, nullptr, &output->texture), "Create shared display texture");
        checkHr(output->texture.As(&output->mutex), "Get shared texture keyed mutex");
        ComPtr<IDXGIResource> resource; checkHr(output->texture.As(&resource), "Get playback shared resource");
        checkHr(resource->GetSharedHandle(&output->shared), "Share display texture");
        checkHr(gpu->device->CreateRenderTargetView(output->texture.Get(), nullptr, &output->target), "Create reusable conversion target");
        outputs.push_back(output);
        if (output->mutex->AcquireSync(0, 0) != S_OK) throw std::runtime_error("Cannot acquire new playback texture");
        return output;
    }
    std::shared_ptr<const PlaybackTexture> convert(const AVFrame& frame, const std::function<bool()>& cancelled)
    {
        if (frame.width != source->width || frame.height != source->height || frame.colorspace != AVCOL_SPC_BT709
            || frame.color_range != AVCOL_RANGE_MPEG) throw std::runtime_error("Expected Recorder BT.709 limited H.264 surface");
        if (cancelled()) return {};
        auto output = acquireOutput();
        struct Unlock { IDXGIKeyedMutex* value; ~Unlock() { value->ReleaseSync(1); } } unlock{output->mutex.Get()};
        if (frame.hw_frames_ctx)
        {
            output->decoderFramePoolSize = reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data)->initial_pool_size;
            if (output->decoderFramePoolSize <= 0 || output->decoderFramePoolSize > 32)
                throw std::runtime_error("D3D11VA DPB capacity outside bounded playback profile");
            if (!dpbCapacity)
            {
                dpbCapacity = unsigned(output->decoderFramePoolSize);
                PlaybackResources::peak(resources->peakDpbSurfaces, resources->dpbSurfaces.fetch_add(dpbCapacity) + dpbCapacity);
            }
        }
        timing.resourcesReadyQpc = qpcNow();
        if (cancelled()) return {};
        {
            ContextLock lock(*gpu);
            auto* context = gpu->device_context;
            const D3D11_BOX crop{0, 0, 0, output->width, output->height, 1};
            context->CopySubresourceRegion(nv12.Get(), 0, 0, 0, 0, reinterpret_cast<ID3D11Texture2D*>(frame.data[0]),
                static_cast<UINT>(reinterpret_cast<std::intptr_t>(frame.data[1])), &crop);
            const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(output->width), static_cast<float>(output->height), 0, 1};
            context->RSSetViewports(1, &viewport); context->RSSetState(convertRaster.Get());
            auto* target = output->target.Get(); context->OMSetRenderTargets(1, &target, nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(convertVs.Get(), nullptr, 0); context->PSSetShader(convertPs.Get(), nullptr, 0);
            auto* sampler = convertSampler.Get(); context->PSSetSamplers(0, 1, &sampler);
            ID3D11ShaderResourceView* views[]{luma.Get(), chroma.Get()}; context->PSSetShaderResources(0, 2, views);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* empty[]{nullptr, nullptr}; context->PSSetShaderResources(0, 2, empty);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            checkHr(context4->Signal(fence.Get(), ++fenceValue), "Signal playback GPU completion");
            context->Flush(); timing.gpuSubmittedQpc = qpcNow();
        }
        // Keep the AVFrame/DPB slice alive until the GPU copy/draw has completed,
        // including cancellation. Wait on a real GPU event, outside FFmpeg's lock.
        checkHr(fence->SetEventOnCompletion(fenceValue, gpuComplete.nativeHandle()), "Arm playback GPU completion event");
        const auto waited = WaitForSingleObject(gpuComplete.nativeHandle(), 2000);
        checkHr(gpu->device->GetDeviceRemovedReason(), "Playback decode device removed");
        if (waited != WAIT_OBJECT_0) throw std::runtime_error("Playback GPU completion event failed/timed out");
        timing.gpuCompleteQpc = qpcNow();
        return cancelled() ? nullptr : output;
    }
    std::shared_ptr<const VideoIndex> source;
    std::shared_ptr<PlaybackResources> resources;
    unsigned dpbCapacity = 0;
    FormatInput input;
    HardwareRef hardware;
    CodecPtr codec;
    AVD3D11VADeviceContext* gpu = nullptr;
    ComPtr<ID3D11Texture2D> nv12;
    ComPtr<ID3D11ShaderResourceView> luma, chroma;
    ComPtr<ID3D11VertexShader> convertVs;
    ComPtr<ID3D11PixelShader> convertPs;
    ComPtr<ID3D11SamplerState> convertSampler;
    ComPtr<ID3D11RasterizerState> convertRaster;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    PlaybackWakeEvent gpuComplete;
    UINT64 fenceValue = 0;
    std::vector<std::shared_ptr<PlaybackTexture>> outputs;
    std::optional<std::size_t> lastFrame;
    bool draining = false;
    PlaybackDecodeTiming timing;
    DXGI_ADAPTER_DESC1 adapterDescription{};
    juce::String driverVersion;
};
constexpr auto absent = static_cast<std::size_t>(-1);
struct VideoClip : PlaybackVideoClip
{
    Sample displayEnd = 0;
    bool beginsAtClipStart = false, endsAtClipEnd = false;
};
using VideoClips = std::vector<VideoClip>;
Sample sourceSampleAt(const VideoClip& c, Sample sample)
{
    // A legacy subframe seam displays the preceding clip's final source sample.
    // Source handles and the incoming clip's first PTS never move.
    return c.mapping.sourceIn + (std::min)(sample - c.mapping.timelineStartSample, c.mapping.lengthSamples - 1);
}
std::size_t activeClip(const VideoClips& clips, Sample sample)
{
    const auto it = std::upper_bound(clips.begin(), clips.end(), sample,
        [](Sample s, const auto& c) { return s < c.mapping.timelineStartSample; });
    if (it == clips.begin()) return absent;
    return sample < std::prev(it)->displayEnd ? static_cast<std::size_t>(std::distance(clips.begin(), it) - 1) : absent;
}
std::pair<std::size_t, std::size_t> frameKey(const VideoClips& clips, Sample sample)
{
    const auto clip = activeClip(clips, sample);
    if (clip == absent) return {absent, absent};
    const auto& c = clips[clip];
    return {clip, c.source->frameAt(sourceSampleAt(c, sample))};
}
std::size_t prerollClipAt(const VideoClips& clips, Sample sample)
{
    const auto next = std::upper_bound(clips.begin(), clips.end(), sample,
        [](Sample s, const auto& c) { return s < c.mapping.timelineStartSample; });
    if (next == clips.end() || next->mapping.timelineStartSample - sample
        > Sample(next->source->sampleRate) * VideoPlaybackEngine::prerollMilliseconds / 1000) return absent;
    return static_cast<std::size_t>(std::distance(clips.begin(), next));
}
std::array<std::shared_ptr<const VideoClips>, 2> validatedClips(std::vector<PlaybackVideoClip> input)
{
    std::array<VideoClips, 2> lanes;
    for (const auto& clip : input)
    {
        const auto& c = clip.mapping;
        if (clip.camera > 1 || !clip.source || !clip.source->current() || c.clipId.isEmpty() || c.timelineStartSample < 0
            || c.sourceIn < 0 || c.lengthSamples <= 0 || c.sourceIn > clip.source->length || c.lengthSamples > clip.source->length - c.sourceIn
            || c.mediaGeneration != static_cast<Sample>(clip.source->generation)
            || c.timelineStartSample > (std::numeric_limits<Sample>::max)() - c.lengthSamples)
            throw std::invalid_argument("Invalid video clip/source generation");
        // RenderClip gaps are clip-relative. Split only the view mapping; source
        // timestamps remain unchanged on both sides of the unavailable interval.
        Sample offset = 0;
        const auto append = [&](Sample begin, Sample end)
        {
            if (begin == end) return;
            VideoClip part; static_cast<PlaybackVideoClip&>(part) = clip;
            part.mapping.timelineStartSample += begin; part.mapping.sourceIn += begin;
            part.mapping.lengthSamples = end - begin; part.mapping.gaps.clear();
            part.displayEnd = part.mapping.timelineStartSample + part.mapping.lengthSamples;
            part.beginsAtClipStart = begin == 0; part.endsAtClipEnd = end == c.lengthSamples;
            lanes[clip.camera].push_back(std::move(part));
        };
        for (const auto& gap : c.gaps)
        {
            if (gap.start < offset || gap.length <= 0 || gap.start > c.lengthSamples || gap.length > c.lengthSamples - gap.start)
                throw std::invalid_argument("Invalid video clip gap");
            append(offset, gap.start); offset = gap.start + gap.length;
        }
        append(offset, c.lengthSamples);
    }
    std::array<std::shared_ptr<const VideoClips>, 2> result;
    for (unsigned i = 0; i < 2; ++i)
    {
        auto& clips = lanes[i];
        std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) { return a.mapping.timelineStartSample < b.mapping.timelineStartSample; });
        Sample end = 0;
        for (const auto& c : clips)
        { if (c.mapping.timelineStartSample < end) throw std::invalid_argument("Video clips overlap"); end = c.mapping.timelineStartSample + c.mapping.lengthSamples; }
        for (std::size_t n = 1; n < clips.size(); ++n)
        {
            auto& before = clips[n - 1]; const auto& after = clips[n];
            const auto gap = after.mapping.timelineStartSample - before.displayEnd;
            if (gap <= 0 || !before.endsAtClipEnd || !after.beginsAtClipStart
                || before.mapping.trackId != after.mapping.trackId || before.mapping.clipId == after.mapping.clipId) continue;
            const auto& last = before.source->packets[before.source->frameAt(before.mapping.sourceIn + before.mapping.lengthSamples - 1)];
            // Recorded video is project-CFR. Use its actual indexed frame duration,
            // including rational rates; never absorb a full frame or an asset gap.
            if (gap < last.endSample - last.sample) before.displayEnd = after.mapping.timelineStartSample;
        }
        result[i] = std::make_shared<const VideoClips>(std::move(clips));
    }
    return result;
}
std::shared_ptr<const PlaybackVideoFrame> mappedFrame(const VideoClip& c, std::size_t packet,
    std::uint64_t generation, std::shared_ptr<const PlaybackTexture> texture)
{
    const auto& p = c.source->packets[packet];
    auto f = std::make_shared<PlaybackVideoFrame>(); f->clipId = c.mapping.clipId; f->pts = p.pts;
    f->begin = c.mapping.timelineStartSample + (std::max)(Sample{0}, p.sample - c.mapping.sourceIn);
    f->end = c.mapping.timelineStartSample + (std::min)(c.mapping.lengthSamples, p.endSample - c.mapping.sourceIn);
    if (p.endSample >= c.mapping.sourceIn + c.mapping.lengthSamples) f->end = c.displayEnd;
    f->generation = generation; f->source = c.source; f->texture = std::move(texture); return f;
}
}

// The playback view belongs to PreviewPresenter but lives in the round-11 module.
// Its immediate context is exclusive to its present thread, on the decoder's
// adapter. Shared BGRA/keyed-mutex interop never maps pixels to CPU memory.
class PreviewPresenter::PlaybackView
{
public:
    PlaybackView(HWND window, VideoPlaybackEngine& source, unsigned camera) : hwnd(window), engine(source), camera(camera)
    {
        if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) throw std::invalid_argument("Playback HWND must attach on its control owner");
        placeholder = CreateWindowExW(0, L"STATIC", L"영상 없음", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!placeholder) throw std::runtime_error("Create playback gap placeholder failed");
        SendMessageW(placeholder, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        if (!SetWindowSubclass(hwnd, overlayProcedure, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
        { DestroyWindow(placeholder); throw std::runtime_error("Attach playback placeholder failed"); }
        updateOverlay(1);
        try { thread = std::thread([this] { run(); }); }
        catch (...) { RemoveWindowSubclass(hwnd, overlayProcedure, reinterpret_cast<UINT_PTR>(this)); DestroyWindow(placeholder); throw; }
    }
    ~PlaybackView()
    {
        stopping.store(true); stopWake.signal(); if (thread.joinable()) thread.join();
        RemoveWindowSubclass(hwnd, overlayProcedure, reinterpret_cast<UINT_PTR>(this)); DestroyWindow(placeholder);
    }
    juce::Result status() const
    { std::lock_guard<std::mutex> lock(mutex); return error.isEmpty() ? juce::Result::ok() : juce::Result::fail(error); }
private:
    static constexpr UINT overlayMessage = WM_APP + 426;
    static LRESULT CALLBACK overlayProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data)
    {
        auto& self = *reinterpret_cast<PlaybackView*>(data);
        if (message == overlayMessage && l == reinterpret_cast<LPARAM>(&self)) { self.updateOverlay(int(w)); return 0; }
        if (message == WM_SIZE || message == WM_DPICHANGED) self.updateOverlay(self.overlayState);
        return DefSubclassProc(window, message, w, l);
    }
    void updateOverlay(int overlayMode) // HWND owner only; worker posts state, never touches UI/GDI
    {
        overlayState = overlayMode; RECT bounds{}; GetClientRect(hwnd, &bounds);
        const auto height = MulDiv(36, int(GetDpiForWindow(hwnd)), 96);
        SetWindowPos(placeholder, HWND_TOP, 0, (bounds.bottom - height) / 2, bounds.right, height, SWP_NOACTIVATE);
        SetWindowTextW(placeholder, overlayMode == 1 ? L"영상 없음" : L"영상 준비 중");
        ShowWindow(placeholder, overlayMode ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    void postOverlay(const PlaybackDisplaySelection& selection)
    {
        const auto overlayMode = selection.gap ? 1 : selection.buffering ? 2 : 0;
        if (overlayMode != postedOverlay)
        { postedOverlay = overlayMode; PostMessageW(hwnd, overlayMessage, WPARAM(overlayMode), reinterpret_cast<LPARAM>(this)); }
    }
    void run();
    HWND hwnd;
    HWND placeholder = nullptr;
    int overlayState = 1, postedOverlay = -1;
    VideoPlaybackEngine& engine;
    unsigned camera;
    std::thread thread;
    std::atomic<bool> stopping{false};
    PlaybackWakeEvent stopWake;
    mutable std::mutex mutex;
    juce::String error;
};
struct VideoPlaybackEngine::Impl
{
    struct Lane
    {
        std::shared_ptr<const VideoClips> clips = std::make_shared<const VideoClips>();
        mutable std::mutex mutex;
        std::condition_variable wake;
        PlaybackWakeEvent presentWake;
        std::thread worker;
        Sample target = 0;
        std::uint64_t request = 0, stale = 0, decoded = 0, prefetched = 0, late = 0;
        std::uint64_t generation = 0, lateGeneration = 0, decoderOpens = 0, cacheHits = 0, cacheMisses = 0;
        std::uint64_t staleRequests = 0, staleReceipts = 0, cancelledTargets = 0, cancelledPrefetch = 0;
        std::pair<std::size_t, std::size_t> lateKey{absent, absent};
        bool advancing = false;
        std::int64_t decoderCreateTicks = 0, presenterBeginQpc = 0, presenterEndQpc = 0;
        std::uint64_t prerollStarts = 0, prerollPrefixFrames = 0;
        Sample prerollLeadSamples = 0;
        std::int64_t prerollTicks = 0;
        PlaybackDecodeTiming prerollDecode;
        std::vector<std::shared_ptr<const PlaybackVideoFrame>> ready;
        PlaybackFrameCache cache;
        std::shared_ptr<PlaybackResources> resources = std::make_shared<PlaybackResources>();
        std::size_t peakReady = 0;
        PlaybackPresentation presented;
        PlaybackSeekTiming timing;
        juce::String error;
    };
    std::array<Lane, 2> lanes;
    std::array<std::unique_ptr<PreviewPresenter::PlaybackView>, 2> views;
    DecoderFactory factory;
    std::shared_ptr<PlaybackWakeEvent> controlWake;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> stopping{false};
    explicit Impl(DecoderFactory f) : factory(std::move(f)) {}
    void notify(Lane& lane)
    {
        lane.presentWake.signal();
        if (const auto event = std::atomic_load(&controlWake)) event->signal();
    }
    void seekLocked(Sample sample, std::uint64_t gen)
    {
        generation.store(gen);
        for (auto& lane : lanes)
        {
            lane.target = sample; lane.generation = gen; lane.advancing = false; ++lane.request;
            lane.timing = {}; lane.timing.generation = gen; lane.timing.target = sample; lane.timing.requestedQpc = qpcNow();
            // Hits never queue behind an in-flight GPU conversion/prefetch. This
            // non-RT owner only allocates metadata; it never touches a GPU context.
            const auto [which, packet] = frameKey(*lane.clips, sample);
            if (which != absent)
            {
                const auto& c = (*lane.clips)[which];
                const auto hit = lane.cache.find(c.source, packet);
                if (hit)
                {
                    lane.ready = {mappedFrame(c, packet, gen, hit->texture)};
                    lane.timing.cacheKnown = lane.timing.cacheHit = true;
                    lane.timing.readyQpc = qpcNow(); ++lane.cacheHits;
                }
            }
            else lane.timing.readyQpc = qpcNow(); // explicit gap needs no decoder or DXGI receipt
            notify(lane);
        }
    }
    // Keep two current pictures plus two next-segment pictures independently of
    // the LRU. Publishing a current frame must not erase a completed preroll.
    static bool wantedNow(const Lane& lane, const PlaybackVideoFrame& f)
    {
        if (!f.current(lane.generation)) return false;
        const auto& clips = *lane.clips;
        const auto [clip, packet] = frameKey(clips, lane.target);
        const auto next = prerollClipAt(clips, lane.target);
        const auto matches = [&](std::size_t which, std::size_t first)
        {
            if (which == absent) return false;
            const auto& c = clips[which];
            if (f.source != c.source || f.clipId != c.mapping.clipId || f.begin < c.mapping.timelineStartSample
                || f.end > c.displayEnd) return false;
            return f.pts == c.source->packets[first].pts
                || (first + 1 < c.source->packets.size() && f.pts == c.source->packets[first + 1].pts);
        };
        return matches(clip, packet) || (next != absent && matches(next, clips[next].source->frameAt(clips[next].mapping.sourceIn)));
    }
    static void pruneReady(Lane& lane)
    {
        lane.ready.erase(std::remove_if(lane.ready.begin(), lane.ready.end(),
            [&](const auto& f) { return !wantedNow(lane, *f); }), lane.ready.end());
    }
    void publishFrame(Lane& lane, const std::shared_ptr<const PlaybackVideoFrame>& f)
    {
        lane.cache.insert(f); pruneReady(lane);
        if (!wantedNow(lane, *f)) return;
        lane.ready.erase(std::remove_if(lane.ready.begin(), lane.ready.end(), [&](const auto& old)
            { return old->clipId == f->clipId && old->begin == f->begin; }), lane.ready.end());
        lane.ready.push_back(f); lane.peakReady = (std::max)(lane.peakReady, lane.ready.size());
        notify(lane);
    }
    std::future<std::unique_ptr<IVideoFrameDecoder>> startPreroll(Lane& lane, std::shared_ptr<const VideoClips> plan,
        std::size_t next, std::uint64_t gen, std::unique_ptr<IVideoFrameDecoder> decoder)
    {
        return std::async(std::launch::async, [this, &lane, plan, next, gen, decoder = std::move(decoder)]() mutable
        {
            const auto& c = (*plan)[next];
            const auto cancelled = [&]
            {
                if (stopping.load() || generation.load() != gen || !c.source->current()) return true;
                std::lock_guard<std::mutex> lock(lane.mutex);
                return lane.clips != plan || (activeClip(*plan, lane.target) != next && prerollClipAt(*plan, lane.target) != next);
            };
            const auto begin = qpcNow();
            try
            {
                ComApartment apartment;
                { std::lock_guard<std::mutex> lock(lane.mutex); ++lane.prerollStarts; lane.prerollLeadSamples = c.mapping.timelineStartSample - lane.target; }
                if (!cancelled() && !decoder)
                {
                    decoder = factory ? factory(c.source) : std::make_unique<HardwareDecoder>(c.source, lane.resources);
                    if (!decoder) throw std::runtime_error("Playback preroll factory returned null");
                    std::lock_guard<std::mutex> lock(lane.mutex);
                    ++lane.decoderOpens; lane.decoderCreateTicks += qpcNow() - begin;
                }
                const auto first = c.source->frameAt(c.mapping.sourceIn);
                for (auto packet = first; packet < c.source->packets.size() && packet < first + 2 && !cancelled(); ++packet)
                {
                    if (c.source->packets[packet].sample >= c.mapping.sourceIn + c.mapping.lengthSamples) break;
                    auto texture = decoder->decodeFrame(packet, cancelled);
                    if (cancelled())
                    {
                        decoder->resetForSeek(); std::lock_guard<std::mutex> lock(lane.mutex);
                        ++lane.stale; ++lane.cancelledPrefetch; break;
                    }
                    const auto timing = decoder->decodeTiming();
                    auto frame = mappedFrame(c, packet, gen, std::move(texture));
                    std::lock_guard<std::mutex> lock(lane.mutex);
                    if (generation.load() != gen) break;
                    ++lane.decoded; ++lane.prefetched;
                    if (packet == first)
                    { lane.prerollTicks = qpcNow() - begin; lane.prerollPrefixFrames = timing.decodeOnlyFrames; lane.prerollDecode = timing; }
                    publishFrame(lane, frame);
                }
            }
            catch (const std::exception& e)
            {
                if (!cancelled())
                {
                    std::lock_guard<std::mutex> lock(lane.mutex);
                    if (generation.load() == gen && lane.clips == plan && c.source->current())
                        lane.error = juce::String::fromUTF8(e.what());
                }
                decoder.reset();
            }
            { std::lock_guard<std::mutex> lock(lane.mutex); ++lane.request; notify(lane); }
            lane.wake.notify_one(); return std::move(decoder);
        });
    }
    void run(unsigned camera)
    {
        auto& lane = lanes[camera];
        std::uint64_t processed = 0;
        std::map<std::size_t, std::unique_ptr<IVideoFrameDecoder>> decoders;
        std::shared_ptr<const VideoClips> decoderPlan;
        // A prefix job owns its decoder exclusively. At the cut the camera
        // worker adopts that warm DPB after the job has completed.
        std::future<std::unique_ptr<IVideoFrameDecoder>> preroll;
        std::size_t prerollClip = absent, primedClip = absent;
        std::uint64_t prerollGeneration = 0, primedGeneration = 0;
        try
        {
            ComApartment apartment;
            while (!stopping.load())
            {
                Sample target; std::uint64_t request, gen; bool advancing;
                std::shared_ptr<const VideoClips> plan;
                {
                    std::unique_lock<std::mutex> lock(lane.mutex);
                    lane.wake.wait(lock, [&] { return stopping.load() || lane.request != processed; });
                    if (stopping.load()) break;
                    target = lane.target; request = lane.request; gen = lane.generation; plan = lane.clips;
                    advancing = lane.advancing;
                    if (lane.timing.generation == gen && !lane.timing.cacheHit && !lane.timing.workerQpc)
                        lane.timing.workerQpc = qpcNow();
                }
                processed = request;
                const auto& clips = *plan;
                const auto cancelled = [&] { return stopping.load() || generation.load() != gen; };
                const auto clip = activeClip(clips, target);
                const auto next = prerollClipAt(clips, target);
                if (preroll.valid())
                {
                    const bool obsolete = prerollGeneration != gen || decoderPlan != plan
                        || (prerollClip != clip && prerollClip != next);
                    // Current playback never waits on next-segment prefix work.
                    if (obsolete || prerollClip == clip || preroll.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    {
                        auto decoder = preroll.get();
                        if (!obsolete && decoder) decoders[prerollClip] = std::move(decoder);
                    }
                }
                if (decoderPlan != plan) { decoders.clear(); decoderPlan = plan; }
                if (clip == absent)
                {
                    { std::lock_guard<std::mutex> lock(lane.mutex); if (!cancelled()) { pruneReady(lane); notify(lane); } }
                    if (!cancelled() && next != absent && !preroll.valid() && (primedClip != next || primedGeneration != gen))
                    {
                        primedClip = prerollClip = next; primedGeneration = prerollGeneration = gen;
                        decoders.clear(); preroll = startPreroll(lane, plan, next, gen, {});
                    }
                    continue;
                }
                for (auto it = decoders.begin(); it != decoders.end();)
                    if (it->first != clip && it->first != next) it = decoders.erase(it); else ++it;
                const auto& current = clips[clip]; const auto& mapping = current.mapping;
                const auto frame = current.source->frameAt(sourceSampleAt(current, target));
                std::vector<std::pair<std::size_t, std::size_t>> wanted{{clip, frame}};
                if (advancing && frame + 1 < current.source->packets.size()
                    && current.source->packets[frame + 1].sample < mapping.sourceIn + mapping.lengthSamples) wanted.push_back({clip, frame + 1});
                std::vector<std::shared_ptr<const PlaybackVideoFrame>> ready;
                for (const auto& [which, packet] : wanted)
                {
                    if (cancelled()) break;
                    // Exact target is published first. Superseded cursor work
                    // must not wait for optional next-clip/next-frame prefetch.
                    const bool exactTarget = ready.empty();
                    const auto cancelDecode = [&]
                    {
                        if (cancelled()) return true;
                        std::lock_guard<std::mutex> lock(lane.mutex);
                        const auto key = frameKey(*lane.clips, lane.target);
                        return !exactTarget && (!lane.advancing || key.first != clip || key.second > packet);
                    };
                    if (cancelDecode()) break;
                    const auto& c = clips[which];
                    if (!c.source->current()) break; // buffering until finalized media handoff
                    std::shared_ptr<const PlaybackVideoFrame> hit;
                    bool measureSeek = false;
                    {
                        std::lock_guard<std::mutex> lock(lane.mutex);
                        hit = lane.cache.find(c.source, packet);
                        if (!hit) for (const auto& f : lane.ready)
                            if (f->current(gen) && f->source == c.source && f->pts == c.source->packets[packet].pts) { hit = f; break; }
                        measureSeek = exactTarget && lane.timing.generation == gen && !lane.timing.readyQpc;
                        if (measureSeek)
                        {
                            lane.timing.cacheKnown = true; lane.timing.cacheHit = bool(hit);
                            if (lane.timing.cacheHit) ++lane.cacheHits; else ++lane.cacheMisses;
                        }
                    }
                    std::shared_ptr<const PlaybackTexture> texture;
                    if (hit) texture = hit->texture;
                    else
                    {
                        auto& decoder = decoders[which];
                        if (!decoder)
                        {
                            const auto begin = qpcNow();
                            { std::lock_guard<std::mutex> lock(lane.mutex); if (measureSeek && lane.timing.generation == gen) lane.timing.decoderBeginQpc = begin; }
                            try { decoder = factory ? factory(c.source) : std::make_unique<HardwareDecoder>(c.source, lane.resources); }
                            catch (...) { if (cancelled() || !c.source->current()) break; throw; }
                            if (!decoder) throw std::runtime_error("Playback decoder factory returned null");
                            const auto finish = qpcNow();
                            std::lock_guard<std::mutex> lock(lane.mutex);
                            ++lane.decoderOpens; lane.decoderCreateTicks += finish - begin;
                            if (measureSeek && lane.timing.generation == gen) lane.timing.decoderEndQpc = finish;
                        }
                        if (cancelled()) break;
                        { std::lock_guard<std::mutex> lock(lane.mutex); if (measureSeek && lane.timing.generation == gen) lane.timing.decodeBeginQpc = qpcNow(); }
                        try { texture = decoder->decodeFrame(packet, cancelDecode); }
                        catch (...)
                        { if (cancelled() || !c.source->current()) { decoder->resetForSeek(); break; } throw; }
                        const auto finish = qpcNow();
                        const bool discarded = cancelDecode();
                        if (discarded) decoder->resetForSeek(); // completed seeks preserve valid sequential position
                        std::lock_guard<std::mutex> lock(lane.mutex);
                        if (measureSeek && lane.timing.generation == gen)
                        { lane.timing.decodeEndQpc = finish; lane.timing.decode = decoder->decodeTiming(); }
                        if (discarded)
                        {
                            ++lane.stale;
                            if (exactTarget) ++lane.cancelledTargets; else ++lane.cancelledPrefetch;
                            break;
                        }
                        ++lane.decoded;
                    }
                    if (cancelled() || !c.source->current()) break;
                    auto f = mappedFrame(c, packet, gen, std::move(texture)); ready.push_back(f);
                    std::lock_guard<std::mutex> lock(lane.mutex);
                    if (cancelled()) break;
                    if (std::any_of(ready.begin(), ready.end(), [gen](const auto& item) { return !item->current(gen); })) break;
                    publishFrame(lane, f);
                    if (measureSeek) lane.timing.readyQpc = qpcNow();
                    notify(lane);
                }
                if (!cancelled() && next != absent && !preroll.valid() && (primedClip != next || primedGeneration != gen))
                {
                    primedClip = prerollClip = next; primedGeneration = prerollGeneration = gen;
                    auto decoder = std::move(decoders[next]); decoders.erase(next);
                    preroll = startPreroll(lane, plan, next, gen, std::move(decoder));
                }
            }
        }
        catch (const std::exception& e)
        { std::lock_guard<std::mutex> lock(lane.mutex); lane.error = juce::String::fromUTF8(e.what()); notify(lane); }
        if (preroll.valid()) preroll.wait(); // submitted GPU fence finishes before lane resources are released
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
    for (auto& lane : impl->lanes) { std::lock_guard<std::mutex> lock(lane.mutex); lane.ready.clear(); lane.cache.clear(); }
}
void VideoPlaybackEngine::prepare(std::vector<PlaybackVideoClip> clips)
{
    auto plans = validatedClips(std::move(clips));
    stop();
    for (unsigned camera = 0; camera < 2; ++camera)
    {
        auto& lane = impl->lanes[camera]; lane.clips = std::move(plans[camera]);
        lane.error.clear(); lane.request = lane.generation = 0; lane.target = 0;
        lane.presented = {}; lane.timing = {}; lane.advancing = false;
        lane.stale = lane.decoded = lane.prefetched = lane.late = lane.lateGeneration = 0;
        lane.decoderOpens = lane.cacheHits = lane.cacheMisses = 0; lane.lateKey = {absent, absent};
        lane.staleRequests = lane.staleReceipts = lane.cancelledTargets = lane.cancelledPrefetch = 0;
        lane.decoderCreateTicks = lane.presenterBeginQpc = lane.presenterEndQpc = 0;
        lane.prerollStarts = lane.prerollPrefixFrames = 0; lane.prerollLeadSamples = lane.prerollTicks = 0;
        lane.prerollDecode = {};
        lane.peakReady = 0;
    }
    impl->generation.store(0); impl->stopping.store(false);
    for (unsigned camera = 0; camera < 2; ++camera) impl->lanes[camera].worker = std::thread([this, camera] { impl->run(camera); });
}
void VideoPlaybackEngine::handoff(std::vector<PlaybackVideoClip> clips, Sample sample, std::uint64_t gen)
{
    auto plans = validatedClips(std::move(clips));
    if (sample < 0 || gen <= impl->generation.load() || impl->stopping.load()) throw std::invalid_argument("Invalid video plan handoff generation");
    {
        std::scoped_lock lock(impl->lanes[0].mutex, impl->lanes[1].mutex);
        // Plan, generation and common target are published under both lane locks.
        for (unsigned i = 0; i < 2; ++i)
        { impl->lanes[i].clips = std::move(plans[i]); impl->lanes[i].ready.clear(); impl->lanes[i].cache.clear(); }
        impl->seekLocked(sample, gen);
    }
    for (auto& lane : impl->lanes) lane.wake.notify_one();
}
std::uint64_t VideoPlaybackEngine::seek(Sample sample) { const auto gen = impl->generation.load() + 1; seek(sample, gen); return gen; }
void VideoPlaybackEngine::seek(Sample sample, std::uint64_t gen)
{
    if (sample < 0 || !gen || gen <= impl->generation.load()) throw std::invalid_argument("Seek generation must increase");
    {
        // Pair target and generation under the same locks. A worker must never
        // adopt the new generation with the previous lane's target.
        std::scoped_lock lock(impl->lanes[0].mutex, impl->lanes[1].mutex);
        impl->seekLocked(sample, gen);
    }
    for (auto& lane : impl->lanes) lane.wake.notify_one();
}
bool VideoPlaybackEngine::requestFrame(unsigned camera, Sample sample, std::uint64_t gen, bool advancing)
{
    if (camera > 1 || sample < 0) throw std::invalid_argument("Invalid camera/frame request");
    auto& lane = impl->lanes[camera];
    { std::lock_guard<std::mutex> lock(lane.mutex);
      if (gen != impl->generation.load()) { ++lane.stale; ++lane.staleRequests; return false; }
      const bool advancingChanged = lane.advancing != advancing;
      lane.advancing = advancing;
      const auto sameFrame = frameKey(*lane.clips, lane.target) == frameKey(*lane.clips, sample)
          && prerollClipAt(*lane.clips, lane.target) == prerollClipAt(*lane.clips, sample);
      lane.target = sample;
      if (sameFrame && !advancingChanged) return true;
      ++lane.request; lane.presentWake.signal(); }
    lane.wake.notify_one(); return true;
}
bool VideoPlaybackEngine::requestFrames(Sample sample, std::uint64_t gen, bool advancing)
{
    if (sample < 0) throw std::invalid_argument("Invalid audible sample");
    std::array<bool, 2> changed{};
    {
        std::scoped_lock lock(impl->lanes[0].mutex, impl->lanes[1].mutex);
        if (gen != impl->generation.load()) return false;
        for (unsigned i = 0; i < 2; ++i)
        {
            auto& lane = impl->lanes[i];
            changed[i] = lane.advancing != advancing || frameKey(*lane.clips, lane.target) != frameKey(*lane.clips, sample)
                || prerollClipAt(*lane.clips, lane.target) != prerollClipAt(*lane.clips, sample);
            lane.target = sample; lane.advancing = advancing;
            if (changed[i]) { ++lane.request; lane.presentWake.signal(); }
        }
    }
    for (unsigned i = 0; i < 2; ++i) if (changed[i]) impl->lanes[i].wake.notify_one();
    return true;
}
bool VideoPlaybackEngine::ready(Sample sample, std::uint64_t gen) const
{
    if (gen != impl->generation.load()) return false;
    for (const auto& lane : impl->lanes)
    {
        std::lock_guard<std::mutex> lock(lane.mutex);
        if (lane.generation != gen) return false;
        if (activeClip(*lane.clips, sample) == absent) continue;
        if (std::none_of(lane.ready.begin(), lane.ready.end(), [&](const auto& f) { return f->current(gen) && f->begin <= sample && sample < f->end; })) return false;
    }
    return gen == impl->generation.load();
}
PlaybackDisplaySelection VideoPlaybackEngine::displaySelection(unsigned camera, bool presentationTick) const
{
    auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex);
    PlaybackDisplaySelection selected; selected.generation = impl->generation.load();
    selected.gap = activeClip(*lane.clips, lane.target) == absent;
    if (!selected.gap)
    {
        for (const auto& f : lane.ready) if (f->current(selected.generation) && f->begin <= lane.target && lane.target < f->end) { selected.frame = f; break; }
        if (!selected.frame && presentationTick && lane.advancing && lane.timing.readyQpc
            && lane.generation == selected.generation)
        {
            const auto key = frameKey(*lane.clips, lane.target);
            if (lane.lateGeneration != selected.generation || lane.lateKey != key)
            { ++lane.late; lane.lateGeneration = selected.generation; lane.lateKey = key; }
        }
    }
    selected.buffering = !selected.gap && !selected.frame;
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
    else { ++lane.stale; ++lane.staleReceipts; }
    if (const auto event = std::atomic_load(&impl->controlWake)) event->signal();
}
bool VideoPlaybackEngine::submitIfCurrent(unsigned camera, std::uint64_t generation, const std::function<void()>& submit)
{
    auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex);
    if (generation != impl->generation.load() || generation != lane.generation) return false;
    // Only the nonblocking DXGI Present call is inside this fence. GPU drawing,
    // resource creation and waits remain outside it. A seek cannot slip between
    // generation validation and submission of old pixels.
    submit(); return true;
}
PlaybackPresentation VideoPlaybackEngine::lastPresentation(unsigned camera) const
{ const auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex); return lane.presented; }
PlaybackSeekTiming VideoPlaybackEngine::seekTiming(unsigned camera) const
{ const auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex); return lane.timing; }
void VideoPlaybackEngine::setWakeEvent(std::shared_ptr<PlaybackWakeEvent> event)
{
    if (std::atomic_load(&impl->controlWake) != event) std::atomic_store(&impl->controlWake, std::move(event));
}
void* VideoPlaybackEngine::presentationWakeHandle(unsigned camera) const
{ return impl->lanes.at(camera).presentWake.nativeHandle(); }
void VideoPlaybackEngine::presenterInitialised(unsigned camera, std::int64_t begin, std::int64_t end)
{
    auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex);
    lane.presenterBeginQpc = begin; lane.presenterEndQpc = end;
}
void VideoPlaybackEngine::presenterFailed(unsigned camera, const juce::String& error)
{
    auto& lane = impl->lanes.at(camera); std::lock_guard<std::mutex> lock(lane.mutex);
    lane.error = error; impl->notify(lane);
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
    jsonSet(result, "interop", "Dedicated decoder/presenter devices; target-only GPU NV12 copy + BT.709 shader to reusable shared BGRA; keyed mutex + D3D11 fence event; no CPU pixel readback");
    jsonSet(result, "displayReadyLimitPerCamera", maximumReadyFrames); jsonSet(result, "decoderDpb", "FFmpeg-owned per decoder; current plus next clip decoder");
    jsonSet(result, "prerollMilliseconds", prerollMilliseconds);
    jsonSet(result, "prerollPolicy", "Independent next-segment IDR prefix, two retained pictures; survives advancing cursor requests; generation/gap/source fenced");
    jsonSet(result, "conversionSurfaceLimitPerDecoder", 12);
    jsonSet(result, "conversionTextureLimitPerCamera", PlaybackResources::maxTextures);
    jsonSet(result, "conversionByteLimitPerCamera", PlaybackResources::maxBytes);
    jsonSet(result, "resourceDefinition", "BGRA counts/bytes are actual application allocations including old decoder cache/presenter references. DPB counts are FFmpeg declared capacities observed on decoded frames, not measured driver VRAM. NV12 scratch, swapchains and driver overhead are separate. No proxy; full source resolution decoded.");
    jsonSet(result, "readinessNotification", "GPU fence event -> immediate frame publication -> independent presenter/coordinator events; resident seek hit publishes on control owner");
    jsonSet(result, "lateDefinition", "Unique missing containing frames at advancing presentation ticks; excludes seek preparation, gap/startup/UI queries and repeated ticks for the same frame");
    jsonSet(result, "staleDefinition", "Sum of rejected old-generation requests, rejected Present receipts, cancelled target decodes and cancelled optional prefetch decodes; NOT IDR prefix frames or visible drops");
    for (const auto& lane : impl->lanes)
    {
        std::lock_guard<std::mutex> lock(lane.mutex); auto c = jsonObject();
        jsonSet(c, "readyFrames", lane.ready.size()); jsonSet(c, "decoded", lane.decoded); jsonSet(c, "nextClipPrefetched", lane.prefetched);
        jsonSet(c, "prerollStarts", lane.prerollStarts); jsonSet(c, "prerollLeadSamples", lane.prerollLeadSamples);
        jsonSet(c, "prerollFirstFrameMs", 1000.0 * lane.prerollTicks / qpcFrequency());
        jsonSet(c, "prerollPrefixFrames", lane.prerollPrefixFrames);
        jsonSet(c, "prerollIdrSeekMs", 1000.0 * lane.prerollDecode.idrSeekTicks / qpcFrequency());
        jsonSet(c, "prerollFlushMs", 1000.0 * lane.prerollDecode.flushTicks / qpcFrequency());
        jsonSet(c, "prerollDecodeMs", 1000.0 * lane.prerollDecode.decodeTicks / qpcFrequency());
        jsonSet(c, "prerollConvertMs", 1000.0 * lane.prerollDecode.convertTicks / qpcFrequency());
        const auto cache = lane.cache.stats(); const auto& resources = *lane.resources;
        jsonSet(c, "peakReadyFrames", lane.peakReady); jsonSet(c, "frameCacheFrames", cache.frames);
        jsonSet(c, "frameCacheBytes", cache.bytes); jsonSet(c, "frameCachePeakBytes", cache.peakBytes);
        jsonSet(c, "frameCacheLimitBytes", lane.cache.limits.bytes); jsonSet(c, "frameCacheLimitFrames", lane.cache.limits.frames);
        jsonSet(c, "frameCacheEvictions", cache.evictions); jsonSet(c, "frameCacheLimitGops", lane.cache.limits.gops);
        jsonSet(c, "conversionTextures", resources.textures.load()); jsonSet(c, "conversionPeakTextures", resources.peakTextures.load());
        jsonSet(c, "conversionBytes", resources.bytes.load()); jsonSet(c, "conversionPeakBytes", resources.peakBytes.load());
        jsonSet(c, "liveDecoders", resources.decoders.load()); jsonSet(c, "peakDecoders", resources.peakDecoders.load());
        jsonSet(c, "observedDpbCapacity", resources.dpbSurfaces.load()); jsonSet(c, "peakObservedDpbCapacity", resources.peakDpbSurfaces.load());
        jsonSet(c, "staleDiscarded", lane.stale); jsonSet(c, "lateDisplaySelections", lane.late); jsonSet(c, "error", lane.error); cameras.add(c);
        jsonSet(c, "staleFrameRequests", lane.staleRequests); jsonSet(c, "stalePresentReceipts", lane.staleReceipts);
        jsonSet(c, "cancelledTargetDecodes", lane.cancelledTargets); jsonSet(c, "cancelledPrefetchDecodes", lane.cancelledPrefetch);
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
        jsonSet(seek, "plannedDecodeOnlyFrames", t.decode.plannedDecodeOnlyFrames);
        jsonSet(seek, "decodeOnlyFrames", t.decode.decodeOnlyFrames); jsonSet(seek, "convertedFrames", t.decode.convertedFrames);
        jsonSet(seek, "decoderSeekBeginQpc", t.decode.seekBeginQpc); jsonSet(seek, "decoderSeekEndQpc", t.decode.seekEndQpc);
        jsonSet(seek, "prefixDecodeBeginQpc", t.decode.prefixBeginQpc); jsonSet(seek, "prefixDecodeEndQpc", t.decode.prefixEndQpc);
        jsonSet(seek, "targetConvertBeginQpc", t.decode.convertBeginQpc); jsonSet(seek, "targetResourcesReadyQpc", t.decode.resourcesReadyQpc);
        jsonSet(seek, "gpuSubmittedQpc", t.decode.gpuSubmittedQpc); jsonSet(seek, "gpuCompleteQpc", t.decode.gpuCompleteQpc);
        const auto duration = [&](std::int64_t begin, std::int64_t finish)
        { return begin && finish ? juce::var(ms(finish - begin)) : juce::var(); };
        jsonSet(seek, "requestToReadyMs", duration(t.requestedQpc, t.readyQpc));
        jsonSet(seek, "targetResourceMs", duration(t.decode.convertBeginQpc, t.decode.resourcesReadyQpc));
        jsonSet(seek, "targetSubmitMs", duration(t.decode.resourcesReadyQpc, t.decode.gpuSubmittedQpc));
        jsonSet(seek, "gpuCompletionWaitMs", duration(t.decode.gpuSubmittedQpc, t.decode.gpuCompleteQpc));
        jsonSet(seek, "readyToDxgiReceiptMs", duration(t.readyQpc, t.presentQpc));
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
        const HANDLE startupEvents[]{stopWake.nativeHandle(), engine.presentationWakeHandle(camera)};
        PlaybackDisplaySelection first;
        while (!stopping.load())
        {
            first = engine.displaySelection(camera);
            postOverlay(first);
            if (first.frame && first.frame->texture) break;
            const auto result = WaitForMultipleObjects(2, startupEvents, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) return;
            if (result != WAIT_OBJECT_0 + 1) throw std::runtime_error("Wait for first playback frame failed");
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
        PlaybackDisplayState display;
        first = {}; // do not retain the startup frame/decoder device forever
        decoderDevice.Reset(); dxgi.Reset();
        struct TimerHandle { HANDLE value; ~TimerHandle() { if (value) CloseHandle(value); } } timer{
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
        if (!timer.value) timer.value = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!timer.value) throw std::runtime_error("Create playback cadence timer failed");
        const auto period = qpcFrequency() / 60;
        auto nextTick = qpcNow(); bool immediate = true;
        while (!stopping.load())
        {
            // Cadence is only for normal playback/visibility retries. A newly
            // published seek/cache frame interrupts it and submits at the next
            // DXGI opportunity without waiting for a presentation timer tick.
            bool presentationTick = false;
            if (!immediate)
            {
                const auto remaining = (std::max)(std::int64_t{1}, nextTick - qpcNow());
                LARGE_INTEGER due{}; due.QuadPart = -(std::max)(std::int64_t{1}, remaining * 10000000 / qpcFrequency());
                if (!SetWaitableTimer(timer.value, &due, 0, nullptr, nullptr, FALSE)) throw std::runtime_error("Arm playback cadence timer failed");
                const HANDLE events[]{stopWake.nativeHandle(), engine.presentationWakeHandle(camera), timer.value};
                const auto result = WaitForMultipleObjects(3, events, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0) break;
                if (result != WAIT_OBJECT_0 + 1 && result != WAIT_OBJECT_0 + 2) throw std::runtime_error("Wait for playback handoff failed");
                presentationTick = result == WAIT_OBJECT_0 + 2;
            }
            immediate = false; presentationTick |= qpcNow() >= nextTick;
            if (presentationTick) nextTick = qpcNow() + period;
            if (pacing.needsVisibilityTest())
            { const auto hr = swap->Present(0, DXGI_PRESENT_TEST); checkHr(hr, "Test playback visibility"); pacing.visibilityTest(hr); if (pacing.needsVisibilityTest()) continue; }
            if (opportunity.needsWait())
            {
                const HANDLE events[]{stopWake.nativeHandle(), latency.value};
                const auto wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0) break;
                if (wait != WAIT_OBJECT_0 + 1) throw std::runtime_error("Playback latency wait failed");
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
            const auto selection = engine.displaySelection(camera, presentationTick);
            const auto decision = display.select(selection);
            const auto& displayed = decision.frame;
            auto overlay = selection;
            if (displayed) overlay.buffering = false;
            postOverlay(overlay);
            // Startup can skip; invalidated on-screen pixels require a clear
            // even while a valid replacement clip is still buffering.
            if (!decision.shouldSubmit()) continue;
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
            if (current.generation != selection.generation || current.gap != selection.gap
                || (displayed && !displayed->source->current())) continue;
            HRESULT hr = S_FALSE; std::int64_t received = 0;
            if (!engine.submitIfCurrent(camera, current.generation, [&]
                { hr = swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT); received = qpcNow(); })) continue;
            pacing.presented(hr);
            opportunity.submitted(hr == S_OK);
            if (hr == DXGI_STATUS_OCCLUDED || hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            checkHr(hr, "Playback Present");
            if (hr != S_OK) continue;
            display.submitted(displayed);
            // A held pre-seek picture is visible continuity, never an exact
            // receipt for the new generation (and never a stale decode result).
            if (selection.frame && displayed && displayed->texture) engine.presented(camera, *displayed, received);
        }
        context->ClearState(); context->Flush();
    }
    catch (const std::exception& e)
    {
        { std::lock_guard<std::mutex> lock(mutex); error = juce::String::fromUTF8(e.what()); }
        engine.presenterFailed(camera, juce::String::fromUTF8(e.what()));
    }
}
}
