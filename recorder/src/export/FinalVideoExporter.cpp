#include "FinalVideoExporter.h"
#include "playback/VideoPlaybackEngine.h"
#include "record/NvencEncoder.h"
#include "record/ReferenceMixWriter.h"
#include "support/Platform.h"
#include <d3d11.h>
#include <dxgi1_2.h>
extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace gocue::recorder
{
struct FinalMp4Writer::State
{
    juce::File path;
    DurableFile file;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVRational bases[2]{};
    std::int64_t position = 0, lastDts[2]{AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    std::uint64_t packets[2]{};
    bool owned = false, finished = false;
    juce::String ioError;
    State(juce::File p, FileIoFaultAdapter* f) : path(std::move(p)), file(f) {}
    ~State()
    {
        if (format) { format->pb = nullptr; avformat_free_context(format); }
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
        file.close(); if (owned && !finished) path.deleteFile();
    }
    static int write(void* opaque, const std::uint8_t* bytes, int count)
    {
        auto& s = *static_cast<State*>(opaque);
        try
        {
            exportRequire(count >= 0 && s.position >= 0 && std::uint64_t(s.position) <= s.file.writtenBytes(), "Invalid MP4 AVIO write position");
            const auto patch = (std::min)(std::uint64_t(count), s.file.writtenBytes() - std::uint64_t(s.position));
            if (patch) exportCheck(s.file.writeAt(std::uint64_t(s.position), bytes, static_cast<std::size_t>(patch)));
            if (patch < std::uint64_t(count)) exportCheck(s.file.write(bytes + patch, static_cast<std::size_t>(std::uint64_t(count) - patch)));
            s.position += count; return count;
        }
        catch (const std::exception& e) { s.ioError = e.what(); return AVERROR(EIO); }
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int origin)
    {
        auto& s = *static_cast<State*>(opaque); const auto size = static_cast<std::int64_t>(s.file.writtenBytes());
        if (origin & AVSEEK_SIZE) return size;
        origin &= ~AVSEEK_FORCE;
        if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) return AVERROR(EINVAL);
        const auto base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? s.position : size;
        if (offset < -base || offset > size - base) return AVERROR(EINVAL);
        s.position = base + offset; return s.position;
    }
    void checkIo() { exportCheck(file.status()); if (io) ffCheck(io->error, "Export MP4 AVIO"); }
    void append(const AVPacket& p, int stream)
    {
        exportRequire(!finished && p.pts != AV_NOPTS_VALUE && p.dts != AV_NOPTS_VALUE && p.duration > 0
            && (lastDts[stream] == AV_NOPTS_VALUE || p.dts > lastDts[stream]), "Invalid export MP4 packet/DTS");
        auto packet = ffPacket(); ffCheck(av_packet_ref(packet.get(), &p), "Reference final MP4 packet");
        av_packet_rescale_ts(packet.get(), bases[stream], format->streams[stream]->time_base); packet->stream_index = stream; packet->pos = -1;
        const auto result = av_interleaved_write_frame(format, packet.get()); checkIo(); ffCheck(result, "Mux final MP4");
        lastDts[stream] = p.dts; ++packets[stream];
    }
};
FinalMp4Writer::FinalMp4Writer(juce::File partial, const AVCodecContext& video, const AVCodecContext& audio, FileIoFaultAdapter* faults)
    : state(std::make_unique<State>(std::move(partial), faults))
{
    auto& s = *state;
    exportRequire(s.path.getFileName().endsWith(".partial") && !s.path.exists() && video.codec_type == AVMEDIA_TYPE_VIDEO
        && audio.codec_id == AV_CODEC_ID_AAC && audio.sample_rate == 48000 && audio.ch_layout.nb_channels == 2, "Final MP4 requires new .partial, video and stereo 48k AAC");
    exportCheck(s.path.getParentDirectory().createDirectory()); exportCheck(s.file.open(s.path, DurableFile::OpenMode::createNew)); s.owned = true;
    ffCheck(avformat_alloc_output_context2(&s.format, nullptr, "mp4", nullptr), "Allocate ordinary export MP4");
    for (const auto* c : {&video, &audio})
    {
        auto* stream = avformat_new_stream(s.format, nullptr); if (!stream) throw std::bad_alloc();
        stream->id = stream->index + 1; stream->time_base = c->time_base; s.bases[stream->index] = c->time_base;
        ffCheck(avcodec_parameters_from_context(stream->codecpar, c), "Copy final codec parameters");
        if (c == &video) stream->avg_frame_rate = c->framerate;
    }
    s.format->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED; s.format->max_interleave_delta = 250000;
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(65536)); if (!buffer) throw std::bad_alloc();
    s.io = avio_alloc_context(buffer, 65536, 1, &s, nullptr, State::write, State::seek);
    if (!s.io) { av_free(buffer); throw std::bad_alloc(); }
    s.io->seekable = AVIO_SEEKABLE_NORMAL; s.format->pb = s.io; s.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    Dictionary options; options.set("use_editlist", "1"); options.set("movie_timescale", "48000");
    const auto result = avformat_write_header(s.format, &options.value); s.checkIo(); ffCheck(result, "Write ordinary MP4 header");
    exportRequire(!av_dict_count(options.value), "Unconsumed final MP4 options");
}
FinalMp4Writer::~FinalMp4Writer() = default;
void FinalMp4Writer::video(const AVPacket& p) { state->append(p, 0); }
void FinalMp4Writer::audio(const AVPacket& p) { state->append(p, 1); }
void FinalMp4Writer::finish()
{
    auto& s = *state; exportRequire(!s.finished && s.packets[0] && s.packets[1], "Cannot finish empty/finished final MP4");
    const auto result = av_write_trailer(s.format); avio_flush(s.io); s.checkIo(); ffCheck(result, "Finish ordinary MP4");
    exportCheck(s.file.flushData()); exportCheck(s.file.close()); s.finished = true;
}

