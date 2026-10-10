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
        demand(size - at >= static_cast<std::size_t>(lengthBytes), "AVC NAL 길이 정보가 잘렸습니다.");
        std::uint32_t n = 0;
        for (int i = 0; i < lengthBytes; ++i) n = (n << 8) | packet.data[at++];
        demand(n > 0 && n <= size - at, "AVC NAL 크기가 올바르지 않습니다.");
        idr |= (packet.data[at] & 31) == 5; at += n;
    }
    return idr;
}
WavChunk manifestChunk(const juce::File& root, const juce::var& v)
{
    const auto path = v["path"].toString(); demand(isProjectRelativePath(path), "WAV의 프로젝트 상대 경로가 올바르지 않습니다.");
    WavChunk c; c.file = root.getChildFile(path); c.firstSample = integer(v["firstSample"]);
    c.validSamples = integer(v["validSamples"]); c.validBytes = static_cast<std::uint64_t>(integer(v["validBytes"]));
    if (v.hasProperty("dataOffset")) c.dataOffset = static_cast<std::uint64_t>(integer(v["dataOffset"]));
    return c;
}
}
void VideoIndex::validateAndBuild()
{
    demand(sampleRate > 0 && timeBaseNum > 0 && timeBaseDen > 0 && !packets.empty(), "영상 색인의 시간 기준이 올바르지 않거나 영상이 비어 있습니다.");
    idrs.clear(); Sample last = -1;
    for (std::size_t i = 0; i < packets.size(); ++i)
    {
        auto& p = packets[i];
        demand(p.pts != AV_NOPTS_VALUE && p.dts == p.pts && p.pts >= startPts && p.offset >= 0 && p.bytes > 0,
               "재생하려면 탤리 H.264 색인의 재생·디코딩 시각과 위치가 올바르고 B프레임이 없어야 합니다.");
        demand(startPts >= 0 && p.duration > 0 && p.pts - startPts <= (std::numeric_limits<Sample>::max)() - p.duration,
               "영상 시각이 처리 가능한 범위를 넘었습니다.");
        p.sample = av_rescale_q(p.pts - startPts, {timeBaseNum, timeBaseDen}, {1, static_cast<int>(sampleRate)});
        demand(p.sample > last && p.duration > 0, "영상 패킷의 시각이 역순이거나 길이가 0입니다.");
        p.endSample = av_rescale_q(p.pts - startPts + p.duration, {timeBaseNum, timeBaseDen}, {1, static_cast<int>(sampleRate)});
        demand(p.endSample > p.sample, "영상 프레임이 타임라인 샘플 1개보다 짧습니다.");
        if (i) demand(packets[i - 1].endSample == p.sample, "영상 패킷이 비거나 겹치는 구간에는 소재 공백 정보가 필요합니다.");
        if (p.keyframe && p.idr) idrs.push_back(i);
        last = p.sample;
    }
    demand(packets.front().sample == 0 && !idrs.empty() && idrs.front() == 0, "영상 소재는 IDR 프레임으로 시작해야 합니다.");
    length = packets.back().endSample;
}
std::size_t VideoIndex::frameAt(Sample sample) const
{
    if (sample < 0 || sample >= length || packets.empty()) throw std::out_of_range("영상 샘플이 소재 범위를 벗어났습니다.");
    const auto it = std::upper_bound(packets.begin(), packets.end(), sample, [](Sample s, const auto& p) { return s < p.sample; });
    return static_cast<std::size_t>(std::distance(packets.begin(), it) - 1);
}
std::size_t VideoIndex::previousIdr(Sample sample) const
{
    const auto frame = frameAt(sample);
    const auto it = std::upper_bound(idrs.begin(), idrs.end(), frame);
    if (it == idrs.begin()) throw std::runtime_error("앞쪽 IDR 프레임이 없습니다.");
    return *std::prev(it);
}
std::pair<std::size_t, std::size_t> VideoIndex::gopAt(Sample sample) const
{
    const auto frame = frameAt(sample);
    const auto next = std::upper_bound(idrs.begin(), idrs.end(), frame);
    if (next == idrs.begin()) throw std::runtime_error("앞쪽 IDR 프레임이 없습니다.");
    return {*std::prev(next), next == idrs.end() ? packets.size() : *next};
}
std::shared_ptr<const VideoIndex> MediaIndex::openVideo(const juce::File& file, std::uint32_t Fs) const
{
    demand(Fs > 0 && Fs <= 768000 && file.hasFileExtension("mp4") && !file.getFileName().containsIgnoreCase(".recording."),
           "마무리된 MP4 파일만 열 수 있습니다. 녹화 중인 소재는 지원하지 않습니다.");
    auto result = std::make_shared<VideoIndex>(); result->epoch = epoch; result->generation = generation();
    result->file = file; result->sampleRate = Fs;
    Input input;
    ffCheckLocalized(avformat_open_input(&input.value, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "마무리된 MP4를 열 수 없습니다.");
    ffCheckLocalized(avformat_find_stream_info(input.value, nullptr), "MP4 스트림 정보를 읽을 수 없습니다.");
    result->stream = av_find_best_stream(input.value, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    ffCheckLocalized(result->stream, "H.264 스트림을 찾을 수 없습니다.");
    const auto* stream = input.value->streams[result->stream]; const auto* codec = stream->codecpar;
    demand(codec->width > 0 && codec->height > 0 && codec->width <= 1920 && codec->height <= 1080
        && codec->width % 2 == 0 && codec->height % 2 == 0, "H.264 재생은 가로·세로가 짝수이고 1920×1080 이하인 소재만 지원합니다.");
    demand(codec->codec_id == AV_CODEC_ID_H264 && codec->extradata_size >= 5 && codec->extradata[0] == 1, "MP4의 AVC/H.264 설정이 올바르지 않습니다.");
    const int lengthBytes = (codec->extradata[4] & 3) + 1;
    result->timeBaseNum = stream->time_base.num; result->timeBaseDen = stream->time_base.den;
    result->startPts = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    result->width = codec->width; result->height = codec->height;
    auto packet = ffPacket(); int code = 0;
    while ((code = av_read_frame(input.value, packet.get())) >= 0)
    {
        if (!result->current()) throw std::runtime_error("색인을 읽는 중 소재가 교체되었습니다.");
        if (packet->stream_index == result->stream)
            result->packets.push_back({packet->pts, packet->dts, packet->pos, packet->duration, packet->size,
                (packet->flags & AV_PKT_FLAG_KEY) != 0, hasIdr(*packet, lengthBytes)});
        av_packet_unref(packet.get());
    }
    if (code != AVERROR_EOF) ffCheckLocalized(code, "MP4 패킷의 색인을 만들 수 없습니다.");
    result->validateAndBuild();
    if (!result->current()) throw std::runtime_error("색인을 읽은 뒤 소재가 교체되었습니다.");
    return result;
}
std::shared_ptr<const WavSource> MediaIndex::recordedAudio(const MediaAsset& asset, const juce::File& folder, unsigned Fs, const Id& track)
{
    demand(asset.kind == AssetKind::mic && asset.mediaGeneration >= 1 && asset.logicalLength >= 0
        && asset.originalFormat.channels >= 1 && asset.originalFormat.channels <= 2, "녹음한 오디오 소재가 올바르지 않습니다.");
    auto wav = std::make_shared<WavSource>(); wav->trackId = track; wav->sampleRate = Fs;
    wav->channels = unsigned(asset.originalFormat.channels);
    wav->epoch = std::make_shared<MediaEpoch>(); wav->generation = std::uint64_t(asset.mediaGeneration); wav->epoch->value = wav->generation;
    const auto add = [&](const juce::String& path, SampleRange range)
    {
        demand(isProjectRelativePath(path) && range.start >= 0 && range.length > 0
            && range.length <= ((std::numeric_limits<Sample>::max)() - 44) / (wav->channels * 3), "녹음한 WAV 조각이 올바르지 않습니다.");
        wav->chunks.push_back({folder.getChildFile(path), range.start, range.length, 44,
            44 + std::uint64_t(range.length) * wav->channels * 3});
    };
    // Failed saves/recovery retain a path even when no PCM is available. In that
    // case keep an empty source: validateWav performs no file I/O without chunks.
    if (!asset.availableRanges.empty())
    {
        if (asset.chunks.empty())
        {
            // A standalone WAV starts at source sample zero. Availability must
            // establish its durable prefix; logicalLength may include a lost tail.
            Sample durable = 0;
            for (const auto range : asset.availableRanges)
            {
                demand(range.start == durable && range.length > 0 && range.length <= asset.logicalLength - durable,
                    "단일 WAV 파일은 처음부터 연속해서 저장된 구간이 있어야 합니다.");
                durable += range.length;
            }
            add(asset.relativePath, {0, durable});
        }
        else for (const auto& chunk : asset.chunks) add(chunk.relativePath, chunk.sourceRange);
    }
    validateWav(*wav);
    demand(wav->length <= asset.logicalLength, "WAV 조각이 녹음한 테이크 범위를 넘었습니다.");
    wav->length = asset.logicalLength; return wav;
}
void MediaIndex::validateWav(WavSource& source)
{
    demand(source.sampleRate > 0 && source.sampleRate <= 768000 && source.channels >= 1 && source.channels <= 2, "WAV 샘플레이트나 채널 수가 올바르지 않습니다.");
    const auto align = source.channels * 3;
    std::sort(source.chunks.begin(), source.chunks.end(), [](const auto& a, const auto& b) { return a.firstSample < b.firstSample; });
    Sample end = 0;
    for (const auto& c : source.chunks)
    {
        demand(c.firstSample >= end && c.validSamples > 0 && c.validSamples <= (std::numeric_limits<Sample>::max)() - c.firstSample,
               "WAV 조각이 겹치거나 비어 있거나 처리 가능한 범위를 넘었습니다.");
        demand(c.dataOffset == 44 && c.validSamples <= (std::numeric_limits<Sample>::max)() / align, "지원하지 않는 WAV 구성입니다.");
        const auto bytes = c.dataOffset + static_cast<std::uint64_t>(c.validSamples) * align;
        demand(c.validBytes >= bytes && c.validBytes <= static_cast<std::uint64_t>((std::max)(juce::int64{0}, c.file.getSize())), "WAV가 저장이 확인된 파일 범위를 넘었습니다.");
        juce::FileInputStream input(c.file); std::uint8_t header[44]{};
        demand(input.openedOk() && input.read(header, 44) == 44, "WAV 헤더를 읽을 수 없습니다.");
        demand(std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header + 8, "WAVEfmt ", 8) == 0
            && le(header + 16, 4) == 16 && le(header + 20, 2) == 1 && le(header + 22, 2) == source.channels
            && le(header + 24, 4) == source.sampleRate && le(header + 28, 4) == source.sampleRate * align
            && le(header + 32, 2) == align && le(header + 34, 2) == 24 && std::memcmp(header + 36, "data", 4) == 0
            && le(header + 40, 4) >= static_cast<std::uint64_t>(c.validSamples) * align, "모노·스테레오 PCM24 WAV 헤더가 필요합니다.");
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
        if (track.hasProperty("channels")) s->channels = unsigned(integer(track["channels"]));
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
    std::map<juce::String, unsigned> trackChannels;
    for (const auto& record : replay.records)
    {
        const auto& p = record.payload;
        if (juce::Uuid(p["takeId"].toString()) != takeId) continue;
        if (record.kind == JournalKind::TakeStarted)
        {
            demand(!started, "Repeated TakeStarted"); started = true;
            const auto pcm = p["pcm"];
            demand((integer(pcm["channels"]) == 1 || integer(pcm["channels"]) == 2) && integer(pcm["bitsPerSample"]) == 24, "Journal PCM format unsupported");
            const auto fs = integer(pcm["sampleRate"]); demand(fs > 0 && fs <= 768000, "Journal Fs out of range"); rate = static_cast<std::uint32_t>(fs);
            for (const auto& f : *p["files"].getArray()) if (f["path"].toString().endsWithIgnoreCase(".wav"))
            {
                const auto parent = f["path"].toString().upToLastOccurrenceOf("/", false, false);
                trackIds[parent] = f["assetId"].toString();
                trackChannels[parent] = unsigned(integer((f.hasProperty("pcm") ? f["pcm"] : pcm)["channels"]));
            }
        }
        else if (record.kind == JournalKind::Checkpoint)
        {
            demand(started && !finalized, "Checkpoint outside finalized take lifecycle");
            for (const auto& c : *p["files"].getArray())
            {
                if (!c["path"].toString().endsWithIgnoreCase(".wav")) continue;
                const auto parent = c["path"].toString().upToLastOccurrenceOf("/", false, false);
                demand(trackChannels.count(parent) && integer(c["blockAlign"]) == trackChannels.at(parent) * 3, "Journal block alignment mismatch");
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
        if (!s) { s = std::make_shared<WavSource>(); s->epoch = epoch; s->generation = version; s->sampleRate = rate; s->channels = trackChannels.at(parent); s->trackId = id->second; }
        s->chunks.push_back(c);
    }
    std::vector<std::shared_ptr<const WavSource>> result;
    for (auto& [id, s] : tracks) { (void) id; validateWav(*s); result.push_back(s); }
    if (generation() != version) throw std::runtime_error("WAV media replaced during journal indexing");
    return result;
}
}
