#include "NvencEncoder.h"
#include <algorithm>
#include <cstring>

namespace gocue::recorder
{
void NvencProfile::validate() const
{
    if ((fps != 30 && fps != 60) || preset != "p5") throw std::invalid_argument("Round 02 profile requires --project-fps 30|60 and --preset p5");
}
juce::var NvencProfile::toJson() const
{
    auto value = jsonObject();
    jsonSet(value, "codec", "h264_nvenc"); jsonSet(value, "preset", preset); jsonSet(value, "rateControl", "vbr");
    jsonSet(value, "width", 1920); jsonSet(value, "height", 1080); jsonSet(value, "fps", fps);
    jsonSet(value, "bitRate", jsonInt(bitRate())); jsonSet(value, "maxRate", jsonInt(maxRate()));
    jsonSet(value, "gop", fps); jsonSet(value, "forced-idr", true); jsonSet(value, "closedGop", true);
    jsonSet(value, "bFrames", 0); jsonSet(value, "rc-lookahead", 0); jsonSet(value, "multipass", "disabled");
    jsonSet(value, "surfaceLimit", surfaceLimit()); jsonSet(value, "cpuSurfaceCapacity", cpuSurfaces());
    jsonSet(value, "nvencSurfaces", hardwareSurfaces); jsonSet(value, "delay", 0);
    jsonSet(value, "colour", VideoSurface::outputFormat);
    jsonSet(value, "inputPath", "CPU NV12; one copy from decoded preview-owned bytes into a separate AVFrame pool, then FFmpeg/NVENC host-to-device upload. No D3D11/CUDA interop.");
    return value;
}
NvencFramePool::NvencFramePool(int c, int w, int h) : limit(c), width(w), height(h)
{
    if (c < 2 || c > 15 || w <= 0 || h <= 0 || w % 2 || h % 2) throw std::invalid_argument("Invalid NVENC frame pool");
    for (int i = 0; i < c; ++i)
    {
        auto& f = frames[static_cast<size_t>(i)]; f = ffFrame();
        f->format = AV_PIX_FMT_NV12; f->width = w; f->height = h;
        f->colorspace = AVCOL_SPC_BT709; f->color_range = AVCOL_RANGE_MPEG;
        f->color_primaries = AVCOL_PRI_BT709; f->color_trc = AVCOL_TRC_BT709;
        ffCheck(av_frame_get_buffer(f.get(), 32), "Allocate independent encode surface");
    }
}
bool NvencFramePool::copy(const VideoSurface& source, std::uint64_t clockRevision) noexcept
{
    if (source.width != static_cast<unsigned>(width) || source.height != static_cast<unsigned>(height)
        || source.nv12.size() != static_cast<size_t>(width) * height * 3 / 2) return false;
    for (int i = 0; i < limit; ++i)
    {
        auto& state = owned[static_cast<size_t>(i)]; bool expected = false;
        if (!state.compare_exchange_strong(expected, true, std::memory_order_acquire)) continue;
        auto& f = *frames[static_cast<size_t>(i)];
        // Never call make_writable here: that could allocate an extra unbudgeted buffer.
        if (!av_frame_is_writable(&f)) { state.store(false, std::memory_order_release); continue; }
        for (int p = 0; p < 2; ++p)
        {
            const auto* src = source.nv12.data() + (p ? static_cast<size_t>(width) * height : 0);
            for (int y = 0; y < (p ? height / 2 : height); ++y)
                std::memcpy(f.data[p] + static_cast<size_t>(y) * f.linesize[p], src + static_cast<size_t>(y) * width, width);
        }
        stamps[static_cast<size_t>(i)] = source.stamp;
        clockRevisions[static_cast<size_t>(i)] = clockRevision;
        const auto count = occupancy.fetch_add(1) + 1;
        if (count > maximum.load()) maximum.store(count);
        if (queue.push(i)) return true;
        release(i); return false;
    }
    return false;
}
void NvencFramePool::release(int slot) noexcept
{
    occupancy.fetch_sub(1); owned[static_cast<size_t>(slot)].store(false, std::memory_order_release);
}
CodecPtr NvencEncoder::configuredContext(const NvencProfile& p)
{
    p.validate();
    const auto* encoder = avcodec_find_encoder_by_name("h264_nvenc");
    if (!encoder) throw std::runtime_error("Pinned SDK lacks h264_nvenc");
    CodecPtr c(avcodec_alloc_context3(encoder)); if (!c) throw std::bad_alloc();
    c->width = 1920; c->height = 1080; c->pix_fmt = AV_PIX_FMT_NV12;
    c->time_base = {1, p.fps}; c->framerate = {p.fps, 1};
    c->bit_rate = p.bitRate(); c->rc_max_rate = p.maxRate(); c->rc_buffer_size = static_cast<int>(p.maxRate());
    c->gop_size = p.fps; c->max_b_frames = 0;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_CLOSED_GOP;
    c->colorspace = AVCOL_SPC_BT709; c->color_range = AVCOL_RANGE_MPEG;
    c->color_primaries = AVCOL_PRI_BT709; c->color_trc = AVCOL_TRC_BT709;
    c->sample_aspect_ratio = {1, 1}; c->thread_count = 1;
    const std::pair<const char*, const char*> options[] = {{"preset", "p5"}, {"rc", "vbr"}, {"profile", "high"},
        {"forced-idr", "1"}, {"rc-lookahead", "0"}, {"multipass", "disabled"}, {"surfaces", "4"},
        {"delay", "0"}, {"zerolatency", "1"}, {"no-scenecut", "1"}, {"intra-refresh", "0"}};
    for (const auto& option : options) ffCheck(av_opt_set(c->priv_data, option.first, option.second, 0), option.first);
    return c;
}
NvencEncoder::NvencEncoder(NvencProfile p) : profile(std::move(p)), codec(configuredContext(profile)) {}
void NvencEncoder::open()
{
    if (opened) throw std::logic_error("NVENC already open");
    ffCheck(avcodec_open2(codec.get(), codec->codec, nullptr), "Open h264_nvenc (GPU/driver required)"); opened = true;
    std::int64_t surfaces = 0;
    ffCheck(av_opt_get_int(codec->priv_data, "surfaces", 0, &surfaces), "Read effective NVENC surfaces");
    if (surfaces != NvencProfile::hardwareSurfaces) throw std::runtime_error("NVENC changed the prepared surface budget");
}
void NvencEncoder::receive(const PacketSink& sink, bool flushing)
{
    for (;;)
    {
        const int result = avcodec_receive_packet(codec.get(), packet.get());
        if (result == AVERROR_EOF) return;
        if (result == AVERROR(EAGAIN))
        {
            if (flushing) throw std::runtime_error("NVENC drain requested more input");
            return;
        }
        ffCheck(result, "Receive NVENC packet");
        if (pending.empty() || packet->pts != pending.front() || packet->dts != packet->pts || packet->pts <= lastOutput)
            throw std::runtime_error("NVENC input/output PTS or B-frame-zero DTS contract failed");
        pending.pop_front(); lastOutput = packet->pts;
        if (packet->duration <= 0) packet->duration = 1;
        if (packet->pts % profile.fps == 0 && !(packet->flags & AV_PKT_FLAG_KEY))
            throw std::runtime_error("Forced one-second IDR did not produce a key packet");
        ++packets; if (packet->flags & AV_PKT_FLAG_KEY) ++keyframes;
        sink(*packet); av_packet_unref(packet.get());
    }
}
void NvencEncoder::submit(const AVFrame& frame, std::int64_t pts, const PacketSink& sink)
{
    if (!opened || drained || pts <= lastInput || frame.format != AV_PIX_FMT_NV12 || frame.width != 1920 || frame.height != 1080)
        throw std::invalid_argument("Invalid NVENC submit state/PTS/NV12 dimensions");
    const auto began = qpcNow();
    av_frame_unref(view.get()); ffCheck(av_frame_ref(view.get(), &frame), "Reference encode surface");
    view->pts = pts; view->duration = 1; view->pict_type = pts % profile.fps == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    auto result = avcodec_send_frame(codec.get(), view.get());
    if (result == AVERROR(EAGAIN)) { receive(sink, false); result = avcodec_send_frame(codec.get(), view.get()); }
    ffCheck(result, "Submit CPU NV12 to NVENC");
    av_frame_unref(view.get()); pending.push_back(pts);
    pendingMax = std::max<std::uint64_t>(pendingMax, pending.size());
    if (pending.size() > NvencProfile::hardwareSurfaces) throw std::runtime_error("NVENC pending surface bound exceeded");
    lastInput = pts; ++submitted; receive(sink, false);
    const auto ticks = qpcNow() - began; busyTicks += ticks; maxSubmitTicks = std::max(maxSubmitTicks, ticks);
}
void NvencEncoder::drain(const PacketSink& sink)
{
    if (!opened || drained) throw std::logic_error("Invalid NVENC drain state");
    const auto began = qpcNow();
    ffCheck(avcodec_send_frame(codec.get(), nullptr), "Drain NVENC"); receive(sink, true);
    if (!pending.empty() || submitted != packets) throw std::runtime_error("NVENC lost packets at drain");
    busyTicks += qpcNow() - began; drained = true;
}
juce::var NvencEncoder::toJson() const
{
    auto v = profile.toJson();
    jsonSet(v, "framesSubmitted", jsonInt(submitted)); jsonSet(v, "packets", jsonInt(packets));
    jsonSet(v, "keyframes", jsonInt(keyframes)); jsonSet(v, "pendingHighWater", jsonInt(pendingMax));
    jsonSet(v, "lastPacketPts", juce::var(static_cast<juce::int64>(lastOutput)));
    jsonSet(v, "ptsEqualsDts", packets != 0); jsonSet(v, "drained", drained);
    const double seconds = static_cast<double>(busyTicks) / qpcFrequency();
    jsonSet(v, "serviceSeconds", seconds); jsonSet(v, "serviceFps", seconds > 0 ? juce::var(static_cast<double>(packets) / seconds) : juce::var());
    jsonSet(v, "maxSubmitReceiveMs", 1000.0 * maxSubmitTicks / qpcFrequency());
    jsonSet(v, "serviceFpsDefinition", "Frames / wall time inside submit+receive+drain, including packet queue submission. Real-time idle excluded; not the headroom benchmark.");
    jsonSet(v, "driverVersion", "UNAVAILABLE: FFmpeg CPU-input NVENC context does not expose the chosen CUDA adapter/driver; preview DXGI adapter information is separate");
    return v;
}
}