namespace
{
struct Input { AVFormatContext* p = nullptr; ~Input() { avformat_close_input(&p); } };
struct Hardware { AVBufferRef* p = nullptr; ~Hardware() { av_buffer_unref(&p); } };
// Round-11 index, containment and IDR/sequential-decode plan; an export-only
// NV12 endpoint because IVideoFrameDecoder exposes opaque display BGRA textures.
class ExportDecoder
{
public:
    explicit ExportDecoder(std::shared_ptr<const VideoIndex> index) : source(std::move(index))
    {
        exportRequire(source && source->current(), "Stale/missing export video index");
        ffCheck(avformat_open_input(&input.p, source->file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Open export video source");
        ffCheck(avformat_find_stream_info(input.p, nullptr), "Read export video configuration");
        const auto* decoder = avcodec_find_decoder(AV_CODEC_ID_H264); exportRequire(decoder != nullptr, "H.264 decoder unavailable");
        hardware.p = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA); if (!hardware.p) throw std::bad_alloc();
        auto* device = reinterpret_cast<AVHWDeviceContext*>(hardware.p->data);
        auto* gpu = static_cast<AVD3D11VADeviceContext*>(device->hwctx);
        ComPtr<IDXGIFactory1> factory; checkHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create export DXGI factory");
        ComPtr<IDXGIAdapter1> selected;
        for (UINT n = 0;; ++n)
        {
            ComPtr<IDXGIAdapter1> adapter; const auto hr = factory->EnumAdapters1(n, &adapter); if (hr == DXGI_ERROR_NOT_FOUND) break;
            checkHr(hr, "Enumerate export adapter"); DXGI_ADAPTER_DESC1 desc{}; checkHr(adapter->GetDesc1(&desc), "Read export adapter");
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { selected = adapter; break; }
        }
        exportRequire(selected != nullptr, "NVIDIA D3D11VA adapter unavailable; no software fallback");
        D3D_FEATURE_LEVEL level{}; const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        checkHr(D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &gpu->device, &level, &gpu->device_context), "Create export D3D11VA device");
        ffCheck(av_hwdevice_ctx_init(hardware.p), "Initialise export D3D11VA");
        codec.reset(avcodec_alloc_context3(decoder)); if (!codec) throw std::bad_alloc();
        ffCheck(avcodec_parameters_to_context(codec.get(), input.p->streams[source->stream]->codecpar), "Copy export H.264 configuration");
        exportRequire(codec->width == 1920 && codec->height == 1080 && codec->color_range == AVCOL_RANGE_MPEG
            && codec->colorspace == AVCOL_SPC_BT709 && codec->color_primaries == AVCOL_PRI_BT709 && codec->color_trc == AVCOL_TRC_BT709,
            "Export camera source must be Recorder 1080p BT.709 limited SDR");
        codec->hw_device_ctx = av_buffer_ref(hardware.p); if (!codec->hw_device_ctx) throw std::bad_alloc();
        codec->get_format = [](AVCodecContext*, const AVPixelFormat* f)
        { for (; *f != AV_PIX_FMT_NONE; ++f) if (*f == AV_PIX_FMT_D3D11) return *f; return AV_PIX_FMT_NONE; };
        codec->thread_count = 1; codec->err_recognition = AV_EF_EXPLODE;
        ffCheck(avcodec_open2(codec.get(), decoder, nullptr), "Open D3D11VA export decoder");
    }
    const AVFrame& frame(std::size_t target, const ExportControl& control)
    {
        control.checkpoint(); exportRequire(source->current(), "Export video generation changed");
        if (last && *last == target) return *nv12;
        const auto plan = playbackDecodePlan(*source, target, last);
        if (plan.fromIdr)
        {
            ffCheck(av_seek_frame(input.p, source->stream, source->packets.at(plan.firstPacket).pts, AVSEEK_FLAG_BACKWARD), "Seek export IDR");
            avcodec_flush_buffers(codec.get()); draining = false;
        }
        for (;;)
        {
            control.checkpoint(); exportRequire(source->current(), "Export video generation changed during decode");
            const int received = avcodec_receive_frame(codec.get(), decoded.get());
            if (received == 0)
            {
                exportRequire(decoded->format == AV_PIX_FMT_D3D11 && !decoded->decode_error_flags, "D3D11VA export decode error/fallback");
                const auto pts = decoded->best_effort_timestamp;
                exportRequire(pts != AV_NOPTS_VALUE && pts <= source->packets.at(target).pts, "Export decoder missed indexed frame");
                if (plan.convert(pts, *source))
                {
                    av_frame_unref(nv12.get()); nv12->format = AV_PIX_FMT_NV12;
                    ffCheck(av_hwframe_transfer_data(nv12.get(), decoded.get(), 0), "Transfer D3D11VA NV12 for NVENC");
                    exportRequire(nv12->format == AV_PIX_FMT_NV12 && nv12->width == 1920 && nv12->height == 1080, "Unexpected export NV12 layout");
                    nv12->colorspace = AVCOL_SPC_BT709; nv12->color_range = AVCOL_RANGE_MPEG;
                    nv12->color_primaries = AVCOL_PRI_BT709; nv12->color_trc = AVCOL_TRC_BT709;
                    last = target; av_frame_unref(decoded.get()); return *nv12;
                }
                av_frame_unref(decoded.get()); continue;
            }
            if (received != AVERROR(EAGAIN)) { ffCheck(received, "Receive export source frame"); throw std::runtime_error("Source ended before requested export frame"); }
            exportRequire(!draining, "Export decoder drain produced no frame");
            for (;;)
            {
                control.checkpoint(); const auto read = av_read_frame(input.p, packet.get());
                if (read == AVERROR_EOF) { ffCheck(avcodec_send_packet(codec.get(), nullptr), "Drain export source"); draining = true; break; }
                ffCheck(read, "Read export source packet");
                if (packet->stream_index == source->stream)
                { const auto sent = avcodec_send_packet(codec.get(), packet.get()); av_packet_unref(packet.get()); ffCheck(sent, "Decode export source packet"); break; }
                av_packet_unref(packet.get()); // reference AAC never enters export PCM
            }
        }
    }
private:
    std::shared_ptr<const VideoIndex> source;
    Input input; Hardware hardware; CodecPtr codec;
    FramePtr decoded = ffFrame(), nv12 = ffFrame(); PacketPtr packet = ffPacket();
    std::optional<std::size_t> last; bool draining = false;
};
bool camera(TrackKind kind) { return kind == TrackKind::cam1 || kind == TrackKind::cam2; }
}
AudioSourceMask FinalVideoExporter::audioSource(const ExportJob& job, const juce::String& choice)
{
    using K = AudioSourceMask::Kind;
    if (choice == "mix") return {K::microphoneMix};
    if (choice.startsWith("mic:"))
    {
        const auto value = choice.substring(4); exportRequire(value.length() == 1 && value[0] >= '1' && value[0] <= '8', "Microphone selection must be mic:1..8");
        for (const auto& t : job.snapshot.tracks) if (t.kind == TrackKind::mic && t.microphoneIndex == value.getIntValue() - 1) return {K::microphone, t.trackId};
        throw std::runtime_error("Selected microphone lane does not exist");
    }
    if (choice.startsWith("import:"))
    {
        const auto id = choice.substring(7); AudioSourceMask result{K::completedAudio, {}, id};
        for (const auto& t : job.snapshot.tracks) if (t.kind == TrackKind::importAudio)
            for (const auto& c : job.plan().activeClips) if (c.trackId == t.trackId && c.assetId == id)
            {
                exportRequire(result.trackId.isEmpty() || result.trackId == t.trackId, "Imported asset is on multiple lanes; choose an explicit track mask"); result.trackId = t.trackId;
            }
        exportRequire(result.trackId.isNotEmpty(), "Selected import has no active edited clips"); return result;
    }
    throw std::runtime_error("Audio selection must be mix, mic:2 or import:<asset ID>");
}
void FinalVideoExporter::validateSelection(const ExportJob& j, const FinalExportSelection& selection)
{
    using K = AudioSourceMask::Kind;
    exportRequire(camera(selection.video), "Choose exactly one camera lane");
    exportRequire(std::any_of(j.plan().tracks.begin(), j.plan().tracks.end(), [&](const auto& t) { return t.kind == selection.video; }), "Selected camera lane does not exist");
    const auto& m = selection.audio;
    exportRequire(m.kind == K::microphoneMix || m.kind == K::microphone || m.kind == K::completedAudio, "Final export requires one of the three audio sources");
    if (m.kind == K::microphoneMix) exportRequire(m.trackId.isEmpty() && m.assetId.isEmpty(), "Mix does not accept a second source selector");
    else
    {
        const auto it = std::find_if(j.plan().tracks.begin(), j.plan().tracks.end(), [&](const auto& t) { return t.trackId == m.trackId; });
        exportRequire(it != j.plan().tracks.end() && it->kind == (m.kind == K::microphone ? TrackKind::mic : TrackKind::importAudio), "Selected audio lane has the wrong kind/is missing");
        if (m.kind == K::microphone) exportRequire(m.assetId.isEmpty(), "Microphone selection must identify a track");
        else
        {
            const auto* a = j.snapshot.media->findAsset(m.assetId);
            exportRequire(a && a->kind == AssetKind::importAudio && a->originalFormat.channels >= 1 && a->originalFormat.channels <= 2,
                "Completed audio supports mono/stereo only; selected asset missing or has 3+ channels");
            exportRequire(std::any_of(j.plan().activeClips.begin(), j.plan().activeClips.end(), [&](const auto& c) { return c.trackId == m.trackId && c.assetId == m.assetId; }), "Completed audio requires active timeline clips");
        }
    }
    exportRequire(j.snapshot.Fs >= 8000 && (j.snapshot.fps == FrameRate{30, 1} || j.snapshot.fps == FrameRate{60, 1}), "Final encoder requires project 30/60 fps and Fs >= 8k");
}
ExportVideoMapping FinalVideoExporter::mappingAt(const ExportJob& job, TrackKind kind, Sample n)
{
    exportRequire(camera(kind) && n >= 0 && n < job.range.frameCount, "Invalid export camera/frame");
    ExportVideoMapping m; m.outputFrame = n; m.timelineSample = frameToSample(job.range.firstFrame + n, job.snapshot.Fs, job.snapshot.fps);
    for (const auto& t : job.plan().tracks) if (t.kind == kind) for (const auto& s : t.spans)
        if (!s.isGap() && m.timelineSample >= s.timeline.start && m.timelineSample - s.timeline.start < s.timeline.length)
    {
        m.assetId = s.assetId; m.clipId = s.clipId; m.sourceSample = s.sourceIn + (m.timelineSample - s.timeline.start);
        m.sourceFrame = RenderPlanCompiler::sourceUnitAt(s, m.timelineSample, true); return m;
    }
    return m;
}

