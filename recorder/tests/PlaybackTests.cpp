#include "TestSupport.h"
#include "media/MediaIndex.h"
#include "playback/TimelineAudioRenderer.h"
#include "playback/TimelineTransport.h"
#include "playback/VideoPlaybackEngine.h"
#include "record/WavTrackWriter.h"
#include "storage/StorageEncoding.h"
#include "support/Platform.h"
#include "CutSeamChecks.h"
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
    std::atomic<unsigned> opens{0}, decodes{0}, resets{0};
    std::atomic<int> blockedPacket{-1};
    std::atomic<bool> delay{false}, entered{false}, release{false};
    std::atomic<bool> waitOnEvent{false};
    PlaybackWakeEvent enteredWake, releaseWake;
};
class StubDecoder final : public IVideoFrameDecoder
{
public:
    explicit StubDecoder(std::shared_ptr<StubState> s) : shared(std::move(s)) { ++shared->opens; }
    void resetForSeek() override { ++shared->resets; }
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t packet, const std::function<bool()>& cancelled) override
    {
        ++shared->decodes;
        if (shared->waitOnEvent.exchange(false))
        {
            shared->enteredWake.signal();
            // Model an already-submitted GPU fence: cancellation cannot release
            // the DPB slice until completion. No sleep/timer polling in this seam.
            WaitForSingleObject(shared->releaseWake.nativeHandle(), 5000);
        }
        if (shared->delay.exchange(false) || shared->blockedPacket.load() == static_cast<int>(packet))
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
    suite.test("Playback starting in a long gap paints the whole host black before any decoder exists", []
    {
        struct Host
        {
            HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 80, 60, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            ~Host() { if (window) DestroyWindow(window); }
        } host;
        require(host.window != nullptr, "Create hidden playback test host");
        auto state = std::make_shared<StubState>(); const auto index = videoIndex();
        VideoPlaybackEngine video(factory(state)); video.prepare({videoClip(index, 15 * 48000, 0, 48000)});
        video.seek(0); video.attachPlaybackView(0, host.window);
        const auto overlay = FindWindowExW(host.window, nullptr, L"STATIC", nullptr);
        require(overlay != nullptr, "Gap overlay exists without a decoded texture");
        RECT bounds{}; GetClientRect(overlay, &bounds);
        require(bounds.right == 80 && bounds.bottom == 60, "Initial gap covers the entire host");
        struct Canvas
        {
            HDC dc = CreateCompatibleDC(nullptr); HBITMAP bitmap = nullptr; HGDIOBJ previous = nullptr;
            ~Canvas() { if (previous) SelectObject(dc, previous); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); }
        } canvas;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 80; info.bmiHeader.biHeight = -60;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr; canvas.bitmap = CreateDIBSection(canvas.dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        require(canvas.dc && canvas.bitmap && pixels, "Create device-free gap paint canvas");
        canvas.previous = SelectObject(canvas.dc, canvas.bitmap); FillRect(canvas.dc, &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        SendMessageW(overlay, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.dc), PRF_CLIENT | PRF_ERASEBKGND);
        require(GetPixel(canvas.dc, 1, 1) == RGB(0, 0, 0) && GetPixel(canvas.dc, 78, 58) == RGB(0, 0, 0), "Actual startup overlay paints black across the host");
        require(state->opens == 0, "Leading gap must not open a decoder or GPU adapter");
    });
    suite.test("Transport keeps pending and beyond-end cursors, prepares silent tail and stops without error", []
    {
        TimelineAudioRenderer audio(1000, 100); audio.setPlan({}, 1000);
        VideoPlaybackEngine video(factory(std::make_shared<StubState>())); video.prepare({});
        TimelineTransport transport(1000, 1000000, audio.queue(), 1000); StubOutput output; output.start(transport);
        transport.scrub(15000, true, 1000000);
        require(transport.playhead(1000000) == 15000, "Unacknowledged seek reports requested position");
        transport.play();
        eventually([&]
        {
            output.tick(); transport.service(audio, video, output, output.current.callbackQpc);
            require(transport.status().wasOk(), "Beyond-end preparation failed");
            for (unsigned i = 0; i < 100; ++i) require(output.left[i] == 0 && output.right[i] == 0, "Beyond-end playback submitted audio");
            return transport.snapshot().state == TransportState::stopped && transport.snapshot().generation == transport.generation();
        });
        require(transport.playhead(output.current.callbackQpc) == 15000 && video.displaySelection(0).gap, "Cursor and black selection retained past end");
        transport.scrub(700, true, 2000000); transport.scrub(725, false, 2000001);
        require(transport.playhead(2000001) == 725, "Throttled scrub position is visible before dispatch");
        transport.goToStart(); require(transport.playhead(2000002) == 0, "Go to start retires a pending far seek");
        rejects([&] { transport.seek(-1); }); rejects([&] { transport.scrub(-1, true, 2000003); });
    });
    suite.test("One full frame and multi-second timeline gaps clear both cameras; subframe seam holds", []
    {
        const auto index = videoIndex(); const Sample frame = index->packets[0].endSample - index->packets[0].sample;
        for (const Sample gap : {frame - 1, frame, 10 * Sample(index->sampleRate)})
        {
            VideoPlaybackEngine video(factory(std::make_shared<StubState>())); std::vector<PlaybackVideoClip> clips;
            for (unsigned cam = 0; cam < 2; ++cam)
            {
                clips.push_back(videoClip(index, 0, 0, frame * 2, cam));
                clips.push_back(videoClip(index, frame * 2 + gap, frame * 4, frame * 2, cam));
            }
            video.prepare(clips); std::array<PlaybackDisplayState, 2> displays;
            auto generation = video.seek(frame); eventually([&] { return video.ready(frame, generation); });
            for (unsigned cam = 0; cam < 2; ++cam) displays[cam].submitted(video.displaySelection(cam).frame);
            for (const Sample at : {frame * 2, frame * 2 + gap - 1})
            {
                generation = video.seek(at); eventually([&] { return video.ready(at, generation); });
                for (unsigned cam = 0; cam < 2; ++cam)
                {
                    const auto selection = video.displaySelection(cam); const auto decision = displays[cam].select(selection);
                    require(selection.gap == (gap >= frame), "Exact one-frame gap boundary");
                    require((decision.action == PlaybackDisplayAction::clear) == (gap >= frame), "Long gap must clear retained pixels");
                    if (gap >= frame) require(!decision.frame && !displays[cam].retained, "Previous frame cannot remain in a gap");
                    else require(decision.frame != nullptr, "Legacy subframe seam keeps the final frame");
                }
            }
            const auto next = frame * 2 + gap; generation = video.seek(next); eventually([&] { return video.ready(next, generation); });
            for (unsigned cam = 0; cam < 2; ++cam) require(video.displaySelection(cam).frame != nullptr, "Next clip resumes after gap");
        }
        require(VideoPlaybackEngine::prerollMilliseconds == 250, "Existing IDR preroll duration retained");
    });
    suite.test("alignment: compiled CFR frames match the indexed rounded sample boundaries", []
    {
        for (unsigned Fs : {48000u, 44100u, 32000u})
        {
            auto source = videoIndex(); source->sampleRate = Fs; source->validateAndBuild();
            RenderSpan span{{12345, source->length - 101}, newId(), newId(), 101, 1, 60, Fs};
            for (Sample delta = 0; delta < Fs; ++delta)
                require(RenderPlanCompiler::sourceUnitAt(span, 12345 + delta, true) == Sample(source->frameAt(101 + delta)),
                    "Compiler and decoder disagree at a rounded CFR sample boundary");
        }
    });
    suite.test("alignment: moved trimmed clip maps every boundary to the same source video and PCM", []
    {
        Temp temp; auto source = videoIndex(601);
        source->startPts = 120;
        for (auto& p : source->packets) { p.pts += 120; p.dts += 120; }
        source->validateAndBuild();
        std::vector<std::int32_t> pcm(std::size_t(source->length));
        for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = std::int32_t(i) + 1;
        auto audio = std::make_shared<WavSource>(); audio->epoch = std::make_shared<MediaEpoch>(); audio->trackId = "alignment-mic";
        audio->chunks.push_back(wav(temp.root.getChildFile("position.wav"), 0, pcm, source->length)); MediaIndex::validateWav(*audio);
        constexpr Sample placement = 12345, in = 63841, length = 96123;
        auto vc = videoClip(source, placement, in, length); vc.mapping.gaps = {{1600, 800}};
        auto ac = audioTrack(audio, placement, in, length); ac.clips[0].mapping.gaps = vc.mapping.gaps;
        TimelineAudioRenderer renderer(48000, 512); renderer.setPlan({ac}, placement + length);
        VideoPlaybackEngine video(factory(std::make_shared<StubState>())); video.prepare({vc}); std::uint64_t generation = 0;
        for (Sample delta : {Sample{-1}, Sample{0}, Sample{1}, Sample{158}, Sample{159}, Sample{160}, Sample{799}, Sample{800},
                             Sample{1599}, Sample{1600}, Sample{2399}, Sample{2400}, Sample{48000}, length - 1, length})
        {
            const auto t = placement + delta; const auto u = in + delta;
            video.seek(t, ++generation); eventually([&] { return video.ready(t, generation); });
            const auto selected = video.displaySelection(0); float l = 0, r = 0; renderer.renderAudio(t, 1, &l, &r);
            const bool gap = delta < 0 || delta >= length || (delta >= 1600 && delta < 2400);
            require(selected.gap == gap, "Clip block/gap and selected picture disagree");
            if (!gap)
            {
                require(selected.frame && selected.frame->pts == 120 + u / 800 && selected.frame->begin <= t && t < selected.frame->end,
                    "Nonzero PTS origin, sourceIn or placement shifted the selected frame");
                require(l == float(u + 1) / 8388608.0f && r == l, "PCM source coordinate differs from video source coordinate");
            }
            else require(l == 0 && r == 0, "Audio escaped the visible clip/gap boundary");
        }
    });
    suite.test("alignment: coordinator selects the audible playhead frame without a fixed display lead", []
    {
        auto source = videoIndex(); source->sampleRate = 1000; source->validateAndBuild();
        VideoPlaybackEngine video(factory(std::make_shared<StubState>())); video.prepare({videoClip(source, 0, 0, source->length)});
        TimelineAudioRenderer audio(1000, 100); audio.setPlan({}, source->length);
        TimelineTransport transport(1000, 1000000, audio.queue(), source->length); StubOutput output; output.start(transport);
        transport.seek(0); transport.play(); unsigned checked = 0;
        for (int i = 0; i < 16; ++i)
        {
            // Advance synthetic callbacks only after the worker has supplied
            // their PCM; otherwise this tight loop fabricates an underrun.
            if (transport.snapshot().state == TransportState::playing)
                eventually([&] { return audio.queue().queuedFrames() >= 100; });
            output.tick(); transport.service(audio, video, output, output.current.callbackQpc);
            if (transport.snapshot().state == TransportState::preparing)
                eventually([&] { transport.service(audio, video, output, output.current.callbackQpc); return audio.ready() && video.ready(transport.snapshot().frozenSample, transport.generation()); });
            const auto snapshot = transport.snapshot();
            if (snapshot.state != TransportState::playing) continue;
            const auto audible = TimelineTransport::audibleCursor(snapshot, 1000, 1000000, output.current.callbackQpc);
            const auto target = Sample(video.telemetry()["cameras"][0]["targetSample"]);
            require(target == audible, "Video coordinator requested a different time from the audible/UI cursor");
            eventually([&] { return video.ready(target, transport.generation()); });
            const auto selected = video.displaySelection(0);
            require(selected.frame && selected.frame->begin <= audible && audible < selected.frame->end,
                "Selected video frame does not contain the displayed/audible playhead");
            ++checked;
        }
        require(checked >= 8 && transport.status().wasOk(), "Synthetic advancing output did not exercise alignment");
    });
    suite.test("packet index builds closed-GOP IDR lookup and floors exact frame containment", []
    {
        const auto v = videoIndex(); require(v->length == 144000 && v->idrs.size() == 3, "Index length/IDR count");
        require(v->frameAt(799) == 0 && v->frameAt(800) == 1 && v->frameAt(47999) == 59, "Frame boundary floor");
        require(v->previousIdr(47999) == 0 && v->previousIdr(48000) == 60 && v->previousIdr(95999) == 60, "Previous IDR selection");
        rejects([&] { v->frameAt(v->length); }); rejects([&] { v->frameAt(-1); });
    });
    suite.test("seek plan decodes IDR prefix without conversion and reuses the shortest valid DPB path", []
    {
        const auto source = videoIndex();
        const auto plan = playbackDecodePlan(*source, 119);
        require(plan.fromIdr && plan.firstPacket == 60 && plan.decodeOnlyFrames == 59, "Full GOP seek prefix plan");
        unsigned converted = 0, discarded = 0;
        for (auto packet = plan.firstPacket; packet <= plan.targetPacket; ++packet)
            if (plan.convert(source->packets[packet].pts, *source)) ++converted; else ++discarded;
        require(converted == 1 && discarded == 59, "Prefix frame reached target conversion path");
        const auto forward = playbackDecodePlan(*source, 119, 115);
        require(!forward.fromIdr && forward.firstPacket == 116 && forward.decodeOnlyFrames == 3, "Forward seek redecoded existing prefix");
        const auto adjacentIdr = playbackDecodePlan(*source, 60, 59);
        require(!adjacentIdr.fromIdr && adjacentIdr.decodeOnlyFrames == 0, "Adjacent IDR unnecessarily flushed decoder");
        require(playbackDecodePlan(*source, 119, 10).firstPacket == 60, "Distant forward seek missed closer IDR");
        require(playbackDecodePlan(*source, 59, 119).fromIdr, "Backward seek reused invalid sequential position");
        require(playbackDecodePlan(*source, 59, 59).fromIdr, "Uncached same frame skipped required reset");
        rejects([&] { playbackDecodePlan(*source, source->packets.size()); });
    });
    suite.test("completed frame wakes independent coordinator and presenter events without polling", []
    {
        auto state = std::make_shared<StubState>(); state->waitOnEvent = true;
        const auto source = videoIndex(); auto wake = std::make_shared<PlaybackWakeEvent>();
        VideoPlaybackEngine video(factory(state)); video.setWakeEvent(wake);
        video.prepare({videoClip(source, 0, 0, source->length)}); video.seek(0, 1);
        require(WaitForSingleObject(state->enteredWake.nativeHandle(), 1000) == WAIT_OBJECT_0, "Decoder did not enter gate");
        require(WaitForSingleObject(wake->nativeHandle(), 0) == WAIT_OBJECT_0, "Request notification lost before wait");
        require(WaitForSingleObject(video.presentationWakeHandle(0), 0) == WAIT_OBJECT_0, "Presenter request event missing");
        require(!video.ready(0, 1), "Incomplete decode marked ready");
        state->releaseWake.signal();
        // The coordinator must be woken without polling, but a wake may arrive slightly before the frame becomes
        // visible to ready(); tolerate that by re-waiting on the same event instead of asserting on the first wake.
        bool woken = false;
        for (const auto deadline = juce::Time::getMillisecondCounter() + 5000; juce::Time::getMillisecondCounter() < deadline;)
        {
            if (WaitForSingleObject(wake->nativeHandle(), 200) == WAIT_OBJECT_0) woken = true;
            if (woken && video.ready(0, 1)) break;
        }
        require(woken, "Publication did not wake the coordinator");
        require(video.ready(0, 1), "Published frame never became ready after the wake");
        wake->signal(); // the loop above may have consumed the publication wake; the receipt check below owns the next one
        require(WaitForSingleObject(video.presentationWakeHandle(0), 0) == WAIT_OBJECT_0, "Coordinator consumed presenter's notification");
        const auto frame = video.displaySelection(0).frame;
        video.presented(0, *frame, qpcNow());
        require(WaitForSingleObject(wake->nativeHandle(), 0) == WAIT_OBJECT_0 && video.seekTiming(0).presentQpc,
            "DXGI receipt observation waited for another service tick");
        require(state->decodes == 1, "Paused preparation started unnecessary same-clip prefetch");
    });
    suite.test("resident hit publishes synchronously while cancelled prefetch still owns its GPU fence", []
    {
        auto state = std::make_shared<StubState>(); const auto source = videoIndex();
        auto wake = std::make_shared<PlaybackWakeEvent>(); VideoPlaybackEngine video(factory(state));
        video.setWakeEvent(wake); video.prepare({videoClip(source, 0, 0, source->length)}); video.seek(0, 1);
        eventually([&] { return video.ready(0, 1); }); const auto old = video.displaySelection(0).frame;
        state->waitOnEvent = true; video.requestFrame(0, 0, 1, true);
        require(WaitForSingleObject(state->enteredWake.nativeHandle(), 1000) == WAIT_OBJECT_0, "Prefetch did not enter fence gate");
        WaitForSingleObject(wake->nativeHandle(), 0); WaitForSingleObject(video.presentationWakeHandle(0), 0);
        const auto decodes = state->decodes.load(); video.seek(200, 2);
        require(video.ready(200, 2), "Resident hit queued behind cancelled GPU prefetch");
        const auto t = video.seekTiming(0); const auto frame = video.displaySelection(0).frame;
        require(t.cacheHit && t.readyQpc && !t.workerQpc && !t.decodeBeginQpc && state->decodes == decodes,
            "Hit called the worker/decoder before publishing");
        require(frame != old && frame->generation == 2 && old->generation == 1, "Cached generation wrapper mutated in place");
        require(WaitForSingleObject(wake->nativeHandle(), 0) == WAIT_OBJECT_0
            && WaitForSingleObject(video.presentationWakeHandle(0), 0) == WAIT_OBJECT_0, "Synchronous hit did not signal both handoffs");
        source->epoch->value.fetch_add(1); video.seek(300, 3);
        require(!video.ready(300, 3) && !video.seekTiming(0).cacheHit, "Stale media epoch reused cached texture");
        state->releaseWake.signal();
    });
    suite.test("ASIO acknowledgement signals a preallocated coordinator event after atomic publication", []
    {
        PlaybackPcmQueue queue(1000, 100); TimelineTransport transport(1000, 1000000, queue, 1000);
        float l[100], r[100]; transport.seek(100);
        require(WaitForSingleObject(transport.wakeHandle(), 0) == WAIT_OBJECT_0, "Seek did not wake command owner");
        require(WaitForSingleObject(transport.wakeHandle(), 0) == WAIT_TIMEOUT, "Event did not auto-reset");
        transport.processOutput(stamp(0), l, r);
        require(WaitForSingleObject(transport.wakeHandle(), 0) == WAIT_OBJECT_0
            && transport.snapshot().generation == transport.generation(), "Callback acknowledgement lost its publication wake");
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
        transport.processOutput(stamp(400), l, r); require(transport.snapshot().state == TransportState::stopped && transport.snapshot().frozenSample == 300, "Stop did not retain current position");
        transport.goToStart(); transport.processOutput(stamp(500), l, r); transport.prepared(0, false);
        transport.processOutput(stamp(600), l, r); require(transport.snapshot().state == TransportState::ready && transport.snapshot().frozenSample == 0, "Beginning did not cue first frame");
        transport.prepared(650, true); transport.processOutput(stamp(700), l, r);
        require(transport.snapshot().state == TransportState::buffering && transport.snapshot().firstBlockQpc == 0, "Missed reservation silently started late");
    });
    suite.test("stop seeks to the audible cursor and preserves an unacknowledged seek target", []
    {
        PlaybackPcmQueue queue(1000, 100); TimelineTransport transport(1000, 1000000, queue, 1000);
        float l[100], r[100], pcm[300]; std::fill_n(pcm, 300, 1.0f);
        transport.seek(300); transport.processOutput(stamp(0), l, r);
        require(queue.push(300, transport.generation(), pcm, pcm, 100)
                && queue.push(400, transport.generation(), pcm, pcm, 100)
                && queue.push(500, transport.generation(), pcm, pcm, 100), "Prepared playback PCM");
        transport.prepared(100, true);
        transport.processOutput(stamp(100, 100, 1000, 50), l, r);
        transport.processOutput(stamp(200, 100, 1000, 50), l, r);
        const auto playing = transport.snapshot();
        require(playing.submittedEnd == 500 && TimelineTransport::audibleCursor(playing, 1000, 1000000, playing.callbackQpc) == 350,
                "Fixture must distinguish submitted and audible samples");
        transport.stop(); transport.processOutput(stamp(300, 100, 1000, 50), l, r);
        queue.reset(); transport.prepared(0, false); transport.processOutput(stamp(400, 100, 1000, 50), l, r);
        require(transport.snapshot().state == TransportState::stopped && transport.snapshot().frozenSample == 350, "Stop lost the audible cursor");
        transport.seek(777); transport.stop(); transport.processOutput(stamp(500, 100, 1000, 50), l, r);
        transport.prepared(0, false); transport.processOutput(stamp(600, 100, 1000, 50), l, r);
        require(transport.snapshot().state == TransportState::stopped && transport.snapshot().frozenSample == 777, "Stop lost the pending seek target");
    });
    suite.test("dubbing lock rejects transport changes without cancelling pending play", []
    {
        TimelineAudioRenderer audio(1000, 100); audio.setPlan({}, 10000);
        auto state = std::make_shared<StubState>(); VideoPlaybackEngine video(factory(state)); video.prepare({});
        TimelineTransport transport(1000, 1000000, audio.queue(), 10000); StubOutput output; output.start(transport);
        transport.seek(100); transport.play(); const auto generation = transport.generation();
        transport.setDubbingLocked(true);
        rejects([&] { transport.seek(900); }); rejects([&] { transport.scrub(900, true, qpcNow()); });
        rejects([&] { transport.play(); }); rejects([&] { transport.pause(); });
        rejects([&] { transport.stop(); }); rejects([&] { transport.goToStart(); });
        rejects([&] { transport.prepared(300, true); });
        output.tick(); transport.service(audio, video, output, output.current.callbackQpc);
        require(transport.generation() == generation && audio.queue().queuedFrames() == 0, "Dubbing lock allowed output preparation");
        transport.setDubbingLocked(false); transport.service(audio, video, output, output.current.callbackQpc);
        eventually([&] { return audio.ready(); }); transport.service(audio, video, output, output.current.callbackQpc);
        for (int i = 0; i < 8 && transport.snapshot().state != TransportState::playing; ++i) output.tick();
        require(transport.snapshot().state == TransportState::playing && transport.snapshot().timelineOrigin == 100,
                "Rejected transport command mutated pending play intent or seek target");
        require(transport.status().wasOk(), "Transport failed after unlocking"); output.close();
    });
    suite.test("seek generation discards in-flight decode and refuses stale frame requests", []
    {
        auto state = std::make_shared<StubState>(); state->delay.store(true);
        const auto index = videoIndex(); VideoPlaybackEngine video(factory(state)); video.prepare({videoClip(index, 0, 0, index->length)});
        video.seek(0, 1); eventually([&] { return state->entered.load(); }); video.seek(8000, 2); state->release.store(true);
        eventually([&] { return video.ready(8000, 2); }); const auto display = video.displaySelection(0);
        require(display.frame && display.frame->generation == 2 && display.frame->begin == 8000, "Stale decode became display-ready");
        require(!video.requestFrame(0, 0, 1) && !video.ready(0, 1), "Old seek accepted");
        require(state->opens == 1 && state->resets >= 1, "Cancelled seek recreated the device or retained decoder position");
    });
    suite.test("exact seek publishes before blocked prefetch and rebinds cached frames without reopening decoders", []
    {
        auto state = std::make_shared<StubState>(); state->blockedPacket = 1;
        const auto index = videoIndex(); VideoPlaybackEngine video(factory(state));
        video.prepare({videoClip(index, 0, 0, index->length)}); video.seek(0, 1);
        eventually([&] { return video.ready(0, 1); }); video.requestFrame(0, 0, 1, true);
        eventually([&] { return state->entered.load(); });
        require(video.ready(0, 1), "Exact target waits for optional prefetch");
        const auto old = video.displaySelection(0).frame;
        video.seek(200, 2); // same containing frame, new exact seek generation
        eventually([&] { return video.ready(200, 2); });
        const auto frame = video.displaySelection(0).frame;
        require(frame && frame != old && frame->begin == 0 && frame->generation == 2, "Cached immutable frame not rebound");
        require(video.seekTiming(0).cacheHit && video.seekTiming(0).decodeBeginQpc == 0, "Cache hit performed target decode");
        video.presented(0, *old, 100); require(video.lastPresentation(0).generation != 1, "Old mailbox receipt accepted");
        video.presented(0, *frame, 200); video.presented(0, *frame, 300);
        require(video.lastPresentation(0).qpc == 200 && video.seekTiming(0).presentQpc == 200, "First exact receipt overwritten by repeated Present");
        require(state->opens == 1, "Warm cached seek reopened decoder"); state->release = true;
    });
    suite.test("100 seek generations select exact containing frames on both cameras with bounded caches", []
    {
        auto state = std::make_shared<StubState>(); const auto index = videoIndex(3600);
        VideoPlaybackEngine video(factory(state));
        video.prepare({videoClip(index, 0, 0, index->length, 0), videoClip(index, 0, 0, index->length, 1)});
        Sample previousTarget = 0;
        for (std::uint64_t gen = 1; gen <= 100; ++gen)
        {
            const Sample target = gen % 10 == 0 ? previousTarget : static_cast<Sample>((gen * 7919) % index->length);
            video.seek(target, gen); eventually([&] { return video.ready(target, gen); });
            for (unsigned camera = 0; camera < 2; ++camera)
            {
                const auto selection = video.displaySelection(camera);
                require(selection.frame && selection.frame->generation == gen && selection.frame->begin <= target
                    && target < selection.frame->end && selection.frame->pts == index->packets[index->frameAt(target)].pts, "Seek did not select exact indexed frame");
                if (gen % 10 == 0) require(video.seekTiming(camera).cacheHit, "Repeated seek missed resident exact frame");
                require(static_cast<int>(video.telemetry()["cameras"][static_cast<int>(camera)]["readyFrames"]) <= 3, "Cache exceeded three frames");
            }
            require(!video.ready(target, gen - 1), "Previous generation became ready"); previousTarget = target;
        }
        require(state->opens == 2, "Seek storm recreated per-camera devices");
        require(state->resets == 0, "Completed seeks unconditionally invalidated decoder position");
    });
    suite.test("late counts missing frames only at advancing present ticks, never inspection or seek preparation", []
    {
        auto state = std::make_shared<StubState>(); state->delay = true;
        const auto index = videoIndex(); VideoPlaybackEngine video(factory(state));
        video.prepare({videoClip(index, 0, 0, index->length)}); video.seek(0, 1);
        eventually([&] { return state->entered.load(); });
        const auto late = [&] { return static_cast<juce::int64>(video.telemetry()["cameras"][0]["lateDisplaySelections"]); };
        for (int i = 0; i < 2500; ++i) { video.displaySelection(0); video.displaySelection(0, true); }
        require(late() == 0, "Startup/paused exact preparation counted as late");
        video.requestFrame(0, 100, 1, true);
        for (int i = 0; i < 2500; ++i) video.displaySelection(0, true);
        require(late() == 0, "Caller marked initial seek preparation advancing and inflated late count");
        state->release = true; eventually([&] { return video.ready(0, 1); });
        state->release = false; state->blockedPacket = 5; state->entered = false;
        video.requestFrame(0, 4000, 1, true); eventually([&] { return state->entered.load(); });
        for (int i = 0; i < 2500; ++i) video.displaySelection(0);
        require(late() == 0, "UI gap/telemetry query counted as presentation");
        for (Sample sample = 4000; sample < 4800; ++sample)
        { video.requestFrame(0, sample, 1, true); video.displaySelection(0, true); }
        require(late() == 1, "Repeated ticks/audio samples within one late frame inflated count");
        video.requestFrame(0, 4800, 1, true); video.displaySelection(0, true); require(late() == 2, "Next missing frame not counted");
        video.requestFrame(0, index->length, 1, true); require(video.displaySelection(0, true).gap && late() == 2, "Gap counted as late");
        video.seek(1600, 2); state->release = true; eventually([&] { return video.ready(1600, 2); });
        for (Sample sample = 1600; sample < 2400; ++sample)
        {
            video.requestFrame(0, sample, 2, true); const auto s = video.displaySelection(0, true);
            require(s.frame && s.frame->begin == 1600, "Containing-frame floor selection changed within a frame");
        }
        require(late() == 2, "Ready frame counted as late");
    });
    suite.test("epoch replacement buffers a valid clip, submits black once, then submits the new frame", []
    {
        auto state = std::make_shared<StubState>(); const auto old = videoIndex(), fresh = videoIndex();
        VideoPlaybackEngine video(factory(state)); video.prepare({videoClip(old, 0, 0, old->length)});
        video.seek(0, 1); eventually([&] { return video.ready(0, 1); });
        PlaybackDisplayState display; auto decision = display.select(video.displaySelection(0));
        require(decision.action == PlaybackDisplayAction::picture, "Initial picture not ready");
        display.submitted(decision.frame);
        ++old->epoch->value; state->blockedPacket = 2;
        video.handoff({videoClip(fresh, 0, 0, fresh->length)}, 1600, 2);
        eventually([&] { return state->entered.load(); });
        const auto selection = video.displaySelection(0);
        require(!selection.gap && selection.buffering && !selection.frame, "Replacement fixture did not buffer a valid clip");
        decision = display.select(selection);
        require(decision.shouldSubmit() && decision.action == PlaybackDisplayAction::clear && !decision.frame, "Presenter would skip invalidated on-screen pixels");
        unsigned black = 0, pictures = 0;
        require(!video.submitIfCurrent(0, 1, [&] { ++black; }), "Old generation submitted the clear");
        decision = display.select(video.displaySelection(0));
        require(decision.shouldSubmit() && video.submitIfCurrent(0, 2, [&] { ++black; }), "Generation rejection consumed pending clear");
        display.submitted(decision.frame); // synthetic receipt of the same submit/skip branch
        for (int i = 0; i < 3; ++i) require(!display.select(video.displaySelection(0)).shouldSubmit(), "Buffering resubmitted black after success");
        state->release = true; eventually([&] { return video.ready(1600, 2); });
        decision = display.select(video.displaySelection(0));
        require(decision.action == PlaybackDisplayAction::picture && decision.frame && decision.frame->source == fresh && decision.frame->pts == 2, "Replacement did not resume at exact PTS");
        require(video.submitIfCurrent(0, 2, [&] { ++pictures; }), "Replacement submit rejected");
        display.submitted(decision.frame);
        require(black == 1 && pictures == 1 && video.status().wasOk(), "Invalidation/replacement submission sequence failed");
    });
    suite.test("seek discard and texture retry retain acquired DXGI opportunity until successful Present", []
    {
        struct Event { HANDLE value = CreateEventW(nullptr, FALSE, TRUE, nullptr); ~Event() { if (value) CloseHandle(value); } } signal;
        require(signal.value && WaitForSingleObject(signal.value, 0) == WAIT_OBJECT_0, "Acquire initial synthetic latency signal");
        PlaybackPresentOpportunity opportunity; require(opportunity.needsWait(), "Initial frame did not wait");
        opportunity.acquired();
        // Seek changes generation between drawing and Present. No submit and no
        // future swapchain signal: the next iteration must reuse this opportunity.
        require(WaitForSingleObject(signal.value, 0) == WAIT_TIMEOUT, "Synthetic wait was not consumed");
        const auto canSubmit = !opportunity.needsWait() || WaitForSingleObject(signal.value, 0) == WAIT_OBJECT_0;
        require(canSubmit, "Seek discard waits for a new signal without submitting the next frame");
        opportunity.submitted(false); require(!opportunity.needsWait(), "Occluded/busy Present consumed opportunity");
        opportunity.submitted(true); require(opportunity.needsWait(), "Successful Present did not require next latency wait");
    });
    suite.test("unacknowledged seek burst cannot publish an old target with the newest generation", []
    {
        auto state = std::make_shared<StubState>(); state->delay = true;
        const auto index = videoIndex(); VideoPlaybackEngine video(factory(state));
        video.prepare({videoClip(index, 0, 0, index->length, 0), videoClip(index, 0, 0, index->length, 1)});
        video.seek(0, 1); eventually([&] { return state->entered.load(); });
        for (std::uint64_t gen = 2; gen <= 101; ++gen) video.seek(static_cast<Sample>(gen * 800), gen);
        state->release = true; eventually([&] { return video.ready(80800, 101); });
        for (unsigned camera = 0; camera < 2; ++camera)
        {
            const auto s = video.displaySelection(camera);
            require(s.frame && s.frame->generation == 101 && s.frame->begin == 80800, "Burst paired new generation with old target");
            require(!video.requestFrame(camera, 0, 100), "Burst accepted superseded generation");
        }
    });
    suite.test("seek from ready immediately cancels video while audio waits for callback acknowledgement", []
    {
        TimelineAudioRenderer audio(1000, 100); audio.setPlan({}, 10000);
        auto state = std::make_shared<StubState>(); VideoPlaybackEngine video(factory(state)); video.prepare({});
        TimelineTransport transport(1000, 1000000, audio.queue(), 10000); StubOutput output; output.start(transport);
        transport.seek(0); output.tick(); transport.service(audio, video, output, output.current.callbackQpc);
        eventually([&] { return audio.ready(); }); transport.service(audio, video, output, output.current.callbackQpc);
        output.tick(); require(transport.snapshot().state == TransportState::ready, "Initial seek not ready");
        transport.seek(500); transport.play(); const auto gen = transport.generation();
        transport.service(audio, video, output, output.current.callbackQpc);
        require(transport.status().wasOk() && video.displaySelection(0).generation == gen
            && transport.snapshot().generation != gen, "Pending seek armed old ready state or failed to cancel video");
        output.tick(); transport.service(audio, video, output, output.current.callbackQpc);
        require(transport.status().wasOk() && transport.snapshot().frozenSample == 500, "Seek target changed before ack");
        output.close();
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
    recorder_cut_seam::addTests(suite);
    return suite.result("playback-engine");
}
