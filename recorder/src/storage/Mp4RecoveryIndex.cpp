#include "Mp4RecoveryIndex.h"
#include "StorageEncoding.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace gocue::recorder
{
namespace
{
using namespace recovery;
std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
std::uint64_t be64(const std::uint8_t* p) { return (std::uint64_t(be32(p)) << 32) | be32(p + 4); }
bool tag(const std::uint8_t* p, const char* s) { return std::memcmp(p, s, 4) == 0; }
void readAt(juce::FileInputStream& in, std::uint64_t at, void* p, int size)
{
    require(at <= std::uint64_t(INT64_MAX) && at <= std::uint64_t(in.getTotalLength())
        && std::uint64_t(size) <= std::uint64_t(in.getTotalLength()) - at, "MP4 range outside actual bytes");
    require(in.setPosition(juce::int64(at)) && in.read(p, size) == size, "MP4 short read"); check(in.getStatus());
}
struct Packet { std::uint64_t offset = 0; std::uint32_t size = 0, track = 0, flags = 0; std::int64_t pts = 0, dts = 0, duration = 0; };
// Strict parser for the pinned writer profile: default_base_moof, explicit
// tfdt, one or more truns, no encryption, max 1 MiB moof / 64 MiB packet.
std::vector<Packet> packets(const juce::MemoryBlock& moof, std::uint64_t start, std::uint64_t data, std::uint64_t end)
{
    const auto* bytes = static_cast<const std::uint8_t*>(moof.getData()); const auto length = moof.getSize();
    require(length >= 8 && length <= 1024 * 1024 && be32(bytes) == length && tag(bytes + 4, "moof"), "Invalid saved moof");
    std::vector<Packet> out;
    for (std::size_t at = 8; at < length;)
    {
        require(length - at >= 8, "Truncated moof child"); const auto size = be32(bytes + at);
        require(size >= 8 && size <= length - at, "Invalid moof child");
        if (tag(bytes + at + 4, "traf"))
        {
            std::uint32_t track = 0, defaultDuration = 0, defaultSize = 0, defaultFlags = 0;
            std::int64_t dts = -1; std::uint64_t base = start, next = data;
            for (std::size_t p = at + 8; p < at + size;)
            {
                require(at + size - p >= 8, "Truncated traf child"); const auto n = be32(bytes + p);
                require(n >= 8 && n <= at + size - p, "Invalid traf child");
                const auto* b = bytes + p;
                if (tag(b + 4, "tfhd"))
                {
                    require(n >= 16, "Short tfhd"); const auto flags = be32(b + 8) & 0xffffffu; track = be32(b + 12); std::size_t q = 16;
                    require(flags & 0x020000u, "Unsupported non-moof-relative fragment");
                    require(!(flags & 1u), "Unexpected explicit base data offset");
                    const auto take = [&]() { require(q + 4 <= n, "Short tfhd defaults"); const auto v = be32(b + q); q += 4; return v; };
                    if (flags & 2u) take();
                    if (flags & 8u) defaultDuration = take();
                    if (flags & 16u) defaultSize = take();
                    if (flags & 32u) defaultFlags = take();
                }
                else if (tag(b + 4, "tfdt"))
                {
                    require(n >= 16 && b[8] <= 1 && (b[8] == 0 || n >= 20), "Short/unknown tfdt");
                    const auto t = b[8] ? be64(b + 12) : be32(b + 12); require(t <= std::uint64_t(INT64_MAX), "tfdt overflow"); dts = std::int64_t(t);
                }
                else if (tag(b + 4, "trun"))
                {
                    require(track && dts >= 0 && n >= 16 && b[8] <= 1, "trun missing tfhd/tfdt");
                    const auto flags = be32(b + 8) & 0xffffffu, count = be32(b + 12); std::size_t q = 16;
                    require(count <= 65536, "trun packet count exceeds fragment bound");
                    const auto take = [&]() { require(q + 4 <= n, "Short trun sample table"); const auto v = be32(b + q); q += 4; return v; };
                    if (flags & 1u) { const auto offset = static_cast<std::int32_t>(take()); require(offset >= 0 && base <= UINT64_MAX - std::uint32_t(offset), "Invalid trun data offset"); next = base + std::uint32_t(offset); }
                    const auto firstFlags = flags & 4u ? take() : defaultFlags;
                    for (std::uint32_t i = 0; i < count; ++i)
                    {
                        Packet pkt; pkt.track = track; pkt.offset = next; pkt.dts = dts;
                        pkt.duration = flags & 0x100u ? take() : defaultDuration; pkt.size = flags & 0x200u ? take() : defaultSize;
                        pkt.flags = flags & 0x400u ? take() : i == 0 ? firstFlags : defaultFlags;
                        const auto cts = flags & 0x800u ? take() : 0;
                        const std::int64_t delta = b[8] == 1 ? std::int64_t(static_cast<std::int32_t>(cts)) : std::int64_t(cts);
                        require(pkt.duration > 0 && dts <= INT64_MAX - pkt.duration && delta >= -dts && (delta <= 0 || dts <= INT64_MAX - delta), "Invalid packet timestamp");
                        pkt.pts = dts + delta;
                        require(pkt.size > 0 && pkt.size <= 64 * 1024 * 1024 && next >= data && next <= end && pkt.size <= end - next, "Packet outside complete mdat");
                        next += pkt.size; dts += pkt.duration; out.push_back(pkt);
                    }
                    require(q == n, "Unexpected trun fields");
                }
                p += n;
            }
        }
        at += size;
    }
    require(!out.empty(), "Empty fragment index");
    auto sorted = out; std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a.offset < b.offset; });
    std::uint64_t previous = data;
    for (const auto& p : sorted) { require(p.offset >= previous, "Overlapping packet offsets"); previous = p.offset + p.size; }
    return out;
}
juce::MemoryBlock decodeBlock(const juce::var& v)
{ juce::MemoryBlock out; require(v.isString() && out.fromBase64Encoding(v.toString()), "Invalid index byte encoding"); return out; }
struct Input { AVFormatContext* p = nullptr; ~Input() { avformat_close_input(&p); } };
struct Output
{
    AVFormatContext* p = nullptr; AVIOContext* io = nullptr; DurableFile file; std::uint64_t position = 0;
    explicit Output(FileIoFaultAdapter* f) : file(f) {}
    ~Output() { if (io) { av_freep(&io->buffer); avio_context_free(&io); } if (p) { p->pb = nullptr; avformat_free_context(p); } }
    static int write(void* context, const std::uint8_t* b, int n)
    {
        auto& s = *static_cast<Output*>(context);
        auto r = s.position == s.file.writtenBytes() ? s.file.write(b, std::size_t(n)) : s.file.writeAt(s.position, b, std::size_t(n));
        if (r.failed()) return AVERROR(EIO); s.position += std::uint64_t(n); return n;
    }
    static std::int64_t seek(void* context, std::int64_t offset, int whence)
    {
        auto& s = *static_cast<Output*>(context); if (whence & AVSEEK_SIZE) return std::int64_t(s.file.writtenBytes());
        whence &= ~AVSEEK_FORCE; std::int64_t base = 0;
        if (whence == SEEK_CUR) base = std::int64_t(s.position); else if (whence == SEEK_END) base = std::int64_t(s.file.writtenBytes()); else if (whence != SEEK_SET) return AVERROR(EINVAL);
        if (offset < -base || (offset > 0 && base > INT64_MAX - offset)) return AVERROR(EINVAL);
        s.position = std::uint64_t(base + offset); return std::int64_t(s.position);
    }
    void open(const juce::File& path)
    {
        const auto directory = path.getParentDirectory().createDirectory();
        if (directory.failed()) throw OutputError(directory.getErrorMessage().toStdString());
        const auto opened = file.open(path, DurableFile::OpenMode::createNew);
        if (opened.failed()) throw OutputError(opened.getErrorMessage().toStdString());
        ffCheck(avformat_alloc_output_context2(&p, nullptr, "mp4", nullptr), "Allocate recovery MP4");
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(65536)); require(buffer != nullptr, "Allocate MP4 IO");
        io = avio_alloc_context(buffer, 65536, 1, this, nullptr, write, seek);
        if (!io) { av_free(buffer); throw std::bad_alloc(); }
        io->seekable = AVIO_SEEKABLE_NORMAL; p->pb = io; p->flags |= AVFMT_FLAG_CUSTOM_IO; p->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    }
};
}
juce::File Mp4RecoveryIndex::pathFor(const juce::File& mp4)
{
    return mp4.getParentDirectory().getChildFile("index/" + mp4.getFileNameWithoutExtension().replace(".recording", "") + ".packets.log");
}
void Mp4RecoveryIndex::start(const juce::File& path, const AVFormatContext& format, const juce::File& initialMp4)
{
    // The muxer may transform Annex B encoder extradata to avcC. Capture the
    // initial serialized moov's parameters, because indexed payload is AVCC.
    // This runs on the sole mux owner, paused after the initial header flush.
    Input initial;
    if (initialMp4 != juce::File()) ffCheck(avformat_open_input(&initial.p, initialMp4.getFullPathName().toRawUTF8(), nullptr, nullptr), "Read serialized initial MP4 codecs");
    log.open(path, true); auto v = object(); juce::Array<juce::var> streams;
    for (unsigned i = 0; i < format.nb_streams; ++i)
    {
        const auto& s = *format.streams[i]; const AVCodecParameters* actual = s.codecpar;
        if (initial.p) for (unsigned j = 0; j < initial.p->nb_streams; ++j) if (initial.p->streams[j]->id == s.id) actual = initial.p->streams[j]->codecpar;
        const auto& c = *actual; auto x = object();
        require(c.extradata_size > 0 && c.extradata_size < 1024 * 1024, "Missing/oversize initial codec extradata");
        set(x, "track", s.id); set(x, "codec", int(c.codec_id)); set(x, "type", int(c.codec_type));
        set(x, "width", c.width); set(x, "height", c.height); set(x, "rate", c.sample_rate); set(x, "channels", c.ch_layout.nb_channels);
        set(x, "timeNum", s.time_base.num); set(x, "timeDen", s.time_base.den); set(x, "fpsNum", s.avg_frame_rate.num); set(x, "fpsDen", s.avg_frame_rate.den);
        set(x, "padding", c.initial_padding); set(x, "frameSize", c.frame_size);
        set(x, "extra", juce::MemoryBlock(c.extradata, std::size_t(c.extradata_size)).toBase64Encoding()); streams.add(x);
    }
    set(v, "streams", streams); log.append(Kind::codec, v);
}
void Mp4RecoveryIndex::fragment(const juce::File& source, std::uint64_t moofStart, std::uint64_t end)
{
    juce::FileInputStream in(source); check(in.getStatus()); std::uint8_t h[16]{}; readAt(in, moofStart, h, 8);
    const auto length = be32(h); require(length >= 8 && length <= 1024 * 1024, "Moof size bound");
    juce::MemoryBlock moof(length); readAt(in, moofStart, moof.getData(), int(length));
    const auto mdat = moofStart + length; readAt(in, mdat, h, 8); require(tag(h + 4, "mdat"), "moof must precede mdat");
    std::uint64_t mdatSize = be32(h), header = 8;
    if (mdatSize == 1) { readAt(in, mdat + 8, h + 8, 8); mdatSize = be64(h + 8); header = 16; }
    require(mdat <= end && mdatSize == end - mdat && mdatSize >= header, "Incomplete mdat");
    const auto parsed = packets(moof, moofStart, mdat + header, end); juce::Array<juce::var> checksums;
    for (const auto& pkt : parsed)
    {
        std::vector<std::uint8_t> data(pkt.size); readAt(in, pkt.offset, data.data(), int(data.size()));
        checksums.add(integer(storageEncoding::crc32(data.data(), data.size())));
    }
    auto v = object(); set(v, "moof", moof.toBase64Encoding()); set(v, "start", integer(std::int64_t(moofStart)));
    set(v, "data", integer(std::int64_t(mdat + header))); set(v, "end", integer(std::int64_t(end))); set(v, "packetCrc", checksums);
    log.append(Kind::fragment, v);
}
RecoveredMp4 Mp4RecoveryIndex::recover(const juce::File& source, const juce::File& index, const juce::File& dest, std::uint32_t Fs, FileIoFaultAdapter* faults)
{
    juce::FileInputStream in(source); check(in.getStatus()); Output out(faults); out.open(dest);
    try
    {
    std::map<int, int> tracks; std::vector<AVRational> bases; std::vector<std::int64_t> last;
    bool header = false; std::uint64_t lastEnd = 0, packetCount = 0, fragmentCount = 0, videoPackets = 0;
    const auto replay = Log::read(index, [&](const Record& r)
    {
        if (r.kind == Kind::codec)
        {
            require(!header && r.sequence == 1 && r.payload["streams"].isArray(), "Invalid codec index");
            for (const auto& v : *r.payload["streams"].getArray())
            {
                const auto codec = number(v["codec"]), type = number(v["type"]);
                require((codec == AV_CODEC_ID_H264 && type == AVMEDIA_TYPE_VIDEO) || (codec == AV_CODEC_ID_AAC && type == AVMEDIA_TYPE_AUDIO), "Unsupported recovery codec");
                auto* s = avformat_new_stream(out.p, nullptr); require(s != nullptr, "Allocate recovery stream");
                const auto id = int(number(v["track"])); require(id > 0 && tracks.emplace(id, s->index).second, "Duplicate track index");
                auto* c = s->codecpar; c->codec_id = AVCodecID(codec); c->codec_type = AVMediaType(type);
                c->width = int(number(v["width"])); c->height = int(number(v["height"])); c->sample_rate = int(number(v["rate"]));
                if (type == AVMEDIA_TYPE_AUDIO) { require(number(v["channels"]) == 2 && c->sample_rate == 48000, "Invalid reference audio"); av_channel_layout_default(&c->ch_layout, 2); }
                c->initial_padding = int(number(v["padding"])); c->frame_size = int(number(v["frameSize"]));
                auto extra = decodeBlock(v["extra"]); require(extra.getSize() > 0 && extra.getSize() <= 1024 * 1024, "Invalid extradata size");
                c->extradata = static_cast<std::uint8_t*>(av_mallocz(extra.getSize() + AV_INPUT_BUFFER_PADDING_SIZE)); require(c->extradata != nullptr, "Allocate extradata");
                std::memcpy(c->extradata, extra.getData(), extra.getSize()); c->extradata_size = int(extra.getSize());
                s->time_base = {int(number(v["timeNum"])), int(number(v["timeDen"]))};
                require(s->time_base.num > 0 && s->time_base.den > 0, "Invalid index time base");
                s->avg_frame_rate = {int(number(v["fpsNum"])), int(number(v["fpsDen"]))};
                bases.push_back(s->time_base); last.push_back(AV_NOPTS_VALUE);
            }
            require(tracks.size() == 2, "Expected camera H264 + AAC");
            Dictionary options; options.set("use_editlist", "1"); options.set("movie_timescale", "48000");
            ffCheck(avformat_write_header(out.p, &options.value), "Recovery ordinary MP4 header"); header = true; return true;
        }
        require(header && r.kind == Kind::fragment, "Invalid fragment index order");
        const auto start = number(r.payload["start"]), data = number(r.payload["data"]), end = number(r.payload["end"]);
        if (start < 0 || data <= start || end <= data || std::uint64_t(start) < lastEnd || end > in.getTotalLength()) return false;
        const auto moof = decodeBlock(r.payload["moof"]); const auto parsed = packets(moof, std::uint64_t(start), std::uint64_t(data), std::uint64_t(end));
        const auto crc = r.payload["packetCrc"]; require(crc.isArray() && crc.size() == int(parsed.size()), "Packet index count mismatch");
        // Validate the ENTIRE fragment before emitting any packet. A damaged
        // packet excludes its whole fragment and every later one.
        for (std::size_t i = 0; i < parsed.size(); ++i)
        {
            const auto& p = parsed[i]; std::vector<std::uint8_t> bytes(p.size); readAt(in, p.offset, bytes.data(), int(p.size));
            if (number(crc[int(i)]) != storageEncoding::crc32(bytes.data(), bytes.size())) return false;
            require(tracks.count(int(p.track)) != 0, "Unknown packet track");
        }
        for (const auto& p : parsed)
        {
            const int stream = tracks.at(int(p.track)); require(last[std::size_t(stream)] == AV_NOPTS_VALUE || p.dts > last[std::size_t(stream)], "Nonmonotonic recovery DTS");
            if (out.p->streams[stream]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ++videoPackets;
            auto packet = ffPacket(); ffCheck(av_new_packet(packet.get(), int(p.size)), "Allocate recovered packet"); readAt(in, p.offset, packet->data, int(p.size));
            packet->pts = p.pts; packet->dts = p.dts; packet->duration = p.duration; packet->stream_index = stream;
            packet->flags = p.flags & 0x10000u ? 0 : AV_PKT_FLAG_KEY;
            av_packet_rescale_ts(packet.get(), bases[std::size_t(stream)], out.p->streams[stream]->time_base);
            ffCheck(av_interleaved_write_frame(out.p, packet.get()), "Stream-copy recovered packet"); last[std::size_t(stream)] = p.dts; ++packetCount;
        }
        lastEnd = std::uint64_t(end); ++fragmentCount; return true;
    });
    require(header && packetCount > 0, "No validated MP4 packets");
    ffCheck(av_write_trailer(out.p), "Recovery ordinary MP4 trailer"); avio_flush(out.io); check(out.file.status()); ffCheck(out.io->error, "Recovery MP4 AVIO flush");
    check(out.file.flushData()); check(out.file.close());
    auto result = decode(dest, Fs); require(result.videoFrames == videoPackets, "Decoded frame count differs from validated packet index");
    result.packets = packetCount; result.fragments = fragmentCount; result.ignoredTail = replay.ignoredTail; return result;
    }
    catch (...)
    {
        if (out.file.status().failed()) throw OutputError(out.file.status().getErrorMessage().toStdString());
        throw;
    }
}
RecoveredMp4 Mp4RecoveryIndex::decode(const juce::File& path, std::uint32_t Fs)
{
    Input in; ffCheck(avformat_open_input(&in.p, path.getFullPathName().toRawUTF8(), nullptr, nullptr), "Open decode verification");
    ffCheck(avformat_find_stream_info(in.p, nullptr), "Decode stream metadata");
    std::vector<CodecPtr> codecs; std::vector<std::uint64_t> frames; RecoveredMp4 result;
    for (unsigned i = 0; i < in.p->nb_streams; ++i)
    {
        const auto* par = in.p->streams[i]->codecpar; const auto* decoder = avcodec_find_decoder(par->codec_id); require(decoder != nullptr, "Missing full verification decoder");
        CodecPtr c(avcodec_alloc_context3(decoder)); require(c != nullptr, "Allocate verification decoder");
        ffCheck(avcodec_parameters_to_context(c.get(), par), "Decoder parameters"); c->thread_count = 1;
        c->err_recognition = AV_EF_EXPLODE | AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER;
        ffCheck(avcodec_open2(c.get(), decoder, nullptr), "Open verification decoder"); codecs.push_back(std::move(c)); frames.push_back(0);
        if (par->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            result.format.codec = avcodec_get_name(par->codec_id); result.format.width = par->width; result.format.height = par->height;
            const auto rate = in.p->streams[i]->avg_frame_rate; require(rate.num > 0 && rate.den > 0, "Missing video rate");
            result.format.fps = {std::uint32_t(rate.num), std::uint32_t(rate.den)};
        }
    }
    auto frame = ffFrame();
    const auto drain = [&](unsigned i)
    {
        for (;;)
        {
            const int code = avcodec_receive_frame(codecs[i].get(), frame.get()); if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) break;
            ffCheck(code, "Full decode frame"); require(!frame->decode_error_flags && !(frame->flags & AV_FRAME_FLAG_CORRUPT), "Corrupt decoded frame"); ++frames[i];
            if (codecs[i]->codec_type == AVMEDIA_TYPE_VIDEO) ++result.videoFrames; else result.audioSamples += std::uint64_t(frame->nb_samples);
            av_frame_unref(frame.get());
        }
    };
    auto packet = ffPacket(); int status = 0;
    while ((status = av_read_frame(in.p, packet.get())) >= 0)
    {
        const auto i = unsigned(packet->stream_index); require(i < codecs.size() && !(packet->flags & AV_PKT_FLAG_CORRUPT), "Invalid/corrupt demux packet");
        if (codecs[i]->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            require(packet->pts != AV_NOPTS_VALUE && packet->duration > 0 && packet->pts <= INT64_MAX - packet->duration, "Invalid decoded video extent");
            result.samples = std::max(result.samples, rescaleRound(packet->pts + packet->duration,
                std::uint64_t(in.p->streams[i]->time_base.num) * Fs, std::uint64_t(in.p->streams[i]->time_base.den)));
        }
        ffCheck(avcodec_send_packet(codecs[i].get(), packet.get()), "Full decode packet"); drain(i); av_packet_unref(packet.get());
    }
    require(status == AVERROR_EOF, "Full decode demux failure");
    for (unsigned i = 0; i < codecs.size(); ++i) { ffCheck(avcodec_send_packet(codecs[i].get(), nullptr), "Full decode drain"); drain(i); require(frames[i] > 0, "Empty decoded stream"); }
    require(result.videoFrames > 0 && result.audioSamples > 0, "Missing decoded camera/reference stream"); return result;
}
void Mp4RecoveryIndex::rebuild(const juce::File& source, const juce::File& index)
{
    Input input; ffCheck(avformat_open_input(&input.p, source.getFullPathName().toRawUTF8(), nullptr, nullptr), "Read legacy initial codecs");
    ffCheck(avformat_find_stream_info(input.p, nullptr), "Read legacy stream parameters");
    Mp4RecoveryIndex writer; writer.start(index, *input.p); juce::FileInputStream in(source); check(in.getStatus());
    std::uint64_t at = 0, moof = UINT64_MAX;
    while (at + 8 <= std::uint64_t(in.getTotalLength()))
    {
        std::uint8_t h[16]{}; readAt(in, at, h, 8); std::uint64_t size = be32(h), header = 8;
        if (size == 1) { if (at + 16 > std::uint64_t(in.getTotalLength())) break; readAt(in, at + 8, h + 8, 8); size = be64(h + 8); header = 16; }
        if (size < header || size > std::uint64_t(in.getTotalLength()) - at) break;
        if (tag(h + 4, "moof")) moof = at;
        if (tag(h + 4, "mdat") && moof != UINT64_MAX) { writer.fragment(source, moof, at + size); moof = UINT64_MAX; }
        at += size;
    }
    writer.close();
}
}
