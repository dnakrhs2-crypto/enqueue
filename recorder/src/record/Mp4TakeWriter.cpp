#include "Mp4TakeWriter.h"
#include "storage/Mp4RecoveryIndex.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace gocue::recorder
{
namespace
{
std::uint32_t be32(const uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
std::uint64_t be64(const uint8_t* p) { return (std::uint64_t(be32(p)) << 32) | be32(p + 4); }
constexpr std::uint32_t tag(char a, char b, char c, char d) { return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(c) << 8) | d; }
struct Fragment
{
    std::uint64_t start = 0, end = 0, videoPackets = 0, audioPackets = 0;
};
}
struct Mp4TakeWriter::State
{
    juce::File finalFile, recording;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE, reader = INVALID_HANDLE_VALUE;
    AVRational videoBase{}, audioBase{};
    PacketPtr packet = ffPacket();
    std::int64_t position = 0, highEnd = 0, scanOffset = 0;
    std::int64_t lastPts[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE}, lastDts[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    std::uint64_t packetCounts[2]{}, fragmentCount = 0;
    std::array<Fragment, 128> fragments{};
    Fragment pending;
    bool haveMoof = false, finalized = false;
    bool indexing = false, inTrailer = false;
    FileIoFaultAdapter* faults = nullptr;
    recovery::Hook hook;
    Mp4RecoveryIndex index;
    juce::String ioError;
    double trailerMs = 0, flushMs = 0, renameMs = 0;
    explicit State(juce::File f, FileIoFaultAdapter* adapter, recovery::Hook h)
        : finalFile(std::move(f)), recording(finalFile.getSiblingFile(finalFile.getFileNameWithoutExtension() + ".recording.mp4")),
          faults(adapter), hook(std::move(h)), index(adapter) {}
    ~State()
    {
        if (io) { avio_flush(io); av_freep(&io->buffer); avio_context_free(&io); }
        if (format) { format->pb = nullptr; avformat_free_context(format); }
        closeHandles();
    }
    void closeHandles()
    {
        if (reader != INVALID_HANDLE_VALUE) { CloseHandle(reader); reader = INVALID_HANDLE_VALUE; }
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
    }
    static int write(void* opaque, const uint8_t* bytes, int size)
    {
        auto& s = *static_cast<State*>(opaque);
        try
        {
            if (s.faults) recovery::check(s.faults->beforeIo(s.position < s.highEnd ? FileIoOperation::patch : FileIoOperation::append,
                s.recording, s.faults->observedOffset(std::uint64_t(s.position)), std::size_t(size)));
            const auto* moov = std::search(bytes, bytes + size, "moov", "moov" + 4);
            const bool finalMoov = s.inTrailer && moov != bytes + size;
            const bool split = s.hook && s.indexing && size > 16 && (!s.inTrailer || finalMoov);
            const int first = split ? finalMoov ? int((moov - bytes + size) / 2) : size / 2 : size;
            const auto writePart = [&](const uint8_t* p, int n)
            {
                DWORD written = 0;
                recovery::require(WriteFile(s.file, p, DWORD(n), &written, nullptr) && written == DWORD(n), "MP4 WriteFile failed (Win32 " + juce::String(int(GetLastError())) + ")");
                s.position += n; s.highEnd = std::max(s.highEnd, s.position);
            };
            writePart(bytes, first);
            if (split) { recovery::hit(s.hook, finalMoov ? "final-moov-write" : "fragment-write"); writePart(bytes + first, size - first); }
            return size;
        }
        catch (const std::exception& e) { s.ioError = e.what(); return AVERROR(EIO); }
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int origin)
    {
        auto& s = *static_cast<State*>(opaque);
        if (origin & AVSEEK_SIZE) return s.highEnd;
        origin &= ~AVSEEK_FORCE;
        if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) return AVERROR(EINVAL);
        LARGE_INTEGER move{}, result{}; move.QuadPart = offset;
        if (!SetFilePointerEx(s.file, move, &result, origin == SEEK_SET ? FILE_BEGIN : origin == SEEK_CUR ? FILE_CURRENT : FILE_END)) return AVERROR(EIO);
        s.position = result.QuadPart; return s.position;
    }
    void readAt(std::int64_t offset, uint8_t* bytes, DWORD size)
    {
        LARGE_INTEGER at{}; at.QuadPart = offset; DWORD count = 0;
        if (!SetFilePointerEx(reader, at, nullptr, FILE_BEGIN) || !ReadFile(reader, bytes, size, &count, nullptr) || count != size)
            throw std::runtime_error("Read completed MP4 box failed");
    }
    void countTraf(const uint8_t* bytes, size_t length, Fragment& f)
    {
        std::uint32_t track = 0; std::uint64_t count = 0;
        for (size_t at = 0; at + 8 <= length;)
        {
            const auto size = be32(bytes + at), type = be32(bytes + at + 4);
            if (size < 8 || size > length - at) throw std::runtime_error("Malformed completed traf box");
            if (type == tag('t','f','h','d') && size >= 16) track = be32(bytes + at + 12);
            if (type == tag('t','r','u','n') && size >= 16) count += be32(bytes + at + 12);
            at += size;
        }
        if (track == 1) f.videoPackets += count;
        else if (track == 2) f.audioPackets += count;
        else throw std::runtime_error("Unexpected MP4 fragment track ID");
    }
    void observeFragments()
    {
        avio_flush(io); ffCheck(io->error, "Flush MP4 AVIO");
        while (scanOffset + 8 <= highEnd)
        {
            uint8_t header[16]{}; readAt(scanOffset, header, 8);
            std::uint64_t size = be32(header); const auto type = be32(header + 4);
            size_t headerSize = 8;
            if (size == 1)
            {
                if (scanOffset + 16 > highEnd) return;
                readAt(scanOffset + 8, header + 8, 8); size = be64(header + 8); headerSize = 16;
            }
            if (!size || size > static_cast<std::uint64_t>(highEnd - scanOffset)) return;
            if (size < headerSize) throw std::runtime_error("Malformed completed MP4 box size");
            if (type == tag('m','o','o','f'))
            {
                if (size > 1024 * 1024) throw std::runtime_error("MP4 moof exceeds diagnostic bound");
                std::vector<uint8_t> bytes(static_cast<size_t>(size)); readAt(scanOffset, bytes.data(), static_cast<DWORD>(size));
                pending = {}; pending.start = scanOffset; haveMoof = true;
                for (size_t at = headerSize; at + 8 <= bytes.size();)
                {
                    const auto child = be32(bytes.data() + at);
                    if (child < 8 || child > bytes.size() - at) throw std::runtime_error("Malformed moof child");
                    if (be32(bytes.data() + at + 4) == tag('t','r','a','f')) countTraf(bytes.data() + at + 8, child - 8, pending);
                    at += child;
                }
            }
            else if (type == tag('m','d','a','t') && haveMoof)
            {
                pending.end = scanOffset + size;
                if (indexing)
                {
                    if (faults) recovery::check(faults->beforeIo(FileIoOperation::flushData, recording, std::uint64_t(highEnd), 0));
                    if (!FlushFileBuffers(file)) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Flush completed MP4 fragment");
                    index.fragment(recording, pending.start, pending.end);
                }
                fragments[fragmentCount++ % fragments.size()] = pending; haveMoof = false;
            }
            scanOffset += static_cast<std::int64_t>(size);
        }
    }
    void append(const AVPacket& input, int stream)
    {
        if (finalized || input.pts == AV_NOPTS_VALUE || input.dts == AV_NOPTS_VALUE || input.duration <= 0
            || (packetCounts[stream] && input.dts <= lastDts[stream])) throw std::invalid_argument("Invalid MP4 packet/state or nonmonotonic DTS");
        const bool key = stream == 0 && (input.flags & AV_PKT_FLAG_KEY);
        av_packet_unref(packet.get()); ffCheck(av_packet_ref(packet.get(), &input), "Reference mux packet");
        av_packet_rescale_ts(packet.get(), stream == 0 ? videoBase : audioBase, format->streams[stream]->time_base);
        packet->stream_index = stream; packet->pos = -1;
        ffCheck(av_interleaved_write_frame(format, packet.get()), "Mux MP4 packet");
        lastPts[stream] = input.pts; lastDts[stream] = input.dts; ++packetCounts[stream];
        if (key)
        {
            ffCheck(av_interleaved_write_frame(format, nullptr), "Flush interleaver at IDR");
            observeFragments();
        }
    }
};
Mp4TakeWriter::Mp4TakeWriter(const juce::File& file, const AVCodecContext& video, const AVCodecContext& audio,
                           FileIoFaultAdapter* faults, recovery::Hook hook)
    : state(std::make_unique<State>(file, faults, std::move(hook)))
{
    auto& s = *state;
    if (file.getFileExtension() != ".mp4" || file.exists() || s.recording.exists()) throw std::invalid_argument("MP4 output exists or does not end in .mp4; use a new output directory");
    if (video.codec_type != AVMEDIA_TYPE_VIDEO || audio.codec_id != AV_CODEC_ID_AAC || audio.sample_rate != 48000 || audio.ch_layout.nb_channels != 2)
        throw std::invalid_argument("MP4 take requires video plus 48k stereo AAC");
    const auto directory = file.getParentDirectory().createDirectory();
    if (directory.failed()) throw std::runtime_error(directory.getErrorMessage().toStdString());
    ffCheck(avformat_alloc_output_context2(&s.format, nullptr, "mp4", nullptr), "Allocate MP4 context");
    s.videoBase = video.time_base; s.audioBase = audio.time_base;
    for (const auto* codec : {&video, &audio})
    {
        auto* stream = avformat_new_stream(s.format, nullptr); if (!stream) throw std::bad_alloc();
        stream->id = stream->index + 1; stream->time_base = codec->time_base;
        ffCheck(avcodec_parameters_from_context(stream->codecpar, codec), "Copy MP4 codec parameters");
        if (codec == &video) stream->avg_frame_rate = codec->framerate;
    }
    s.format->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    s.format->max_interleave_delta = 250000;
    s.file = CreateFileW(s.recording.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Create new .recording.mp4");
    s.reader = CreateFileW(s.recording.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.reader == INVALID_HANDLE_VALUE) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Open MP4 box observer");
    auto* buffer = static_cast<uint8_t*>(av_malloc(65536)); if (!buffer) throw std::bad_alloc();
    s.io = avio_alloc_context(buffer, 65536, 1, &s, nullptr, State::write, State::seek);
    if (!s.io) { av_free(buffer); throw std::bad_alloc(); }
    s.io->seekable = AVIO_SEEKABLE_NORMAL; s.format->pb = s.io; s.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    Dictionary options;
    options.set("movflags", movFlags); options.set("use_editlist", "1"); options.set("movie_timescale", "48000");
    ffCheck(avformat_write_header(s.format, &options.value), "Write hybrid MP4 header");
    if (av_dict_count(options.value)) throw std::runtime_error("Unconsumed MP4 options");
    s.observeFragments();
    if (!FlushFileBuffers(s.file)) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Flush initial MP4 codec header");
    s.index.start(Mp4RecoveryIndex::pathFor(s.recording), *s.format, s.recording); s.indexing = true;
}
Mp4TakeWriter::~Mp4TakeWriter() = default;
void Mp4TakeWriter::video(const AVPacket& p) { state->append(p, 0); }
void Mp4TakeWriter::audio(const AVPacket& p) { state->append(p, 1); }
const juce::File& Mp4TakeWriter::recordingFile() const { return state->recording; }
void Mp4TakeWriter::finalize()
{
    auto& s = *state;
    if (s.finalized || !s.packetCounts[0] || !s.packetCounts[1]) throw std::logic_error("Cannot finalize empty/already finalized take");
    ffCheck(av_interleaved_write_frame(s.format, nullptr), "Drain MP4 interleaver");
    // Close the final fragment before hybrid trailer rewrites the root boxes.
    ffCheck(av_write_frame(s.format, nullptr), "Close final MP4 fragment"); s.observeFragments();
    s.inTrailer = true;
    auto start = qpcNow(); ffCheck(av_write_trailer(s.format), "Write hybrid MP4 trailer");
    s.trailerMs = 1000.0 * (qpcNow() - start) / qpcFrequency();
    start = qpcNow(); avio_flush(s.io); ffCheck(s.io->error, "Flush finalized MP4 AVIO");
    if (!FlushFileBuffers(s.file)) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Flush finalized MP4 file");
    s.flushMs = 1000.0 * (qpcNow() - start) / qpcFrequency();
    s.index.close(); s.closeHandles(); start = qpcNow();
    if (!MoveFileExW(s.recording.getFullPathName().toWideCharPointer(), s.finalFile.getFullPathName().toWideCharPointer(), MOVEFILE_WRITE_THROUGH))
        checkHr(HRESULT_FROM_WIN32(GetLastError()), "Rename finalized MP4 without replacement");
    s.renameMs = 1000.0 * (qpcNow() - start) / qpcFrequency(); s.finalized = true;
}
juce::var Mp4TakeWriter::toJson() const
{
    const auto& s = *state; auto v = jsonObject(); juce::Array<juce::var> fragments;
    const auto start = s.fragmentCount > s.fragments.size() ? s.fragmentCount - s.fragments.size() : 0;
    for (auto i = start; i < s.fragmentCount; ++i)
    {
        const auto& f = s.fragments[i % s.fragments.size()]; auto item = jsonObject();
        jsonSet(item, "index", jsonInt(i)); jsonSet(item, "moofOffset", jsonInt(f.start)); jsonSet(item, "completedMdatEnd", jsonInt(f.end));
        jsonSet(item, "videoPackets", jsonInt(f.videoPackets)); jsonSet(item, "audioPackets", jsonInt(f.audioPackets)); fragments.add(item);
    }
    jsonSet(v, "movflags", movFlags); jsonSet(v, "finalized", s.finalized);
    jsonSet(v, "recordingPath", s.recording.getFullPathName()); jsonSet(v, "finalPath", s.finalFile.getFullPathName());
    jsonSet(v, "videoPackets", jsonInt(s.packetCounts[0])); jsonSet(v, "audioPackets", jsonInt(s.packetCounts[1]));
    jsonSet(v, "lastVideoPts", juce::var(static_cast<juce::int64>(s.lastPts[0]))); jsonSet(v, "lastAudioPts", juce::var(static_cast<juce::int64>(s.lastPts[1])));
    jsonSet(v, "completedFragments", jsonInt(s.fragmentCount)); jsonSet(v, "fragments", fragments);
    jsonSet(v, "fragmentDefinition", "Last 128 completed moof+mdat boundaries. Full crash-only codec/tfhd/tfdt/trun/packet-CRC index is durable in index/*.packets.log after media FlushFileBuffers; not a GrowingTakeReader. Last PTS fields are mux input time bases.");
    jsonSet(v, "trailerMs", s.trailerMs); jsonSet(v, "fileFlushMs", s.flushMs); jsonSet(v, "renameMs", s.renameMs);
    jsonSet(v, "fileSizeBytes", jsonInt(s.finalized ? s.finalFile.getSize() : s.highEnd));
    return v;
}
juce::var Mp4TakeWriter::inspect(const juce::File& file)
{
    struct Input { AVFormatContext* p = nullptr; ~Input() { avformat_close_input(&p); } } input;
    ffCheck(avformat_open_input(&input.p, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Reopen finalized MP4");
    ffCheck(avformat_find_stream_info(input.p, nullptr), "Read finalized stream metadata");
    auto result = jsonObject(); juce::Array<juce::var> streams;
    bool presentationZero = input.p->nb_streams == 2;
    for (unsigned i = 0; i < input.p->nb_streams; ++i)
    {
        const auto& s = *input.p->streams[i]; auto v = jsonObject();
        jsonSet(v, "codec", avcodec_get_name(s.codecpar->codec_id)); jsonSet(v, "startTime", juce::var(static_cast<juce::int64>(s.start_time)));
        jsonSet(v, "timeBaseNumerator", s.time_base.num); jsonSet(v, "timeBaseDenominator", s.time_base.den);
        jsonSet(v, "duration", juce::var(static_cast<juce::int64>(s.duration))); jsonSet(v, "nbFrames", jsonInt(s.nb_frames));
        presentationZero &= s.start_time == 0; streams.add(v);
    }
    jsonSet(result, "streams", streams); jsonSet(result, "presentationStartsAtZero", presentationZero);
    jsonSet(result, "validation", "Final stream headers reopened after rename; full media decode and independent source frame oracle are external probe validation steps.");
    return result;
}
}
