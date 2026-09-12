#include "record/NvencEncoder.h"
#include "record/Mp4TakeWriter.h"
#include "record/ReferenceMixWriter.h"
#include "TestSupport.h"
#include <algorithm>
#include <cstring>
#include <vector>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
std::int64_t option(const AVCodecContext& c, const char* name)
{
    std::int64_t value = 0; ffCheck(av_opt_get_int(c.priv_data, name, 0, &value), name); return value;
}
CodecPtr fixtureEncoder()
{
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4); require(codec != nullptr, "Software MPEG4 fixture encoder");
    CodecPtr c(avcodec_alloc_context3(codec)); require(bool(c), "Fixture codec context");
    c->width = c->height = 16; c->pix_fmt = AV_PIX_FMT_YUV420P; c->time_base = {1,30}; c->framerate = {30,1};
    c->gop_size = 15; c->max_b_frames = 0; c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; c->thread_count = 1;
    c->colorspace = AVCOL_SPC_BT709; c->color_range = AVCOL_RANGE_MPEG; c->color_primaries = AVCOL_PRI_BT709; c->color_trc = AVCOL_TRC_BT709;
    ffCheck(avcodec_open2(c.get(), codec, nullptr), "Open software video fixture"); return c;
}
FramePtr fixtureFrame()
{
    auto f = ffFrame(); f->width = f->height = 16; f->format = AV_PIX_FMT_YUV420P;
    ffCheck(av_frame_get_buffer(f.get(), 32), "Allocate software video fixture");
    for (int p = 0; p < 3; ++p) for (int y = 0; y < (p ? 8 : 16); ++y) std::memset(f->data[p] + y * f->linesize[p], p ? 128 : 64, p ? 8 : 16);
    return f;
}
void verifyDecode(const juce::File& file, std::uint64_t expectedVideo, std::int64_t audioSamples)
{
    struct Input { AVFormatContext* p = nullptr; ~Input(){ avformat_close_input(&p); } } input;
    ffCheck(avformat_open_input(&input.p, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Open test MP4 decode");
    ffCheck(avformat_find_stream_info(input.p, nullptr), "Read test MP4 streams");
    require(input.p->nb_streams == 2 && input.p->streams[1]->duration == audioSamples, "AAC edit-list presentation length exact, padding excluded");
    std::array<CodecPtr,2> codecs;
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto* decoder = avcodec_find_decoder(input.p->streams[i]->codecpar->codec_id);
        codecs[i].reset(avcodec_alloc_context3(decoder)); require(bool(codecs[i]), "Decoder context");
        ffCheck(avcodec_parameters_to_context(codecs[i].get(), input.p->streams[i]->codecpar), "Fixture decode parameters");
        codecs[i]->pkt_timebase = input.p->streams[i]->time_base; codecs[i]->thread_count = 1;
        ffCheck(avcodec_open2(codecs[i].get(), decoder, nullptr), "Open software fixture decoder");
    }
    auto packet = ffPacket(); auto frame = ffFrame();
    std::uint64_t video = 0; std::int64_t decodedAudio = 0, firstAudio = AV_NOPTS_VALUE;
    const auto receive = [&](int stream)
    {
        for (;;)
        {
            const int result = avcodec_receive_frame(codecs[static_cast<size_t>(stream)].get(), frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            ffCheck(result, "Full software fixture decode");
            if (!stream) ++video;
            else { if (firstAudio == AV_NOPTS_VALUE) firstAudio = frame->pts; decodedAudio += frame->nb_samples; }
            av_frame_unref(frame.get());
        }
    };
    std::int64_t lastDts[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    for (;;)
    {
        const int result = av_read_frame(input.p, packet.get()); if (result == AVERROR_EOF) break;
        ffCheck(result, "Read all fixture packets"); const auto stream = packet->stream_index;
        require(stream >= 0 && stream < 2 && packet->pts == packet->dts && (lastDts[stream] == AV_NOPTS_VALUE || packet->dts > lastDts[stream]), "Mux PTS=DTS, strictly increasing per stream");
        lastDts[stream] = packet->dts;
        ffCheck(avcodec_send_packet(codecs[static_cast<size_t>(stream)].get(), packet.get()), "Send fixture decoder packet"); receive(stream); av_packet_unref(packet.get());
    }
    for (int stream = 0; stream < 2; ++stream) { ffCheck(avcodec_send_packet(codecs[static_cast<size_t>(stream)].get(), nullptr), "Drain fixture decoder"); receive(stream); }
    require(video == expectedVideo && firstAudio == 0, "Every video frame decodes; AAC first presented PCM sample at zero");
    require(decodedAudio >= audioSamples && decodedAudio - audioSamples < 1024, "Only codec tail padding can remain in raw decoder output; edit-list length trims presentation");
}
}
int runEncoderContractTests()
{
    Suite s;
    s.test("NVENC unopened contexts carry pinned 30/60 options and safe lifetimes", []
    {
        for (const auto fps : {30,60}) for (int iteration = 0; iteration < 8; ++iteration)
        {
            NvencProfile profile; profile.fps = fps; const auto c = NvencEncoder::configuredContext(profile);
            const auto json = profile.toJson();
            require((json["bFrames"].isInt() || json["bFrames"].isInt64()) && static_cast<int>(json["bFrames"]) == 0
                && json["forced-idr"].isBool(), "JSON zero options are numbers and boolean options remain booleans");
            require(c->codec_id == AV_CODEC_ID_H264 && c->pix_fmt == AV_PIX_FMT_NV12 && c->width == 1920 && c->height == 1080, "1080p CPU NV12 context");
            require(c->time_base.num == 1 && c->time_base.den == fps && c->gop_size == fps && !c->max_b_frames, "CFR time base and one-second GOP, no B frames");
            require(c->bit_rate == profile.bitRate() && c->rc_max_rate == profile.maxRate(), "Bitrate profile");
            require(option(*c,"preset") == 16 && option(*c,"rc") == 1 && option(*c,"forced-idr") == 1 && !option(*c,"rc-lookahead") && !option(*c,"multipass"), "Pinned P5/VBR/IDR/lookahead/multipass options");
            require(option(*c,"surfaces") == 4 && !option(*c,"delay") && (c->flags & AV_CODEC_FLAG_CLOSED_GOP), "Bounded hardware surface count and closed GOP");
            require(c->colorspace == AVCOL_SPC_BT709 && c->color_range == AVCOL_RANGE_MPEG && c->color_primaries == AVCOL_PRI_BT709 && c->color_trc == AVCOL_TRC_BT709, "709 limited metadata");
        }
        NvencProfile invalid; invalid.preset = "p3"; rejects([&]{ NvencEncoder::configuredContext(invalid); });
        NvencEncoder unopened({}); auto frame = ffFrame(); rejects([&]{ unopened.submit(*frame, 0, [](const AVPacket&){}); });
    });
    s.test("encode pool includes retained refs without starving preview or allocating replacements", []
    {
        for (const auto fps : {30,60})
        {
            NvencProfile p; p.fps = fps; NvencFramePool encode(p.cpuSurfaces(),16,16); VideoSurfacePool preview(16,16);
            VideoSurface source; source.prepare(16,16); std::fill(source.nv12.begin(),source.nv12.end(),64);
            std::vector<int> slots;
            for (int i = 0; i < encode.capacity(); ++i) { source.stamp.frame = i + 1; require(encode.copy(source), "Fill bounded encode pool"); int slot = -1; require(encode.pop(slot), "Consume FIFO"); slots.push_back(slot); }
            require(!encode.copy(source) && encode.highWater() == static_cast<unsigned>(p.cpuSurfaces()) && p.cpuSurfaces() + 4 <= p.surfaceLimit(), "Pool plus NVENC surfaces stay within 8/15 cap");
            for (int i = 0; i < 100; ++i) { const auto slot = preview.acquireWrite(); require(slot >= 0,"Preview has independent reserved storage"); preview.publish(slot); }
            auto retained = ffFrame(); ffCheck(av_frame_ref(retained.get(), &encode.frame(slots[0])), "Retain fixture encoder reference");
            for (auto slot : slots) encode.release(slot);
            std::fill(source.nv12.begin(),source.nv12.end(),192); slots.clear();
            for (int i = 1; i < encode.capacity(); ++i) { require(encode.copy(source),"Other buffers remain reusable"); int slot = -1; require(encode.pop(slot),"Frame queue order"); slots.push_back(slot); }
            require(!encode.copy(source) && retained->data[0][0] == 64, "Retained payload cannot be overwritten or copied into an unbudgeted frame");
            for (auto slot : slots) encode.release(slot);
            av_frame_unref(retained.get()); require(encode.copy(source), "Released FFmpeg ref restores reusable capacity");
            int slot = -1; require(encode.pop(slot),"Final frame"); encode.release(slot);
        }
    });
    s.test("AAC resampler, priming and partial tail arithmetic agree with real built-in encoder", []
    {
        for (const std::int64_t count : {48000LL,48001LL,49152LL})
        {
            ReferenceMixWriter audio; std::int64_t first = AV_NOPTS_VALUE, end = AV_NOPTS_VALUE; unsigned packets = 0;
            const auto sink = [&](const AVPacket& p) { if (!packets++) first = p.pts; require(p.pts == p.dts && p.duration > 0,"AAC packet timing"); end = p.pts + p.duration; };
            audio.advance(count/2,sink); audio.finish(count,sink);
            require(first == -audio.context().initial_padding && first == -1024 && end == count, "Negative priming and exact valid last packet end");
            const auto padding = ReferenceMixWriter::padding(count,1024,1024);
            require((count+1024+padding)%1024 == 0 && padding >= 0 && padding < 1024, "AAC priming/padding bounds");
            require(static_cast<juce::int64>(audio.toJson()["presentationSamples"]) == count, "Exactly requested resampled presentation samples");
            rejects([&]{ audio.advance(count+1,sink); });
        }
        rejects([]{ ReferenceMixWriter::padding(-1,1024,1024); });
    });
    s.test("hybrid MP4 normal stop has exact packet counts, final presentation zero and decodes offline", []
    {
        auto video = fixtureEncoder(); auto frame = fixtureFrame(); auto packet = ffPacket(); ReferenceMixWriter audio;
        const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderEncoderContracts-" + juce::Uuid().toString() + ".mp4");
        Mp4TakeWriter writer(file,*video,audio.context()); const auto recordingFile = writer.recordingFile();
        require(recordingFile.existsAsFile() && !file.exists(), "In-progress extension only");
        const PacketSink audioSink = [&](const AVPacket& p){ writer.audio(p); };
        const auto receive = [&]
        {
            for (;;) { const auto result = avcodec_receive_packet(video.get(),packet.get()); if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
                ffCheck(result,"Software fixture video packet"); if (!packet->duration) packet->duration=1; writer.video(*packet); av_packet_unref(packet.get()); }
        };
        constexpr std::int64_t audioSamples = 48001;
        for (int i = 0; i < 31; ++i)
        {
            audio.advance(std::min<std::int64_t>((i+1)*1600,audioSamples),audioSink);
            frame->pts=i; ffCheck(avcodec_send_frame(video.get(),frame.get()),"Encode software fixture frame"); receive();
        }
        ffCheck(avcodec_send_frame(video.get(),nullptr),"Drain fixture video"); receive(); audio.finish(audioSamples,audioSink); writer.finalize();
        const auto report=writer.toJson();
        require(file.existsAsFile() && !recordingFile.exists() && static_cast<bool>(report["finalized"]),"Trailer before rename");
        require(static_cast<juce::int64>(report["videoPackets"])==31 && static_cast<juce::int64>(report["completedFragments"])>=2,"Real completed fragment telemetry");
        std::int64_t videoPackets=0,audioPackets=0;
        for (const auto& fragment : *report["fragments"].getArray()) { videoPackets+=static_cast<juce::int64>(fragment["videoPackets"]); audioPackets+=static_cast<juce::int64>(fragment["audioPackets"]); }
        require(videoPackets==31 && audioPackets==static_cast<juce::int64>(report["audioPackets"]),"trun counts include exactly every muxed packet");
        require(static_cast<bool>(Mp4TakeWriter::inspect(file)["presentationStartsAtZero"]),"Both final streams start at zero");
        verifyDecode(file,31,audioSamples);
        rejects([&]{ writer.finalize(); }); rejects([&]{ Mp4TakeWriter duplicate(file,*video,audio.context()); });
        std::cout << "Offline hybrid fixture: " << file.getFullPathName() << "; trailerMs=" << report["trailerMs"].toString() << '\n';
    });
    s.test("abandoned writer preserves recording file, no destructor trailer or rename", []
    {
        const auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderAbandoned-"+juce::Uuid().toString()+".mp4");
        auto video=fixtureEncoder(); ReferenceMixWriter audio; juce::File recording;
        { Mp4TakeWriter writer(file,*video,audio.context()); recording=writer.recordingFile(); rejects([&]{ writer.finalize(); }); }
        require(recording.existsAsFile() && !file.exists(),"Incomplete original remains available for later recovery round");
        // Open for exclusive access proves the destructor released AVIO/file handles.
        const auto handle=CreateFileW(recording.getFullPathName().toWideCharPointer(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
        require(handle!=INVALID_HANDLE_VALUE,"No leaked writer/observer handle"); CloseHandle(handle);
    });
    return s.result("RecorderEncoderContracts");
}