juce::var FinalVideoExporter::verify(const juce::File& file, Sample frames, Sample projectSamples, std::uint32_t Fs,
                                    FrameRate fps, ExportControl& control, const ExportVerificationObserver& observer, AVCodecID expectedVideo)
{
    control.checkpoint(); const auto validAudio = rescaleRound(projectSamples, 48000, Fs);
    exportRequire(frames > 0 && validAudio > 0, "Invalid final verification duration");
    Input input; ffCheck(avformat_open_input(&input.p, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Reopen final export before publish");
    ffCheck(avformat_find_stream_info(input.p, nullptr), "Read final export streams"); exportRequire(input.p->nb_streams == 2, "Final MP4 must have exactly two streams");
    const int vi = av_find_best_stream(input.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0), ai = av_find_best_stream(input.p, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    exportRequire(vi >= 0 && ai >= 0 && vi != ai, "Final MP4 requires video1+audio1");
    const auto* vs = input.p->streams[vi]; const auto* as = input.p->streams[ai];
    exportRequire(vs->codecpar->codec_id == expectedVideo && as->codecpar->codec_id == AV_CODEC_ID_AAC
        && as->codecpar->profile == AV_PROFILE_AAC_LOW && as->codecpar->sample_rate == 48000 && as->codecpar->ch_layout.nb_channels == 2,
        "Final stream codec/audio format mismatch");
    exportRequire(vs->start_time == 0 && as->start_time == 0 && av_compare_ts(vs->duration, vs->time_base, frames, {int(fps.denominator), int(fps.numerator)}) == 0
        && av_compare_ts(as->duration, as->time_base, validAudio, {1, 48000}) == 0, "Final presentation origin/duration mismatch");
    if (expectedVideo == AV_CODEC_ID_H264)
        exportRequire(vs->codecpar->width == 1920 && vs->codecpar->height == 1080 && vs->codecpar->color_range == AVCOL_RANGE_MPEG
            && vs->codecpar->color_space == AVCOL_SPC_BT709 && vs->codecpar->color_primaries == AVCOL_PRI_BT709 && vs->codecpar->color_trc == AVCOL_TRC_BT709,
            "Final video is not 1080p BT.709 limited SDR");
    CodecPtr codecs[2];
    for (int i = 0; i < 2; ++i)
    {
        const auto* s = i ? as : vs; const auto* c = avcodec_find_decoder(s->codecpar->codec_id); exportRequire(c != nullptr, "Verification decoder unavailable");
        codecs[i].reset(avcodec_alloc_context3(c)); if (!codecs[i]) throw std::bad_alloc();
        ffCheck(avcodec_parameters_to_context(codecs[i].get(), s->codecpar), "Prepare final verification decoder");
        codecs[i]->thread_count = 1; codecs[i]->err_recognition = AV_EF_EXPLODE; codecs[i]->pkt_timebase = s->time_base;
        ffCheck(avcodec_open2(codecs[i].get(), c, nullptr), "Open final full-decode validator");
    }
    Sample videoFrames = 0, audioCursor = 0, decodedAudio = 0, videoPackets = 0, audioPackets = 0;
    Sample firstAudioPts = AV_NOPTS_VALUE, lastAudioEnd = AV_NOPTS_VALUE, priming = 0;
    auto packet = ffPacket(); auto decoded = ffFrame();
    const auto receive = [&](int which, bool draining)
    {
        for (;;)
        {
            control.checkpoint(); const auto code = avcodec_receive_frame(codecs[which].get(), decoded.get());
            if (code == AVERROR_EOF) return;
            if (code == AVERROR(EAGAIN)) { exportRequire(!draining, "Verification decoder did not reach EOF"); return; }
            ffCheck(code, "Full decode final output"); exportRequire(!decoded->decode_error_flags, "Corrupt decoded final frame");
            const auto pts = decoded->best_effort_timestamp; exportRequire(pts != AV_NOPTS_VALUE, "Final decoded frame has no PTS");
            if (!which)
            {
                exportRequire(videoFrames < frames && av_compare_ts(pts, vs->time_base, videoFrames, {int(fps.denominator), int(fps.numerator)}) == 0
                    && decoded->pict_type != AV_PICTURE_TYPE_B, "Final video frame grid/B-frame mismatch");
                if (observer.video) observer.video(videoFrames, *decoded); ++videoFrames;
            }
            else
            {
                exportRequire(decoded->format == AV_SAMPLE_FMT_FLTP && decoded->ch_layout.nb_channels == 2, "Unexpected decoded AAC PCM format");
                const auto first = av_rescale_q(pts, as->time_base, {1, 48000}), end = first + decoded->nb_samples;
                const auto begin = (std::max)(Sample{0}, first), finish = (std::min)(validAudio, end); decodedAudio += decoded->nb_samples;
                if (begin < finish)
                {
                    exportRequire(begin == audioCursor, "AAC presentation gap/overlap");
                    if (observer.audio) observer.audio(begin, static_cast<unsigned>(finish - begin),
                        reinterpret_cast<const float*>(decoded->data[0]) + begin - first, reinterpret_cast<const float*>(decoded->data[1]) + begin - first);
                    audioCursor = finish;
                }
            }
            av_frame_unref(decoded.get());
        }
    };
    for (;;)
    {
        control.checkpoint(); const auto read = av_read_frame(input.p, packet.get()); if (read == AVERROR_EOF) break; ffCheck(read, "Read final validation packet");
        const int which = packet->stream_index == vi ? 0 : 1;
        exportRequire(packet->pts != AV_NOPTS_VALUE && packet->pts == packet->dts && packet->duration > 0, "Invalid final packet PTS/DTS/duration");
        if (!which)
        {
            exportRequire(av_compare_ts(packet->pts, vs->time_base, videoPackets, {int(fps.denominator), int(fps.numerator)}) == 0, "Final video packet grid mismatch"); ++videoPackets;
        }
        else
        {
            const auto first = av_rescale_q(packet->pts, as->time_base, {1, 48000});
            if (!audioPackets)
            {
                firstAudioPts = first; size_t bytes = 0; const auto* skip = av_packet_get_side_data(packet.get(), AV_PKT_DATA_SKIP_SAMPLES, &bytes);
                if (skip && bytes >= 10) for (unsigned b = 0; b < 4; ++b) priming |= Sample(skip[b]) << (8 * b);
            }
            else exportRequire(first == lastAudioEnd, "AAC packet gap/overlap");
            lastAudioEnd = first + av_rescale_q(packet->duration, as->time_base, {1, 48000}); ++audioPackets;
        }
        auto sent = avcodec_send_packet(codecs[which].get(), packet.get());
        if (sent == AVERROR(EAGAIN)) { receive(which, false); sent = avcodec_send_packet(codecs[which].get(), packet.get()); }
        ffCheck(sent, "Submit final verification packet"); receive(which, false); av_packet_unref(packet.get());
    }
    for (int i = 0; i < 2; ++i) { ffCheck(avcodec_send_packet(codecs[i].get(), nullptr), "Drain final verification"); receive(i, true); }
    exportRequire(videoFrames == frames && videoPackets == frames && audioCursor == validAudio && lastAudioEnd == validAudio
        && firstAudioPts < 0 && priming == -firstAudioPts && decodedAudio >= validAudio && decodedAudio - validAudio < 1024, "Final full-decode count/priming/AAC tail mismatch");
    if (observer.finish) observer.finish();
    control.checkpoint();
    auto result = jsonObject(); jsonSet(result, "verified", true); jsonSet(result, "frameCount", videoFrames); jsonSet(result, "sampleCount", validAudio);
    jsonSet(result, "Fs", 48000); jsonSet(result, "renderedPcmSampleCount", projectSamples); jsonSet(result, "renderedPcmFs", Fs);
    jsonSet(result, "aacPresentationSamples", validAudio); jsonSet(result, "aacDecodedSamples", decodedAudio); jsonSet(result, "aacPhysicalPacketSamples", audioPackets * 1024);
    jsonSet(result, "aacInitialPadding", priming); jsonSet(result, "aacDecodedTailPadding", decodedAudio - validAudio); jsonSet(result, "aacLastPacketEnd", lastAudioEnd);
    jsonSet(result, "presentationStartsAtZero", true); jsonSet(result, "fullDecodeReachedEof", true); return result;
}

juce::var FinalVideoExporter::run(const ExportJob& j, const FinalExportSelection& selection, ExportControl& control,
                                 const ExportVerificationObserver& observer, FileIoFaultAdapter* faults)
{
    ExportActivity::Lease lease(control.activity); control.checkpoint(); validateSelection(j, selection);
    const auto began = std::chrono::steady_clock::now();
    const auto progress = [&](const char* stage, double fraction)
    {
        control.checkpoint(); if (!control.onProgress) return;
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        control.onProgress({stage, fraction, seconds, fraction > 0 ? std::optional<double>(seconds * (1 - fraction) / fraction) : std::nullopt});
    };
    auto bindings = TimelineExporter::openSources(j, selection.audio, control);
    ExportAudioRenderer pcm(j, std::move(bindings), selection.audio);
    MediaIndex media; std::map<Id, std::shared_ptr<const VideoIndex>> indexes;
    for (const auto& t : j.plan().tracks) if (t.kind == selection.video) for (const auto& s : t.spans)
        if (!s.isGap() && s.timeline.start < j.range.startSample + j.range.sampleCount && s.timeline.start + s.timeline.length > j.range.startSample && !indexes.count(s.assetId))
    {
        control.checkpoint(); const auto* a = j.snapshot.media->findAsset(s.assetId);
        exportRequire(a && a->kind == AssetKind::camera && a->chunks.empty() && isProjectRelativePath(a->relativePath), "Camera export requires a finalized MP4 asset");
        auto index = media.openVideo(j.projectDirectory.getChildFile(a->relativePath), j.snapshot.Fs);
        exportRequire(index->width == 1920 && index->height == 1080, "Camera export requires 1080p source"); indexes.emplace(s.assetId, std::move(index));
    }
    for (const auto& t : j.plan().tracks) if (t.kind == selection.video) for (const auto& s : t.spans)
        if (!s.isGap() && s.timeline.start < j.range.startSample + j.range.sampleCount && s.timeline.start + s.timeline.length > j.range.startSample)
    {
        const auto begin = (std::max)(s.timeline.start, j.range.startSample), end = (std::min)(s.timeline.start + s.timeline.length, j.range.startSample + j.range.sampleCount);
        const auto sourceEnd = s.sourceIn + (end - s.timeline.start);
        exportRequire(sourceEnd <= indexes.at(s.assetId)->length && s.sourceIn + (begin - s.timeline.start) >= 0, "Selected video edit exceeds decoded source length");
    }
    progress("prepare", .03); control.checkpoint();
    // Decoder and encoder errors propagate; no stream-copy or software encode fallback.
    NvencEncoder encoder({int(j.snapshot.fps.numerator), "p5"}); encoder.open(); ReferenceMixWriter aac(j.snapshot.Fs);
    ExportPublication output(j); const auto finalFile = output.file("final.mp4"), partial = finalFile.getSiblingFile("final.mp4.partial");
    auto black = ffFrame(); black->format = AV_PIX_FMT_NV12; black->width = 1920; black->height = 1080;
    black->colorspace = AVCOL_SPC_BT709; black->color_range = AVCOL_RANGE_MPEG; black->color_primaries = AVCOL_PRI_BT709; black->color_trc = AVCOL_TRC_BT709;
    ffCheck(av_frame_get_buffer(black.get(), 32), "Allocate export black NV12");
    for (int p = 0; p < 2; ++p) for (int y = 0; y < (p ? 540 : 1080); ++y) std::memset(black->data[p] + std::size_t(y) * black->linesize[p], p ? 128 : 16, 1920);
    Sample pcmCursor = 0, blackFrames = 0; std::set<Id> videoAssets;
    std::vector<SampleRange> blackRuns;
    std::vector<float> left(16384), right(left.size()), stereo(left.size() * 2);
    {
        FinalMp4Writer mux(partial, encoder.context(), aac.context(), faults);
        const PacketSink videoSink = [&](const AVPacket& p) { control.checkpoint(); mux.video(p); };
        const PacketSink audioSink = [&](const AVPacket& p) { control.checkpoint(); mux.audio(p); };
        Id decoderAsset; std::unique_ptr<ExportDecoder> decoder;
        for (Sample n = 0; n < j.range.frameCount; ++n)
        {
            control.checkpoint(); const auto mapping = mappingAt(j, selection.video, n); const AVFrame* picture = black.get();
            if (mapping.black())
            {
                ++blackFrames;
                if (!blackRuns.empty() && blackRuns.back().start + blackRuns.back().length == n) ++blackRuns.back().length;
                else blackRuns.push_back({n, 1});
            }
            else
            {
                const auto& index = indexes.at(mapping.assetId);
                if (!decoder || decoderAsset != mapping.assetId) { decoder = std::make_unique<ExportDecoder>(index); decoderAsset = mapping.assetId; }
                picture = &decoder->frame(index->frameAt(mapping.sourceSample), control); videoAssets.insert(mapping.assetId);
            }
            encoder.submit(*picture, n, videoSink);
            const auto audioEnd = (std::min)(j.range.sampleCount, frameToSample(n + 1, j.snapshot.Fs, j.snapshot.fps));
            while (pcmCursor < audioEnd)
            {
                control.checkpoint(); const auto count = static_cast<unsigned>((std::min)(Sample(left.size()), audioEnd - pcmCursor));
                pcm.render(pcmCursor, count, left.data(), right.data());
                for (unsigned i = 0; i < count; ++i) { stereo[std::size_t(i) * 2] = left[i]; stereo[std::size_t(i) * 2 + 1] = right[i]; }
                aac.append(stereo.data(), count, audioSink); pcmCursor += count;
            }
            if (n % 30 == 0) progress("render", .03 + .77 * double(n + 1) / j.range.frameCount);
        }
        exportRequire(pcmCursor == j.range.sampleCount, "Continuous export PCM was not rendered exactly once");
        encoder.drain(videoSink); aac.finishInput(audioSink); mux.finish();
    }
    progress("verify", .81);
    auto observing = observer;
    observing.video = [&](Sample n, const AVFrame& frame)
    {
        if (n % 30 == 0) progress("verify", .81 + .16 * double(n + 1) / j.range.frameCount);
        if (observer.video) observer.video(n, frame);
    };
    auto verification = verify(partial, j.range.frameCount, j.range.sampleCount, j.snapshot.Fs, j.snapshot.fps, control, observing);
    progress("publish", .98);
    auto f = verification; jsonSet(f, "name", "final.mp4"); jsonSet(f, "videoSource", selection.video == TrackKind::cam1 ? "cam1" : "cam2");
    jsonSet(f, "videoCodec", "h264_nvenc"); jsonSet(f, "decodePath", "d3d11va -> CPU NV12 -> h264_nvenc; round-11 MediaIndex and playbackDecodePlan");
    jsonSet(f, "streamCopy", false); jsonSet(f, "blackFrames", blackFrames); jsonSet(f, "audioAssetIds", TimelineExporter::assetIds(j, selection.audio));
    juce::Array<juce::var> videoGaps;
    for (const auto& run : blackRuns) { auto gap = jsonObject(); jsonSet(gap, "firstFrame", run.start); jsonSet(gap, "frameCount", run.length); videoGaps.add(gap); }
    jsonSet(f, "videoGaps", videoGaps);
    jsonSet(f, "audioSourceKind", selection.audio.kind == AudioSourceMask::Kind::microphoneMix ? "microphoneMix" : selection.audio.kind == AudioSourceMask::Kind::microphone ? "microphone" : "completedAudio");
    jsonSet(f, "audioTrackId", selection.audio.trackId); jsonSet(f, "audioAssetId", selection.audio.assetId);
    juce::Array<juce::var> ids; for (const auto& id : videoAssets) ids.add(id); jsonSet(f, "videoAssetIds", ids);
    auto aacInfo = aac.toJson(); jsonSet(aacInfo, "source", "One continuous selected timeline PCM render; independent audio clip edits retained"); jsonSet(f, "aac", aacInfo);
    juce::Array<juce::var> files; files.add(f); output.commit(files, control, faults);
    auto result = j.manifest(files); const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    jsonSet(result, "elapsedSeconds", seconds); jsonSet(result, "effectiveFps", double(j.range.frameCount) / seconds);
    jsonSet(result, "speedMeasurement", "Preparation + decode/render/encode + full validation + durable publish; initial 1cam60 target >=120fps, not a guarantee");
    // Publication succeeded: a late cancellation/progress callback cannot turn the
    // completed output into a cancelled job or cause its removal.
    if (control.onProgress) try { control.onProgress({"complete", 1, seconds, 0.0}); } catch (...) {}
    return result;
}
}
