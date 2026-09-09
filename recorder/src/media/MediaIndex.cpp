#include "MediaIndex.h"
#include "record/Ffmpeg.h"
#include "storage/RecordingJournal.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>

namespace gocue::recorder
{
namespace
{
void demand(bool ok, const char* error) { if (!ok) throw std::invalid_argument(error); }
struct Input
{
    AVFormatContext* value = nullptr;
    ~Input() { avformat_close_input(&value); }
};
std::uint32_t le(const std::uint8_t* b, unsigned n)
{ std::uint32_t v = 0; for (unsigned i = 0; i < n; ++i) v |= std::uint32_t(b[i]) << (8 * i); return v; }
Sample integer(const juce::var& v)
{
    demand(v.isInt() || v.isInt64(), "Missing integer media coordinate");
    const auto n = static_cast<juce::int64>(v); demand(n >= 0, "Negative media coordinate"); return n;
}
bool hasIdr(const AVPacket& packet, int lengthBytes)
{
    // Recorder's final MP4 contains length-prefixed AVC. Key flags alone do not
    // prove a closed-GOP IDR; inspect the NAL types while doing the one packet scan.
    std::size_t at = 0, size = static_cast<std::size_t>(packet.size);
    bool idr = false;
    while (at < size)
    {
        demand(size - at >= static_cast<std::size_t>(lengthBytes), "Truncated AVC NAL length");
        std::uint32_t n = 0;
        for (int i = 0; i < lengthBytes; ++i) n = (n << 8) | packet.data[at++];
        demand(n > 0 && n <= size - at, "Invalid AVC NAL size");
        idr |= (packet.data[at] & 31) == 5; at += n;
    }
    return idr;
}
WavChunk manifestChunk(const juce::File& root, const juce::var& v)
{
    const auto path = v["path"].toString(); demand(isProjectRelativePath(path), "Invalid WAV relative path");
    WavChunk c; c.file = root.getChildFile(path); c.firstSample = integer(v["firstSample"]);
    c.validSamples = integer(v["validSamples"]); c.validBytes = static_cast<std::uint64_t>(integer(v["validBytes"]));
    if (v.hasProperty("dataOffset")) c.dataOffset = static_cast<std::uint64_t>(integer(v["dataOffset"]));
    return c;
}
}
void VideoIndex::validateAndBuild()
{
    demand(sampleRate > 0 && timeBaseNum > 0 && timeBaseDen > 0 && !packets.empty(), "Invalid video index time base or empty video");
    idrs.clear(); Sample last = -1;
    for (std::size_t i = 0; i < packets.size(); ++i)
    {
        auto& p = packets[i];
        demand(p.pts != AV_NOPTS_VALUE && p.dts == p.pts && p.pts >= startPts && p.offset >= 0 && p.bytes > 0,
               "Playback requires indexed Recorder H.264 with PTS=DTS, valid offsets and no B-frames");
        demand(startPts >= 0 && p.duration > 0 && p.pts - startPts <= (std::numeric_limits<Sample>::max)() - p.duration,
               "Video timestamp range overflows");
        p.sample = av_rescale_q(p.pts - startPts, {timeBaseNum, timeBaseDen}, {1, static_cast<int>(sampleRate)});
        demand(p.sample > last && p.duration > 0, "Non-monotonic or zero-duration video packet");
        p.endSample = av_rescale_q(p.pts - startPts + p.duration, {timeBaseNum, timeBaseDen}, {1, static_cast<int>(sampleRate)});
        demand(p.endSample > p.sample, "Video frame shorter than one timeline sample");
        if (i) demand(packets[i - 1].endSample == p.sample, "Video packet gap/overlap requires an explicit source gap");
        if (p.keyframe && p.idr) idrs.push_back(i);
        last = p.sample;
    }
    demand(packets.front().sample == 0 && !idrs.empty() && idrs.front() == 0, "Video source must begin at an IDR");
    length = packets.back().endSample;
}
std::size_t VideoIndex::frameAt(Sample sample) const
{
    if (sample < 0 || sample >= length || packets.empty()) throw std::out_of_range("Video sample outside source");
    const auto it = std::upper_bound(packets.begin(), packets.end(), sample, [](Sample s, const auto& p) { return s < p.sample; });
    return static_cast<std::size_t>(std::distance(packets.begin(), it) - 1);
}
std::size_t VideoIndex::previousIdr(Sample sample) const
{
    const auto frame = frameAt(sample);
    const auto it = std::upper_bound(idrs.begin(), idrs.end(), frame);
    if (it == idrs.begin()) throw std::runtime_error("No preceding IDR");
    return *std::prev(it);
}
std::shared_ptr<const VideoIndex> MediaIndex::openVideo(const juce::File& file, std::uint32_t Fs) const
{
    demand(Fs > 0 && Fs <= 768000 && file.hasFileExtension("mp4") && !file.getFileName().containsIgnoreCase(".recording."),
           "Open only a finalized .mp4 after rename; growing media is unsupported");
    auto result = std::make_shared<VideoIndex>(); result->epoch = epoch; result->generation = generation();
    result->file = file; result->sampleRate = Fs;
    Input input;
    ffCheck(avformat_open_input(&input.value, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Open final MP4");
    ffCheck(avformat_find_stream_info(input.value, nullptr), "Read MP4 stream info");
    result->stream = av_find_best_stream(input.value, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    ffCheck(result->stream, "Find H.264 stream");
    const auto* stream = input.value->streams[result->stream]; const auto* codec = stream->codecpar;
    demand(codec->codec_id == AV_CODEC_ID_H264 && codec->extradata_size >= 5 && codec->extradata[0] == 1, "Expected MP4 AVC/H.264 configuration");
    const int lengthBytes = (codec->extradata[4] & 3) + 1;
    result->timeBaseNum = stream->time_base.num; result->timeBaseDen = stream->time_base.den;
    result->startPts = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    result->width = codec->width; result->height = codec->height;
    auto packet = ffPacket(); int code = 0;
    while ((code = av_read_frame(input.value, packet.get())) >= 0)
    {
        if (!result->current()) throw std::runtime_error("Media replaced during index scan");
        if (packet->stream_index == result->stream)
            result->packets.push_back({packet->pts, packet->dts, packet->pos, packet->duration, packet->size,
                (packet->flags & AV_PKT_FLAG_KEY) != 0, hasIdr(*packet, lengthBytes)});
        av_packet_unref(packet.get());
    }
    if (code != AVERROR_EOF) ffCheck(code, "Index MP4 packets");
    result->validateAndBuild();
    if (!result->current()) throw std::runtime_error("Media replaced after index scan");
    return result;
}
void MediaIndex::validateWav(WavSource& source)
{
    demand(source.sampleRate > 0 && source.sampleRate <= 768000, "Invalid WAV sample rate");
    std::sort(source.chunks.begin(), source.chunks.end(), [](const auto& a, const auto& b) { return a.firstSample < b.firstSample; });
    Sample end = 0;
    for (const auto& c : source.chunks)
    {
        demand(c.firstSample >= end && c.validSamples > 0 && c.validSamples <= (std::numeric_limits<Sample>::max)() - c.firstSample,
               "Overlapping, empty or overflowing WAV chunk");
        demand(c.dataOffset == 44 && c.validSamples <= (std::numeric_limits<Sample>::max)() / 3, "Unsupported WAV layout");
        const auto bytes = c.dataOffset + static_cast<std::uint64_t>(c.validSamples) * 3;
        demand(c.validBytes >= bytes && c.validBytes <= static_cast<std::uint64_t>((std::max)(juce::int64{0}, c.file.getSize())), "WAV extends beyond durable file watermark");
        juce::FileInputStream input(c.file); std::uint8_t header[44]{};
        demand(input.openedOk() && input.read(header, 44) == 44, "Cannot read WAV header");
        demand(std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header + 8, "WAVEfmt ", 8) == 0
            && le(header + 16, 4) == 16 && le(header + 20, 2) == 1 && le(header + 22, 2) == 1
            && le(header + 24, 4) == source.sampleRate && le(header + 28, 4) == source.sampleRate * 3
            && le(header + 32, 2) == 3 && le(header + 34, 2) == 24 && std::memcmp(header + 36, "data", 4) == 0
            && le(header + 40, 4) >= static_cast<std::uint64_t>(c.validSamples) * 3, "Expected round-06 mono PCM24 WAV header");
        end = c.firstSample + c.validSamples;
    }
    source.length = end;
}
std::vector<std::shared_ptr<const WavSource>> MediaIndex::openWavManifest(const juce::File& directory) const
{
    const auto version = generation();
    juce::var manifest; const auto parsed = juce::JSON::parse(directory.getChildFile("take.json").loadFileAsString(), manifest);
    demand(parsed.wasOk() && manifest.isObject() && integer(manifest["schemaVersion"]) == 1
        && manifest["state"].toString() == "complete", "Expected complete schema-1 playback take.json");
    const auto rate = integer(manifest["sampleRate"]); demand(rate > 0 && rate <= 768000, "Invalid manifest sample rate");
    const auto* tracks = manifest["audioTracks"].getArray(); demand(tracks != nullptr, "Manifest audioTracks missing");
    std::vector<std::shared_ptr<const WavSource>> result;
    for (const auto& track : *tracks)
    {
        auto s = std::make_shared<WavSource>(); s->epoch = epoch; s->generation = version;
        s->trackId = track["trackId"].toString(); s->sampleRate = static_cast<std::uint32_t>(rate);
        demand(s->trackId.isNotEmpty(), "Manifest trackId missing");
        for (const auto& previous : result) demand(previous->trackId != s->trackId, "Duplicate WAV trackId");
        const auto* chunks = track["chunks"].getArray(); demand(chunks != nullptr, "Manifest chunks missing");
        for (const auto& c : *chunks) s->chunks.push_back(manifestChunk(directory, c));
        validateWav(*s); result.push_back(std::move(s));
    }
    if (generation() != version) throw std::runtime_error("WAV media replaced during manifest indexing");
    return result;
}
std::vector<std::shared_ptr<const WavSource>> MediaIndex::openWavJournal(const juce::File& directory, const juce::Uuid& takeId) const
{
    const auto version = generation();
    JournalReplay replay; const auto status = RecordingJournal::replay(directory.getChildFile("journal"), replay);
    if (status.failed()) throw std::runtime_error(status.getErrorMessage().toStdString());
    bool started = false, finalized = false; std::uint32_t rate = 0;
    std::map<juce::String, WavChunk> latest;
    std::map<juce::String, Id> trackIds;
    for (const auto& record : replay.records)
    {
        const auto& p = record.payload;
        if (juce::Uuid(p["takeId"].toString()) != takeId) continue;
        if (record.kind == JournalKind::TakeStarted)
        {
            demand(!started, "Repeated TakeStarted"); started = true;
            const auto pcm = p["pcm"];
            demand(integer(pcm["channels"]) == 1 && integer(pcm["bitsPerSample"]) == 24, "Journal PCM format unsupported");
            const auto fs = integer(pcm["sampleRate"]); demand(fs > 0 && fs <= 768000, "Journal Fs out of range"); rate = static_cast<std::uint32_t>(fs);
            for (const auto& f : *p["files"].getArray())
                trackIds[f["path"].toString().upToLastOccurrenceOf("/", false, false)] = f["assetId"].toString();
        }
        else if (record.kind == JournalKind::Checkpoint)
        {
            demand(started && !finalized, "Checkpoint outside finalized take lifecycle");
            for (const auto& c : *p["files"].getArray())
            {
                demand(integer(c["blockAlign"]) == 3, "Journal block alignment mismatch");
                auto chunk = manifestChunk(directory, c);
                if (chunk.validSamples) latest[c["path"].toString()] = std::move(chunk);
            }
        }
        else if (record.kind == JournalKind::TakeFinalized) { demand(started, "Finalize without start"); finalized = true; }
    }
    demand(finalized, "Take is not finalized; no growing WAV playback in round 11");
    std::map<Id, std::shared_ptr<WavSource>> tracks;
    for (const auto& [path, c] : latest)
    {
        const auto parent = path.upToLastOccurrenceOf("/", false, false); const auto id = trackIds.find(parent);
        demand(id != trackIds.end(), "Journal chunk has no registered microphone");
        auto& s = tracks[id->second];
        if (!s) { s = std::make_shared<WavSource>(); s->epoch = epoch; s->generation = version; s->sampleRate = rate; s->trackId = id->second; }
        s->chunks.push_back(c);
    }
    std::vector<std::shared_ptr<const WavSource>> result;
    for (auto& [id, s] : tracks) { (void) id; validateWav(*s); result.push_back(s); }
    if (generation() != version) throw std::runtime_error("WAV media replaced during journal indexing");
    return result;
}
}
