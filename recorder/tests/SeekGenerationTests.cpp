#include "TestSupport.h"
#include "playback/TimelineTransport.h"
#include "media/ThumbnailCache.h"
#include "support/Platform.h"
#include <chrono>
#include <limits>
#include <thread>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
template<class F> void eventually(F f)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) { require(std::chrono::steady_clock::now() < until, "Seek generation worker timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
std::shared_ptr<VideoIndex> source()
{
    auto s = std::make_shared<VideoIndex>(); s->epoch = std::make_shared<MediaEpoch>(); s->width = 1920; s->height = 1080;
    for (unsigned i = 0; i < 600; ++i) s->packets.push_back({i, i, 100 + i * 1000, 1, 1000, i % 60 == 0, i % 60 == 0});
    s->validateAndBuild(); return s;
}
PlaybackVideoClip clip(std::shared_ptr<const VideoIndex> s, unsigned camera = 0)
{
    PlaybackVideoClip c; c.source = std::move(s); c.camera = camera; c.mapping.clipId = newId();
    c.mapping.mediaGeneration = Sample(c.source->generation); c.mapping.lengthSamples = c.source->length; return c;
}
std::shared_ptr<const PlaybackVideoFrame> frame(std::shared_ptr<const VideoIndex> source, unsigned packet)
{
    auto f = std::make_shared<PlaybackVideoFrame>(); f->source = std::move(source); f->pts = packet; f->generation = 1;
    f->begin = packet * 800; f->end = f->begin + 800; f->clipId = newId(); return f;
}
struct Gate
{
    std::atomic<bool> block{false}; std::atomic<unsigned> decodes{0}, resets{0}; PlaybackWakeEvent entered, release;
};
struct Decoder : IVideoFrameDecoder
{
    std::shared_ptr<Gate> gate;
    explicit Decoder(std::shared_ptr<Gate> s) : gate(std::move(s)) {}
    void resetForSeek() override { ++gate->resets; }
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t, const std::function<bool()>&) override
    {
        ++gate->decodes;
        if (gate->block.exchange(false)) { gate->entered.signal(); WaitForSingleObject(gate->release.nativeHandle(), 5000); }
        return {}; // Deliberately ignores cancellation: engine must fence publication.
    }
};
}
int runSeekGenerationTests()
{
    Suite suite;
    suite.test("alignment: real thumbnail decode contains the sample including the final partial frame", []
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-thumbnail-alignment-" + juce::Uuid().toString());
        struct Cleanup { juce::File root; ~Cleanup() { if (root.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory)
            && root.getFileName().startsWith("recorder-thumbnail-alignment-")) root.deleteRecursively(); } } cleanup{root};
        require(root.createDirectory().wasOk(), "Create thumbnail alignment fixture"); const auto file = root.getChildFile("frames.mp4");
        // Native software MPEG4 keeps the test independent of NVENC/capture hardware.
        // A nonzero stream origin also catches accidentally applying N0/start_time twice.
        juce::ChildProcess encoder;
        require(encoder.start(juce::StringArray{RECORDER_IMPORT_FFMPEG_EXE, "-v", "error", "-f", "lavfi", "-i",
            "testsrc2=size=160x90:rate=60", "-frames:v", "5", "-c:v", "mpeg4", "-bf", "0", "-output_ts_offset", "2", file.getFullPathName()})
            && encoder.waitForProcessToFinish(15000) && encoder.getExitCode() == 0, "Encode software thumbnail fixture");
        const std::vector<Sample> points{1, 799, 800, 801, 3199, 3200, 3999};
        const auto frames = ThumbnailCache::decodePoints(file, 48000, points, [] { return false; });
        require(frames.size() == points.size(), "Thumbnail dropped a valid final-frame sample");
        for (std::size_t i = 0; i < points.size(); ++i)
            require(frames[i].sample == points[i] / 800 * 800 && frames[i].endSample == frames[i].sample + 800,
                "Thumbnail selected the following video frame or lost its containment interval");
        require(ThumbnailCache::decodePoints(file, 48000, {4000}, [] { return false; }).empty(), "Thumbnail escaped the exclusive source end");
    });
    suite.test("alignment: thumbnail requests follow indexed frame boundaries and never reuse a neighbouring frame", []
    {
        for (unsigned Fs : {48000u, 44100u, 32000u})
        {
            auto indexed = source(); indexed->sampleRate = Fs; indexed->validateAndBuild();
            for (Sample t = 0; t < 2 * Fs; ++t)
                require(ThumbnailCache::frameSample(t, Fs, {60,1}) == indexed->packets[indexed->frameAt(t)].sample,
                    "Thumbnail quantization differs from the indexed containing frame");
        }
        ThumbnailCache cache([](const auto&, unsigned, const auto& points, const auto&)
        { ThumbnailFrame f; f.sample = points[0]; f.endSample = f.sample + 800; f.rgb.resize(160 * 90 * 3); return std::vector<ThumbnailFrame>{f}; });
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("synthetic-thumbnail.mp4");
        require(cache.request("asset/1", file, 48000, 24000), "Request exact frame");
        eventually([&] { return bool(cache.at("asset/1", 24321)); });
        require(cache.at("asset/1", 24799) && !cache.at("asset/1", 23999) && !cache.at("asset/1", 24800)
            && !cache.at("asset/2", 24321), "Neighbouring or stale-generation thumbnail appeared at the playhead");
        require(ThumbnailCache::frameSample(23999, 48000, {60,1}) == 23200, "Half-second bucket erased 29 source frames");
        const auto last = (std::numeric_limits<Sample>::max)() - 1;
        require(ThumbnailCache::frameSample(last, 48000, {60,1}) == last / 800 * 800, "Last representable source frame overflowed during paint");
    });
    suite.test("LRU retains adjacent GOP targets with hard frame and byte limits", []
    {
        const auto s = source(); PlaybackFrameCache cache;
        cache.insert(frame(s, 59)); cache.insert(frame(s, 60));
        require(cache.find(s, 59) && cache.find(s, 60), "Adjacent GOP cache miss");
        require(!cache.find(s, 61), "Uncached frame reported hit");
        for (unsigned i = 0; i < 600; ++i)
        {
            cache.insert(frame(s, i)); const auto stats = cache.stats();
            require(stats.frames <= 4 && stats.bytes <= 32 * 1024 * 1024 && stats.peakBytes <= 32 * 1024 * 1024, "Decoded LRU exceeds memory cap");
        }
        require(cache.stats().evictions > 0 && !cache.find(s, 0) && cache.find(s, 599), "LRU did not evict old targets");
        ++s->epoch->value; require(!cache.find(s, 599) && cache.stats().bytes == 0, "Invalid file generation retained cache data");
        PlaybackFrameCache tiny({4, 1024, 2}); tiny.insert(frame(source(), 0)); require(tiny.stats().frames == 0, "Oversized texture entered tiny budget");
    });
    suite.test("in-flight old decoder result cannot overwrite newest seek or Present receipt", []
    {
        const auto s = source(); auto gate = std::make_shared<Gate>(); gate->block = true;
        VideoPlaybackEngine video([gate](auto) { return std::make_unique<Decoder>(gate); }); video.prepare({clip(s)}); video.seek(0, 1);
        require(WaitForSingleObject(gate->entered.nativeHandle(), 1000) == WAIT_OBJECT_0, "Blocked decode did not start");
        for (std::uint64_t generation = 2; generation <= 1000; ++generation) video.seek(80000 + Sample(generation), generation);
        gate->release.signal(); eventually([&] { return video.ready(81000, 1000); });
        require(video.displaySelection(0).frame->pts == 101 && gate->resets > 0, "Late decoder result won latest seek");
        const auto old = frame(s, 0); video.presented(0, *old, qpcNow());
        require(video.lastPresentation(0).generation != 1, "Stale Present receipt accepted");
        unsigned submitted = 0;
        require(!video.submitIfCurrent(0, 1, [&] { ++submitted; }) && submitted == 0, "Stale frame reached the DXGI submission seam");
        require(video.submitIfCurrent(0, 1000, [&] { ++submitted; }) && submitted == 1, "Current frame submission rejected");
    });
    suite.test("resident reverse seek publishes without waiting for an old GPU fence", []
    {
        const auto s = source(); auto gate = std::make_shared<Gate>();
        VideoPlaybackEngine video([gate](auto) { return std::make_unique<Decoder>(gate); }); video.prepare({clip(s)});
        video.seek(47200, 1); eventually([&] { return video.ready(47200, 1); });
        video.seek(48000, 2); eventually([&] { return video.ready(48000, 2); });
        gate->block = true; video.seek(96000, 3);
        require(WaitForSingleObject(gate->entered.nativeHandle(), 1000) == WAIT_OBJECT_0, "GPU fence seam did not block");
        video.seek(47200, 4); require(video.ready(47200, 4) && video.seekTiming(0).cacheHit, "Reverse GOP hit waited for decoder");
        gate->release.signal();
    });
    suite.test("file generation handoff rejects old work and remaps both new sources", []
    {
        auto old = source(), fresh = source(); fresh->generation = 2; fresh->epoch->value = 2;
        auto gate = std::make_shared<Gate>(); gate->block = true;
        VideoPlaybackEngine video([gate](auto) { return std::make_unique<Decoder>(gate); }); video.prepare({clip(old), clip(old, 1)}); video.seek(0, 1);
        require(WaitForSingleObject(gate->entered.nativeHandle(), 1000) == WAIT_OBJECT_0, "Old file decode not active");
        ++old->epoch->value; auto a = clip(fresh), b = clip(fresh, 1); b.mapping.sourceIn = 48000; b.mapping.lengthSamples -= 48000;
        video.handoff({a, b}, 1600, 2); gate->release.signal(); eventually([&] { return video.ready(1600, 2); });
        require(video.displaySelection(0).frame->source == fresh && video.displaySelection(1).frame->pts == 62, "Handoff published old file/placement");
        require(video.status().wasOk(), "Expected handoff poisoned worker status");
    });
    suite.test("1000 unacknowledged seeks coalesce without filling the ASIO command queue", []
    {
        PlaybackPcmQueue queue(48000, 480); TimelineTransport transport(48000, 1000000, queue, 480000);
        for (unsigned i = 0; i < 1000; ++i) transport.seek(i * 100);
        BlockStamp s{}; s.flags = samplePositionValid | latenciesValid; s.sampleRate = 48000; s.numSamples = 480; s.callbackQpc = 1000000;
        float l[480]{}, r[480]{}; transport.processOutput(s, l, r);
        require(transport.snapshot().generation == 1000 && transport.snapshot().frozenSample == 99900, "ASIO did not adopt only latest seek");
    });
    suite.test("continuous reverse drag dispatches approximately 15 Hz and release is exact", []
    {
        PlaybackPcmQueue queue(48000, 480); TimelineTransport transport(48000, 1000000, queue, 480000);
        for (int i = 0; i < 1000; ++i) transport.scrub(400000 - i * 100, false, 1000000 + i * 1000);
        require(transport.generation() >= 14 && transport.generation() <= 16, "Drag dispatch frequency exceeds 15 Hz");
        const auto before = transport.generation(); transport.scrub(12345, true, 2000001);
        require(transport.generation() == before + 1 && Sample(transport.telemetry()["requestedSample"]) == 12345, "Release did not bypass drag throttle");
    });
    suite.test("thumbnail strips publish incrementally and evict under a bounded RGB budget", []
    {
        ThumbnailCache cache([](const auto&, unsigned, const auto& points, const auto&)
        { ThumbnailFrame t; t.sample = points[0]; t.width = 640; t.height = 360; t.rgb.resize(640 * 360 * 3); return std::vector<ThumbnailFrame>{std::move(t)}; });
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("synthetic.mp4");
        for (int i = 0; i < 30; ++i)
        {
            require(cache.request("asset/generation-2", file, 48000, i * 48000), "Progressive request rejected");
            eventually([&] { const auto t = cache.nearest("asset/generation-2", i * 48000); return t && t->sample == i * 48000; });
            require(cache.stats().bytes <= ThumbnailCache::maximumBytes, "Thumbnail memory limit exceeded");
        }
        require(cache.stats().evictions > 0 && cache.stats().peakBytes <= ThumbnailCache::maximumBytes, "Thumbnail eviction not exercised");
        require(!cache.nearest("asset/generation-1", 0), "Wrong thumbnail media generation reused");
    });
    suite.test("late thumbnail completion is discarded and recording suspends derived requests", []
    {
        PlaybackWakeEvent entered, release;
        ThumbnailCache cache([&](const auto&, unsigned, const auto& points, const auto&)
        { entered.signal(); WaitForSingleObject(release.nativeHandle(), 5000); ThumbnailFrame t; t.sample = points[0]; t.rgb.resize(160 * 90 * 3); return std::vector<ThumbnailFrame>{std::move(t)}; });
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("synthetic.mp4");
        cache.setRecording(true); require(cache.request("old", file, 48000, 0), "Enqueue while suspended failed");
        require(WaitForSingleObject(entered.nativeHandle(), 10) == WAIT_TIMEOUT, "Thumbnail decoder ran during recording");
        cache.setRecording(false); require(WaitForSingleObject(entered.nativeHandle(), 1000) == WAIT_OBJECT_0, "Thumbnail worker did not resume");
        cache.invalidate(); release.signal(); eventually([&] { return cache.stats().stale == 1; });
        require(!cache.nearest("old", 0) && cache.stats().bytes == 0, "Old thumbnail result reappeared after invalidation");
    });
    return suite.result("seek-generation");
}
