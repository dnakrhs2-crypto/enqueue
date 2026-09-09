#include "TestSupport.h"
#include "media/MediaIndex.h"
#include "playback/TimelineAudioRenderer.h"
#include "playback/TimelineTransport.h"
#include "playback/VideoPlaybackEngine.h"
#include "record/WavTrackWriter.h"
#include "storage/StorageEncoding.h"
#include "support/Platform.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <thread>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
template<class F> void eventually(F check)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!check())
    {
        require(std::chrono::steady_clock::now() < deadline, "Asynchronous playback test timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Temp
{
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-playback-test-" + juce::Uuid().toString());
    Temp() { require(root.createDirectory().wasOk(), "Create test fixture directory"); }
    ~Temp()
    {
        // Only our own resolved temporary child can be recursively removed.
        if (root.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory)
            && root.getFileName().startsWith("recorder-playback-test-")) root.deleteRecursively();
    }
};
std::shared_ptr<VideoIndex> videoIndex(unsigned frames = 180)
{
    auto v = std::make_shared<VideoIndex>(); v->epoch = std::make_shared<MediaEpoch>(); v->width = 1920; v->height = 1080;
    for (unsigned i = 0; i < frames; ++i) v->packets.push_back({i, i, 100 + i * 1000, 1, 1000, i % 60 == 0, i % 60 == 0});
    v->validateAndBuild(); return v;
}
PlaybackVideoClip videoClip(std::shared_ptr<const VideoIndex> index, Sample start, Sample in, Sample length, unsigned camera = 0)
{
    PlaybackVideoClip clip; clip.camera = camera; clip.source = std::move(index);
    clip.mapping.clipId = newId(); clip.mapping.mediaGeneration = static_cast<Sample>(clip.source->generation);
    clip.mapping.timelineStartSample = start; clip.mapping.sourceIn = in; clip.mapping.lengthSamples = length; return clip;
}
struct StubState
{
    std::atomic<unsigned> opens{0}, decodes{0};
    std::atomic<bool> delay{false}, entered{false}, release{false};
};
class StubDecoder final : public IVideoFrameDecoder
{
public:
    explicit StubDecoder(std::shared_ptr<StubState> s) : shared(std::move(s)) { ++shared->opens; }
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t, const std::function<bool()>& cancelled) override
    {
        ++shared->decodes;
        if (shared->delay.exchange(false))
        {
            shared->entered.store(true);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!shared->release.load() && !cancelled() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {}; // synthetic display metadata, never a fake successful GPU Present
    }
private:
    std::shared_ptr<StubState> shared;
};
VideoPlaybackEngine::DecoderFactory factory(const std::shared_ptr<StubState>& state)
{ return [state](auto) { return std::make_unique<StubDecoder>(state); }; }
WavChunk wav(const juce::File& file, Sample first, const std::vector<std::int32_t>& pcm, Sample valid)
{
    using storageEncoding::put;
    std::array<std::uint8_t, 44> h{}; const auto bytes = static_cast<std::uint32_t>(pcm.size() * 3);
    std::memcpy(h.data(), "RIFF", 4); put(h.data() + 4, 36u + bytes + (bytes & 1u));
    std::memcpy(h.data() + 8, "WAVEfmt ", 8); put(h.data() + 16, 16u); put(h.data() + 20, std::uint16_t{1}); put(h.data() + 22, std::uint16_t{1});
    put(h.data() + 24, 48000u); put(h.data() + 28, 144000u); put(h.data() + 32, std::uint16_t{3}); put(h.data() + 34, std::uint16_t{24});
    std::memcpy(h.data() + 36, "data", 4); put(h.data() + 40, bytes);
    juce::FileOutputStream out(file); require(out.openedOk(), "Open fixture WAV"); require(out.write(h.data(), h.size()), "Write WAV header");
    for (const auto p : pcm) { std::uint8_t b[3]; require(WavTrackWriter::packPcm24(p, b), "Pack fixture PCM24"); require(out.write(b, 3), "Write PCM24"); }
    if (bytes & 1u) out.writeByte(0); out.flush(); require(out.getStatus().wasOk(), "Flush fixture WAV");
    return {file, first, valid, 44, 44 + static_cast<std::uint64_t>(valid) * 3};
}
std::shared_ptr<WavSource> wavSource(const Temp& temp)
{
    auto source = std::make_shared<WavSource>(); source->epoch = std::make_shared<MediaEpoch>(); source->trackId = "mic1";
    source->chunks.push_back(wav(temp.root.getChildFile("a.wav"), 0, {8388607, -8388608, 4194304, 123456, 654321}, 3));
    source->chunks.push_back(wav(temp.root.getChildFile("b.wav"), 3, {-4194304, 2097152, -2097152}, 3));
    source->chunks.push_back(wav(temp.root.getChildFile("c.wav"), 8, {1048576, -1048576}, 2));
    MediaIndex::validateWav(*source); return source;
}
PlaybackAudioTrack audioTrack(std::shared_ptr<const WavSource> source, Sample start = 0, Sample in = 0, Sample length = 10)
{
    PlaybackAudioTrack track; track.trackId = source->trackId;
    PlaybackAudioClip c; c.source = std::move(source); c.mapping.clipId = newId();
    c.mapping.timelineStartSample = start; c.mapping.sourceIn = in; c.mapping.lengthSamples = length; c.mapping.mediaGeneration = static_cast<Sample>(c.source->generation);
    track.clips.push_back(std::move(c)); return track;
}
BlockStamp stamp(Sample output, unsigned frames = 100, unsigned rate = 1000, int latency = 20)
{
    BlockStamp s{}; s.flags = samplePositionValid | latenciesValid; s.samplePosition = output;
    s.numSamples = frames; s.sampleRate = rate; s.outputLatencySamples = latency;
    s.callbackQpc = 1000000 + output * 1000; s.sequence = static_cast<std::uint64_t>(output / frames); return s;
}
class StubOutput final : public IAudioOutput
{
public:
    AudioOutputInfo open(const AudioOutputConfig&) override { return {"stub", 1000, 100, 150, 0, 1}; }
    void start(IAudioOutputClient& c) override { client = &c; }
    void close() noexcept override { client = nullptr; }
    Sample latestOutputSample() const noexcept override { return sample; }
    juce::Result status() const override { return juce::Result::ok(); }
    void drainTiming() override {}
    void tick() { current = stamp(sample, 100, 1000, 150); client->processOutput(current, left, right); sample += 100; }
    IAudioOutputClient* client = nullptr;
    Sample sample = 0;
    BlockStamp current{};
    float left[100]{}, right[100]{};
};
}
int runPlaybackTests()
{
    Suite suite;
    suite.test("packet index builds closed-GOP IDR lookup and floors exact frame containment", []
    {
        const auto v = videoIndex(); require(v->length == 144000 && v->idrs.size() == 3, "Index length/IDR count");
        require(v->frameAt(799) == 0 && v->frameAt(800) == 1 && v->frameAt(47999) == 59, "Frame boundary floor");
        require(v->previousIdr(47999) == 0 && v->previousIdr(48000) == 60 && v->previousIdr(95999) == 60, "Previous IDR selection");
        rejects([&] { v->frameAt(v->length); }); rejects([&] { v->frameAt(-1); });
    });
    suite.test("invalid video packet layout, open GOP, reorder and growing filename are rejected", []
    {
        auto v = videoIndex(); v->packets[1].dts = 0; rejects([&] { v->validateAndBuild(); });
        v = videoIndex(); v->packets.front().idr = false; rejects([&] { v->validateAndBuild(); });
        v = videoIndex(); v->packets[60].offset = -1; rejects([&] { v->validateAndBuild(); });
        v = videoIndex(); v->packets[5].duration = 2; rejects([&] { v->validateAndBuild(); });
        MediaIndex index; rejects([&] { index.openVideo(juce::File::getCurrentWorkingDirectory().getChildFile("cam1.recording.mp4"), 48000); });
    });
    suite.test("WAV chunk boundaries, committed tail and explicit gaps render one virtual source", []
    {
        Temp temp; const auto source = wavSource(temp); TimelineAudioRenderer renderer(48000, 512);
        renderer.setPlan({audioTrack(source)}, 10); float l[10], r[10]; renderer.renderAudio(0, 10, l, r);
        const float expected[]{8388607.0f / 8388608.0f, -1, .5f, -.5f, .25f, -.25f, 0, 0, .125f, -.125f};
        for (int i = 0; i < 10; ++i) require(l[i] == expected[i] && r[i] == expected[i], "WAV virtual-source PCM mismatch");
        renderer.setPlan({audioTrack(source, 3, 2, 4)}, 9); float a[9], b[9]; renderer.renderAudio(0, 9, a, b);
        require(a[2] == 0 && a[3] == .5f && a[4] == -.5f && a[6] == -.25f && a[7] == 0, "Clip sourceIn/timeline mapping");
    });
    suite.test("mute/solo mean uses selected tracks including silent gaps and empty tracks", []
    {
        Temp temp; const auto source = wavSource(temp); auto first = audioTrack(source);
        PlaybackAudioTrack empty; empty.trackId = "silent";
        TimelineAudioRenderer renderer(48000, 512); float l[10], r[10];
        renderer.setPlan({first, empty}, 10); renderer.renderAudio(0, 10, l, r); require(l[2] == .25f, "Empty track still contributes to fixed K");
        first.solo = true; renderer.setPlan({first, empty}, 10); renderer.renderAudio(0, 10, l, r); require(l[2] == .5f, "Solo selection");
        first.mute = true; renderer.setPlan({first, empty}, 10); renderer.renderAudio(0, 10, l, r);
        for (const auto x : l) require(x == 0, "Zero selected tracks must be silent");
    });
    suite.test("WAV format/watermark errors and stale file generations cannot render", []
    {
        Temp temp; const auto source = wavSource(temp); auto broken = *source;
        broken.chunks[0].validBytes = 44; rejects([&] { MediaIndex::validateWav(broken); });
        broken = *source; broken.chunks[1].firstSample = 2; rejects([&] { MediaIndex::validateWav(broken); });
        broken = *source; broken.sampleRate = 44100; rejects([&] { MediaIndex::validateWav(broken); });
        TimelineAudioRenderer renderer(48000, 512); renderer.setPlan({audioTrack(source)}, 10);
        source->epoch->value.fetch_add(1); float l[10], r[10]; rejects([&] { renderer.renderAudio(0, 10, l, r); });
    });
    suite.test("round-06 committed journal opens only finalized WAV generations across 30s chunks", []
    {
        Temp temp; WavTrackWriter::Config config; config.projectDirectory = temp.root; config.takeId = juce::Uuid();
        config.sampleRate = 100; config.mics = 1; config.framesPerBlock = 100;
        config.devices = {{"stub", "test", 1, 0, 0}};
        WavTrackWriter writer(config); require(writer.start().wasOk(), "Start offline WAV writer");
        MediaIndex index; rejects([&] { index.openWavJournal(temp.root, config.takeId); });
        std::int32_t pcm[100]; std::fill_n(pcm, 100, 4194304);
        for (unsigned at = 0; at < 3100; at += 100)
        {
            eventually([&] { return writer.queueFrames() + 100 <= writer.queueCapacityFrames(); });
            require(writer.tryPush(pcm, 100, at), "Push fixture PCM");
        }
        require(writer.stop(3100, juce::Uuid()).wasOk(), "Finalize fixture WAV");
        const auto sources = index.openWavJournal(temp.root, config.takeId);
        require(sources.size() == 1 && sources[0]->chunks.size() == 2 && sources[0]->length == 3100, "Journal chunk ranges");
        TimelineAudioRenderer renderer(100, 100); renderer.setPlan({audioTrack(sources[0], 0, 0, 3100)}, 3100);
        float l[20], r[20]; renderer.renderAudio(2990, 20, l, r); for (const auto x : l) require(x == .5f, "30s chunk transition introduced silence");
        index.invalidate(); require(!sources[0]->current(), "File generation invalidation");
    });
    suite.test("take.json schema preserves watermark and denies unknown formats/unsafe paths", []
    {
        Temp temp; const auto source = wavSource(temp); auto root = jsonObject(); auto track = jsonObject();
        jsonSet(root, "schemaVersion", 1); jsonSet(root, "state", "complete"); jsonSet(root, "sampleRate", 48000); jsonSet(track, "trackId", "mic1");
        juce::Array<juce::var> chunks;
        for (const auto& c : source->chunks)
        {
            auto j = jsonObject(); jsonSet(j, "path", c.file.getFileName()); jsonSet(j, "firstSample", c.firstSample);
            jsonSet(j, "validSamples", c.validSamples); jsonSet(j, "validBytes", c.validBytes); chunks.add(j);
        }
        jsonSet(track, "chunks", chunks); jsonSet(root, "audioTracks", juce::Array<juce::var>{track});
        require(temp.root.getChildFile("take.json").replaceWithText(juce::JSON::toString(root)), "Write manifest");
        MediaIndex index; const auto opened = index.openWavManifest(temp.root);
        require(opened.size() == 1 && opened[0]->length == 10, "Manifest range import"); index.invalidate(); require(!opened[0]->current(), "Manifest generation invalidation");
        jsonSet(chunks.getReference(0), "path", "../escape.wav"); jsonSet(track, "chunks", chunks);
        jsonSet(root, "audioTracks", juce::Array<juce::var>{track});
        require(temp.root.getChildFile("take.json").replaceWithText(juce::JSON::toString(root)), "Write bad manifest"); rejects([&] { index.openWavManifest(temp.root); });
    });
    suite.test("PCM queue is transactional on underrun, supports split callback blocks and generation fencing", []
    {
        PlaybackPcmQueue queue(48000, 4); const float a[]{1,2,3,4}, b[]{5,6,7,8}; float l[8]{}, r[8]{};
        require(queue.push(10, 7, a, a, 4), "Queue first block");
        require(!queue.consume(10, 7, l, r, 5) && queue.queuedFrames() == 4, "Underrun consumed partial data");
        require(!queue.consume(10, 6, l, r, 4), "Stale generation consumed");
        require(queue.consume(10, 7, l, r, 2) && l[0] == 1 && l[1] == 2, "Partial callback consume");
        require(queue.push(14, 7, b, b, 4), "Queue second block");
        require(queue.consume(12, 7, l, r, 6), "Cross-block consume");
        for (int i = 0; i < 6; ++i) require(l[i] == float(i + 3) && l[i] == r[i], "Queue sample discontinuity");
        require(queue.queuedFrames() == 0, "Queue not empty");
    });
    suite.test("PCM SPSC slots cannot be overwritten while callback owns partial data", []
    {
        PlaybackPcmQueue queue(1000, 16); std::atomic<bool> failure{false};
        std::thread producer([&]
        {
            for (int at = 0; at < 16000; at += 16)
            {
                float pcm[16]; for (int i = 0; i < 16; ++i) pcm[i] = float(at + i);
                while (!queue.push(at, 9, pcm, pcm, 16)) std::this_thread::yield();
            }
        });
        for (int at = 0; at < 16000; at += 5)
        {
            float l[5], r[5]; const auto count = static_cast<unsigned>((std::min)(5, 16000 - at));
            while (!queue.consume(at, 9, l, r, count)) std::this_thread::yield();
            for (unsigned i = 0; i < count; ++i) if (l[i] != at + float(i) || r[i] != l[i]) failure.store(true);
        }
        producer.join(); require(!failure.load() && queue.queuedFrames() == 0, "SPSC PCM overwritten or duplicated");
    });
    suite.test("worker prepares at least 250ms, short take prepares only valid tail", []
    {
        TimelineAudioRenderer longTake(48000, 512); longTake.setPlan({}, 48000); longTake.prepare(0, 1);
        eventually([&] { return longTake.ready(); }); require(longTake.queue().queuedFrames() >= 12000, "Read-ahead below 250ms"); longTake.stopWorker();
        TimelineAudioRenderer shortTake(48000, 512); shortTake.setPlan({}, 50); shortTake.prepare(0, 2);
        eventually([&] { return shortTake.ready(); }); require(shortTake.queue().queuedFrames() == 50, "Short take prepared padded source samples");
    });
    suite.test("audible cursor subtracts software queue plus output latency and interpolates QPC", []
    {
        TransportSnapshot s; s.state = TransportState::playing; s.timelineOrigin = 1000; s.outputOrigin = 5000;
        s.submittedEnd = 10000; s.renderedEnd = 20000; s.softwareQueuedSamples = 10000; s.outputSample = 13500;
        s.outputLatency = 960; s.blockFrames = 480; s.callbackQpc = 1000000;
        require(TimelineTransport::audibleCursor(s, 48000, 1000000, 1000000) == 8540, "Queued/submitted cursor mistaken for audible cursor");
        require(TimelineTransport::audibleCursor(s, 48000, 1000000, 1005000) == 8780, "QPC interpolation");
        require(TimelineTransport::audibleCursor(s, 48000, 1000000, 1005000, 5000) == 9020, "Expected display-time selection");
        s.renderedEnd += 999999; s.softwareQueuedSamples += 999999;
        require(TimelineTransport::audibleCursor(s, 48000, 1000000, 1005000) == 8780, "Worker read-ahead altered audible clock");
        require(TimelineTransport::audibleCursor(s, 48000, 1000000, 999999999) <= s.submittedEnd, "Cursor ran beyond submitted PCM");
    });
    suite.test("ASIO adopts exact reserved start sample; late preparation and underrun output silence", []
    {
        PlaybackPcmQueue queue(1000, 100); TimelineTransport transport(1000, 1000000, queue, 1000);
        float l[100], r[100], pcm[100]; std::fill_n(pcm, 100, 1.0f);
        transport.seek(10); transport.processOutput(stamp(1000), l, r);
        require(transport.snapshot().state == TransportState::preparing, "Seek not acknowledged at callback");
        require(queue.push(10, transport.generation(), pcm, pcm, 100), "Push reserved PCM"); transport.prepared(1150, true);
        transport.processOutput(stamp(1100), l, r);
        for (int i = 0; i < 50; ++i) require(l[i] == 0, "Started before reserved ASIO sample");
        require(l[52] == 1 && transport.snapshot().submittedEnd == 60, "Partial ASIO block start offset");
        transport.processOutput(stamp(1200), l, r);
        require(transport.snapshot().state == TransportState::buffering && transport.snapshot().underruns == 1
            && transport.snapshot().submittedEnd == 60 && queue.queuedFrames() == 50, "Underrun must stop transactionally");
        for (const auto x : l) require(x == 0, "Underrun emitted a partial block");
    });
    suite.test("common transport drains accepted tail, reparses readiness and restarts after underrun", []
    {
        TimelineAudioRenderer audio(1000, 100); audio.setPlan({}, 10000);
        auto state = std::make_shared<StubState>(); VideoPlaybackEngine video(factory(state)); video.prepare({});
        TimelineTransport transport(1000, 1000000, audio.queue(), 10000); StubOutput output; output.start(transport);
        transport.seek(0); transport.play(); output.tick(); transport.service(audio, video, output, output.current.callbackQpc);
        eventually([&] { return audio.ready(); }); transport.service(audio, video, output, output.current.callbackQpc);
        for (int i = 0; i < 8 && transport.snapshot().state != TransportState::playing; ++i) output.tick();
        require(transport.snapshot().state == TransportState::playing, "Prepared streams did not start");
        audio.stopWorker();
        for (int i = 0; i < 20 && transport.snapshot().state != TransportState::buffering; ++i) output.tick();
        const auto before = transport.snapshot(); require(before.state == TransportState::buffering, "Starved stream did not buffer");
        const auto oldGeneration = transport.generation();
        for (int i = 0; i < 10 && transport.generation() == oldGeneration; ++i)
        { output.tick(); transport.service(audio, video, output, output.current.callbackQpc); }
        require(transport.generation() > oldGeneration, "Underrun did not trigger common reprepare");
        output.tick(); transport.service(audio, video, output, output.current.callbackQpc);
        eventually([&] { return audio.ready(); }); transport.service(audio, video, output, output.current.callbackQpc);
        for (int i = 0; i < 10 && transport.snapshot().state != TransportState::playing; ++i) output.tick();
        require(transport.snapshot().state == TransportState::playing && transport.snapshot().timelineOrigin == before.submittedEnd,
                "Resume replayed/dropped already-accepted tail");
        require(transport.status().wasOk(), "Stub transport failed"); output.close();
    });
    suite.test("pause, stop, beginning and missed scheduled start retain distinct transport states", []
    {
        PlaybackPcmQueue queue(1000, 100); TimelineTransport transport(1000, 1000000, queue, 1000);
        float l[100], r[100];
        transport.seek(300); transport.processOutput(stamp(0), l, r); transport.prepared(200, false);
        transport.processOutput(stamp(100), l, r); require(transport.snapshot().state == TransportState::ready, "Paused seek did not cue");
        transport.pause(); transport.processOutput(stamp(200), l, r); require(transport.snapshot().state == TransportState::paused, "Pause state");
        transport.stop(); transport.processOutput(stamp(300), l, r); queue.reset(); transport.prepared(0, false);
        transport.processOutput(stamp(400), l, r); require(transport.snapshot().state == TransportState::stopped && transport.snapshot().frozenSample == 0, "Stop did not return to zero");
        transport.goToStart(); transport.processOutput(stamp(500), l, r); transport.prepared(0, false);
        transport.processOutput(stamp(600), l, r); require(transport.snapshot().state == TransportState::ready, "Beginning did not cue first frame");
        transport.prepared(650, true); transport.processOutput(stamp(700), l, r);
        require(transport.snapshot().state == TransportState::buffering && transport.snapshot().firstBlockQpc == 0, "Missed reservation silently started late");
    });
    suite.test("seek generation discards in-flight decode and refuses stale frame requests", []
    {
        auto state = std::make_shared<StubState>(); state->delay.store(true);
        const auto index = videoIndex(); VideoPlaybackEngine video(factory(state)); video.prepare({videoClip(index, 0, 0, index->length)});
        video.seek(0, 1); eventually([&] { return state->entered.load(); }); video.seek(8000, 2); state->release.store(true);
        eventually([&] { return video.ready(8000, 2); }); const auto display = video.displaySelection(0);
        require(display.frame && display.frame->generation == 2 && display.frame->begin == 8000, "Stale decode became display-ready");
        require(!video.requestFrame(0, 0, 1) && !video.ready(0, 1), "Old seek accepted");
    });
    suite.test("pending Seek survives immediate Play/Pause and scrub release bypasses drag throttle", []
    {
        PlaybackPcmQueue queue(1000, 100); TimelineTransport transport(1000, 1000000, queue, 1000); float l[100], r[100];
        transport.seek(300); const auto gen = transport.generation(); transport.play(); transport.pause();
        transport.processOutput(stamp(0), l, r);
        require(transport.snapshot().state == TransportState::preparing && transport.snapshot().generation == gen
            && transport.snapshot().frozenSample == 300, "Immediate Play/Pause replaced unacknowledged Seek");
        transport.scrub(400, false, 1000000); const auto first = transport.generation();
        transport.scrub(500, false, 1001000); transport.scrub(600, false, 1002000);
        require(transport.generation() == first, "Drag decode requests exceeded 15 Hz");
        transport.scrub(700, true, 1003000); require(transport.generation() == first + 1, "Release was throttled");
        transport.processOutput(stamp(100), l, r); require(transport.snapshot().frozenSample == 700, "Release did not adopt exact latest target");
    });
    suite.test("pause ramp drains prepared samples and resume does not replay the accepted tail", []
    {
        PlaybackPcmQueue queue(1000, 100); TimelineTransport transport(1000, 1000000, queue, 1000);
        float l[100], r[100], pcm[100]; std::fill_n(pcm, 100, 1.0f);
        transport.seek(0); transport.processOutput(stamp(0), l, r);
        require(queue.push(0, transport.generation(), pcm, pcm, 100), "Initial playback PCM"); transport.prepared(100, true);
        transport.processOutput(stamp(100), l, r); require(queue.push(100, transport.generation(), pcm, pcm, 100), "Pause tail PCM");
        transport.pause(); transport.processOutput(stamp(200), l, r);
        require(l[0] > l[1] && l[1] > l[2] && l[2] == 0 && l[3] == 0, "Pause did not ramp into silence");
        require(transport.snapshot().submittedEnd == 103, "Fade tail not included in audible accounting");
        transport.play(); transport.processOutput(stamp(300), l, r);
        require(transport.snapshot().frozenSample == 103, "Resume replayed submitted device tail");
    });
    suite.test("next clip decoder and first frame are ready before a cut, within three frame slots", []
    {
        auto state = std::make_shared<StubState>(); const auto index = videoIndex();
        const auto first = videoClip(index, 0, 0, 1600), next = videoClip(index, 1600, 48000, 1600);
        VideoPlaybackEngine video(factory(state)); video.prepare({first, next}); video.seek(0, 1);
        eventually([&] { return video.ready(0, 1) && video.ready(1600, 1); });
        require(state->opens.load() == 2, "Next decoder was not pre-opened");
        require(static_cast<int>(video.telemetry()["cameras"][0]["readyFrames"]) <= 3, "Display-ready queue exceeded three");
        video.requestFrame(0, 1600, 1); const auto selection = video.displaySelection(0);
        require(selection.frame && selection.frame->clipId == next.mapping.clipId && selection.frame->pts == 60, "Cut had no exact prefetched picture");
        eventually([&] { return video.ready(2400, 1); }); require(state->opens.load() == 2, "Prefetched decoder not promoted");
    });
    suite.test("two cameras share generation/readiness, source gaps clear display, file epoch invalidates ready frames", []
    {
        auto state = std::make_shared<StubState>(); const auto index = videoIndex();
        VideoPlaybackEngine video(factory(state)); video.prepare({videoClip(index, 0, 0, 1600, 0), videoClip(index, 0, 48000, 1600, 1)});
        video.seek(800, 1); eventually([&] { return video.ready(800, 1); });
        require(video.displaySelection(0).frame->pts == 1 && video.displaySelection(1).frame->pts == 61, "Camera mapping desynchronized");
        video.requestFrame(0, 1600, 1); require(video.displaySelection(0).gap, "Gap retained unrelated source");
        index->epoch->value.fetch_add(1); require(!video.ready(800, 1) && !video.displaySelection(1).frame, "Old file generation displayed");
    });
    suite.test("decoder initialization failure is observable and never replaced by a CPU stub", []
    {
        std::atomic<int> opens{0}; const auto index = videoIndex();
        VideoPlaybackEngine video([&](auto) -> std::unique_ptr<IVideoFrameDecoder> { ++opens; throw std::runtime_error("d3d11va initialization failed"); });
        video.prepare({videoClip(index, 0, 0, 1600)}); video.seek(0, 1);
        eventually([&] { return video.status().failed(); }); require(opens == 1 && !video.ready(0, 1), "Decoder error silently fell back");
    });
    return suite.result("playback-engine");
}
