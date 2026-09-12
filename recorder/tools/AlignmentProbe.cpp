#include "model/RecorderSerializer.h"
#include "media/ThumbnailCache.h"
#include "playback/TimelineTransport.h"
#include "record/Ffmpeg.h"
#include "storage/Mp4RecoveryIndex.h"
#include "support/Platform.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace gocue::recorder
{
namespace
{
void need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void waitFor(F ready)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!ready())
    {
        need(std::chrono::steady_clock::now() < end, "Alignment decode/prepare timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Decoded
{
    std::mutex mutex;
    std::map<std::int64_t, std::int64_t> timestamps;
};
// Real software H.264 decoding behind the engine's existing decoder seam.
// No capture, audio device, GPU texture, window or claimed Present receipt.
class SoftwareDecoder final : public IVideoFrameDecoder
{
public:
    SoftwareDecoder(std::shared_ptr<const VideoIndex> index, std::shared_ptr<Decoded> result)
        : source(std::move(index)), decoded(std::move(result))
    {
        ffCheck(avformat_open_input(&input, source->file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Open alignment video");
        ffCheck(avformat_find_stream_info(input, nullptr), "Alignment streams");
        const auto* codec = avcodec_find_decoder(input->streams[source->stream]->codecpar->codec_id);
        context.reset(avcodec_alloc_context3(codec)); need(bool(context), "Allocate alignment decoder");
        ffCheck(avcodec_parameters_to_context(context.get(), input->streams[source->stream]->codecpar), "Alignment codec parameters");
        context->thread_count = 1;
        ffCheck(avcodec_open2(context.get(), codec, nullptr), "Open alignment software decoder");
    }
    ~SoftwareDecoder() override { avformat_close_input(&input); }
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t target, const std::function<bool()>& cancelled) override
    {
        const auto expected = source->packets.at(target).pts;
        const auto idr = source->previousIdr(source->packets[target].sample);
        ffCheck(av_seek_frame(input, source->stream, source->packets[idr].pts, AVSEEK_FLAG_BACKWARD), "Alignment IDR seek");
        avcodec_flush_buffers(context.get());
        auto packet = ffPacket(); auto frame = ffFrame();
        while (!cancelled())
        {
            const auto read = av_read_frame(input, packet.get());
            if (read < 0 && read != AVERROR_EOF) ffCheck(read, "Read alignment packet");
            if (read == AVERROR_EOF || packet->stream_index == source->stream)
            {
                ffCheck(avcodec_send_packet(context.get(), read == AVERROR_EOF ? nullptr : packet.get()), "Decode alignment packet");
                while (avcodec_receive_frame(context.get(), frame.get()) == 0)
                {
                    if (frame->best_effort_timestamp < expected) continue;
                    need(frame->best_effort_timestamp == expected, "Decoder skipped the indexed alignment PTS");
                    const std::lock_guard<std::mutex> lock(decoded->mutex);
                    decoded->timestamps[expected] = frame->best_effort_timestamp;
                    return {};
                }
            }
            av_packet_unref(packet.get());
            if (read == AVERROR_EOF) break;
        }
        need(cancelled(), "Alignment source ended before the requested frame");
        return {};
    }
private:
    std::shared_ptr<const VideoIndex> source;
    std::shared_ptr<Decoded> decoded;
    AVFormatContext* input = nullptr;
    struct FreeCodec { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
    std::unique_ptr<AVCodecContext, FreeCodec> context;
};
class SyntheticOutput final : public IAudioOutput
{
public:
    AudioOutputInfo open(const AudioOutputConfig&) override { return {"synthetic", 48000, 480, 1536, 0, 1}; }
    void start(IAudioOutputClient&) override {}
    void close() noexcept override {}
    Sample latestOutputSample() const noexcept override { return sample; }
    juce::Result status() const override { return juce::Result::ok(); }
    void drainTiming() override {}
    Sample sample = 0;
};
RenderClip mapping(const Clip& clip, const MediaAsset& asset)
{
    RenderClip r; r.clipId = clip.clipId; r.assetId = clip.assetId; r.trackId = clip.trackId;
    r.sourceIn = clip.sourceIn; r.timelineStartSample = clip.timelineStartSample;
    r.lengthSamples = clip.lengthSamples; r.mediaGeneration = asset.mediaGeneration; return r;
}
float rawSample(const WavSource& source, Sample at, unsigned channel)
{
    for (const auto& chunk : source.chunks) if (at >= chunk.firstSample && at < chunk.firstSample + chunk.validSamples)
    {
        juce::FileInputStream input(chunk.file); unsigned char bytes[3]{};
        need(input.openedOk() && input.setPosition(juce::int64(chunk.dataOffset) + ((at - chunk.firstSample) * source.channels + channel) * 3)
            && input.read(bytes, 3) == 3, "Read independent PCM24 sample");
        auto pcm = std::int32_t(bytes[0]) | (std::int32_t(bytes[1]) << 8) | (std::int32_t(bytes[2]) << 16);
        if (pcm & 0x800000) pcm -= 0x1000000;
        return float(pcm) / 8388608.0f;
    }
    return 0;
}
}
int runAlignmentProbe(int argc, wchar_t** argv)
{
    try
    {
        need(argc == 6 && juce::String(argv[2]) == "--project" && juce::String(argv[4]) == "--report",
            "RecorderProbe alignment --project project.recorder --report report.json");
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[3]));
        const auto out = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[5]));
        RecorderProject project;
        const auto read = RecorderSerializer::readCheckpoint(file, project, nullptr, false);
        if (read.failed()) throw std::runtime_error(read.getErrorMessage().toStdString());
        auto report = jsonObject(); jsonSet(report, "project", file.getFullPathName());
        jsonSet(report, "definition", "Real MP4 software decode through VideoPlaybackEngine, independent PCM24 byte reads and synthetic output callbacks. No hardware or Present measurement. Project is read only.");
        jsonSet(report, "Fs", project.Fs); jsonSet(report, "timelineEnd", project.activeTimelineEnd());
        const auto compiled = RenderPlanCompiler::compile(project);
        juce::Array<juce::var> clips, seeks, audioRows, moving;
        MediaIndex index; std::vector<PlaybackVideoClip> videos;
        for (const auto& track : project.tracks) for (const auto& clip : track.clips.items()) if (project.isActive(clip))
        {
            const auto* asset = project.media->findAsset(clip.assetId); need(asset != nullptr, "Missing alignment asset");
            auto c = jsonObject(); jsonSet(c, "clipId", clip.clipId); jsonSet(c, "sourceIn", clip.sourceIn);
            jsonSet(c, "timelineStart", clip.timelineStartSample); jsonSet(c, "length", clip.lengthSamples);
            jsonSet(c, "assetLength", asset->logicalLength);
            if (asset->kind == AssetKind::camera)
            {
                const auto source = index.openVideo(file.getParentDirectory().getChildFile(asset->relativePath), project.Fs);
                jsonSet(c, "startPts", source->startPts); jsonSet(c, "indexLength", source->length);
                jsonSet(c, "packetCount", int(source->packets.size()));
                const auto verified = Mp4RecoveryIndex::decode(source->file, project.Fs);
                jsonSet(c, "recoveryDecodedFrames", verified.videoFrames); jsonSet(c, "recoveryVideoSamples", verified.samples);
                jsonSet(c, "recoveryDecodedAacSamples", verified.audioSamples);
                auto decoded = std::make_shared<Decoded>();
                VideoPlaybackEngine engine([decoded](auto v) { return std::make_unique<SoftwareDecoder>(std::move(v), decoded); });
                const PlaybackVideoClip vc{mapping(clip, *asset), 0, source}; engine.prepare({vc});
                if (track.kind == TrackKind::cam1) videos.push_back(vc);
                std::vector<Sample> points{0, 1, 399, 400, 799, 800, 801, 23999, 24000, 24321, 47999, 48000, 63840,
                    clip.lengthSamples / 2 + 123, clip.lengthSamples - 801, clip.lengthSamples - 1, clip.lengthSamples};
                std::sort(points.begin(), points.end()); points.erase(std::unique(points.begin(), points.end()), points.end());
                std::uint64_t generation = 0;
                for (const auto delta : points) if (delta >= 0 && delta <= clip.lengthSamples)
                {
                    const auto t = clip.timelineStartSample + delta, u = clip.sourceIn + delta;
                    engine.seek(t, ++generation);
                    waitFor([&] { if (engine.status().failed()) throw std::runtime_error(engine.status().getErrorMessage().toStdString()); return engine.ready(t, generation); });
                    const auto selected = engine.displaySelection(0);
                    auto row = jsonObject(); jsonSet(row, "clipId", clip.clipId); jsonSet(row, "timelineSample", t);
                    jsonSet(row, "sourceSample", u); jsonSet(row, "gap", selected.gap);
                    if (selected.frame && !selected.gap)
                    {
                        const auto& f = *selected.frame;
                        const auto sourceStart = av_rescale_q(f.pts - source->startPts, {source->timeBaseNum, source->timeBaseDen}, {1, int(project.Fs)});
                        const auto expected = source->frameAt(u);
                        jsonSet(row, "pts", f.pts); jsonSet(row, "sourceFrameSample", sourceStart);
                        jsonSet(row, "frameBegin", f.begin); jsonSet(row, "frameEnd", f.end);
                        jsonSet(row, "expectedFrame", int(expected)); jsonSet(row, "videoFrameError", int(source->frameAt(sourceStart)) - int(expected));
                        jsonSet(row, "frameStartMinusRequestMs", 1000.0 * (sourceStart - u) / project.Fs);
                        { const std::lock_guard<std::mutex> lock(decoded->mutex); jsonSet(row, "decodedPts", decoded->timestamps.at(f.pts)); }
                        const auto thumbs = ThumbnailCache::decodePoints(source->file, project.Fs, {u}, [] { return false; });
                        jsonSet(row, "uiFrameRequest", ThumbnailCache::frameSample(u, project.Fs, asset->originalFormat.fps));
                        jsonSet(row, "thumbnailSample", thumbs.empty() ? juce::var() : juce::var(thumbs[0].sample));
                        jsonSet(row, "thumbnailEnd", thumbs.empty() ? juce::var() : juce::var(thumbs[0].endSample));
                        if (!thumbs.empty()) jsonSet(row, "thumbnailMinusVideoMs", 1000.0 * (thumbs[0].sample - sourceStart) / project.Fs);
                        // Preserve the original UI policy in the report for comparison.
                        const auto bucket = u / (project.Fs / 2) * (project.Fs / 2);
                        jsonSet(row, "legacyHalfSecondRequest", bucket);
                        jsonSet(row, "legacyHalfSecondFrameError", int(source->frameAt(bucket)) - int(expected));
                        for (const auto& lane : compiled->tracks) if (lane.trackId == track.trackId)
                            for (const auto& span : lane.spans) if (!span.isGap() && t >= span.timeline.start && t < span.timeline.start + span.timeline.length)
                                jsonSet(row, "compiledSourceFrame", RenderPlanCompiler::sourceUnitAt(span, t, true));
                    }
                    seeks.add(row);
                }
            }
            else if (asset->kind == AssetKind::mic)
            {
                const auto source = MediaIndex::recordedAudio(*asset, file.getParentDirectory(), project.Fs, track.trackId);
                PlaybackAudioTrack lane; lane.trackId = track.trackId; lane.clips.push_back({mapping(clip, *asset), source});
                TimelineAudioRenderer renderer(project.Fs, 480); renderer.setPlan({lane}, clip.timelineEnd());
                std::uint64_t compared = 0, mismatches = 0;
                const std::vector<Sample> points{0, 1, 799, 800, 23999, 24321, 48000, 63840, clip.lengthSamples / 2 + 123, clip.lengthSamples - 1, clip.lengthSamples};
                for (const auto delta : points) if (delta >= 0 && delta <= clip.lengthSamples)
                {
                    const auto t = clip.timelineStartSample + delta, u = clip.sourceIn + delta; float l = 0, r = 0;
                    renderer.renderAudio(t, 1, &l, &r);
                    const auto expected = delta == clip.lengthSamples ? 0.0f : rawSample(*source, u, 0);
                    auto row = jsonObject(); jsonSet(row, "timelineSample", t); jsonSet(row, "sourceSample", u);
                    jsonSet(row, "renderedLeft", double(l)); jsonSet(row, "rawLeft", double(expected)); jsonSet(row, "pcmEqual", l == expected);
                    jsonSet(row, "mappingErrorMs", 1000.0 * ((clip.sourceIn + t - clip.timelineStartSample) - u) / project.Fs);
                    ++compared; if (l != expected) ++mismatches; audioRows.add(row);
                }
                jsonSet(c, "wavLength", source->length); jsonSet(c, "pcmCompared", compared); jsonSet(c, "pcmMismatches", mismatches);
            }
            clips.add(c);
        }
        if (!videos.empty())
        {
            auto decoded = std::make_shared<Decoded>();
            VideoPlaybackEngine engine([decoded](auto v) { return std::make_unique<SoftwareDecoder>(std::move(v), decoded); });
            engine.prepare(videos);
            TimelineAudioRenderer renderer(project.Fs, 480); renderer.setPlan(std::vector<PlaybackAudioTrack>{}, project.activeTimelineEnd());
            // A silent synthetic sink exercises the production audible-cursor coordinator.
            constexpr std::int64_t hz = 48000000;
            TimelineTransport transport(project.Fs, hz, renderer.queue(), project.activeTimelineEnd()); SyntheticOutput output;
            transport.seek(videos.front().mapping.timelineStartSample); transport.play();
            BlockStamp stamp{}; stamp.flags = samplePositionValid | latenciesValid;
            stamp.sampleRate = project.Fs; stamp.numSamples = 480; stamp.outputLatencySamples = 1536;
            float l[480]{}, r[480]{};
            for (int block = 0; block < 40; ++block)
            {
                stamp.samplePosition = output.sample; stamp.sequence = std::uint64_t(block);
                stamp.callbackQpc = hz + output.sample * hz / project.Fs;
                transport.processOutput(stamp, l, r); output.sample += 480;
                transport.service(renderer, engine, output, stamp.callbackQpc);
                if (transport.snapshot().state == TransportState::preparing)
                    waitFor([&] { transport.service(renderer, engine, output, stamp.callbackQpc); return renderer.ready() && engine.ready(transport.snapshot().frozenSample, transport.generation()); });
                need(transport.status().wasOk(), "Synthetic transport failed");
                const auto state = transport.snapshot();
                if (state.state != TransportState::playing) continue;
                const auto target = Sample(engine.telemetry()["cameras"][0]["targetSample"]);
                waitFor([&] { return engine.ready(target, transport.generation()); });
                const auto audible = TimelineTransport::audibleCursor(state, project.Fs, hz, stamp.callbackQpc);
                const auto selected = engine.displaySelection(0); auto row = jsonObject();
                jsonSet(row, "audibleSample", audible); jsonSet(row, "videoRequestSample", target);
                jsonSet(row, "videoRequestMinusAudibleMs", 1000.0 * (target - audible) / project.Fs);
                jsonSet(row, "outputLatencySamples", stamp.outputLatencySamples);
                if (selected.frame) { jsonSet(row, "videoBegin", selected.frame->begin); jsonSet(row, "videoEnd", selected.frame->end); jsonSet(row, "videoContainsAudible", selected.frame->begin <= audible && audible < selected.frame->end); }
                moving.add(row);
            }
        }
        jsonSet(report, "clips", clips); jsonSet(report, "seeks", seeks); jsonSet(report, "audio", audioRows); jsonSet(report, "moving", moving);
        need(out.getParentDirectory().createDirectory().wasOk() && out.replaceWithText(juce::JSON::toString(report, true)), "Write alignment report");
        std::cout << "Alignment measurements: " << out.getFullPathName() << '\n'; return 0;
    }
    catch (const std::exception& e) { std::cerr << "Alignment probe: " << e.what() << '\n'; return 1; }
}
}
