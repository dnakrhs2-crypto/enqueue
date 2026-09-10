#pragma once
#include "AudioRenderFixtures.h"
#include "TestSupport.h"
#include "model/ClipEdits.h"
#include "playback/VideoPlaybackEngine.h"
#include "support/Platform.h"
#include <chrono>
#include <thread>

namespace recorder_cut_seam
{
using namespace gocue::recorder;
using recorder_test::require;
inline const bool flushDiagnostics = []
{
    if (juce::SystemStats::getEnvironmentVariable("RECORDER_CUT_SEAM_FLUSH_TEST_OUTPUT", {}).isNotEmpty()) std::cout << std::unitbuf;
    return true;
}();
constexpr Sample step = 800, seam = 48000, sourceIn = 110400;
template<class F> void awaitFrame(F check)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!check())
    {
        require(std::chrono::steady_clock::now() < end, "Cut seam preparation timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct DecodeStats { std::atomic<unsigned> prefixes{0}, cancelled{0}; };
struct DelayedDecoder final : IVideoFrameDecoder
{
    std::shared_ptr<const VideoIndex> source;
    std::shared_ptr<DecodeStats> stats;
    std::optional<std::size_t> last;
    DelayedDecoder(std::shared_ptr<const VideoIndex> s, std::shared_ptr<DecodeStats> t) : source(std::move(s)), stats(std::move(t)) {}
    void resetForSeek() override { last.reset(); }
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t packet, const std::function<bool()>& cancelled) override
    {
        const auto plan = playbackDecodePlan(*source, packet, last);
        const bool prefix = plan.fromIdr && plan.decodeOnlyFrames && source->packets[packet].sample >= sourceIn;
        if (prefix) ++stats->prefixes;
        // 80ms models a seek + IDR prefix longer than a 60Hz cursor request.
        // Completed sequential frames cost 1ms; no GPU/device is opened.
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(prefix ? 80 : 1);
        while (std::chrono::steady_clock::now() < end)
        {
            if (cancelled()) { ++stats->cancelled; return {}; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        last = packet; return {};
    }
};
inline std::shared_ptr<VideoIndex> syntheticIndex()
{
    auto s = std::make_shared<VideoIndex>(); s->epoch = std::make_shared<MediaEpoch>();
    s->width = 1920; s->height = 1080;
    for (unsigned i = 0; i < 600; ++i) s->packets.push_back({i, i, 100 + i * 1000, 1, 1000, i % 60 == 0, i % 60 == 0});
    s->validateAndBuild(); return s;
}
inline RecorderProject checked(ClipEditResult edit)
{
    require(edit.status.wasOk(), edit.status.getErrorMessage().toRawUTF8());
    return std::move(edit.project);
}
inline std::vector<Id> containing(const RecorderProject& p, Sample at)
{
    std::vector<Id> ids;
    for (const auto& t : p.tracks) for (const auto& c : t.clips.items())
        if (p.isActive(c) && c.timelineStartSample <= at && at < c.timelineEnd()) ids.push_back(c.clipId);
    return ids;
}
inline RecorderProject edited(const RecorderProject& original, unsigned mode)
{
    auto p = original;
    if (mode == 2) p = checked(ClipEdits::rippleDeleteAll(p, {seam, sourceIn - seam}));
    else
    {
        p = checked(ClipEdits::split(p, containing(p, seam), seam));
        p = checked(ClipEdits::split(p, containing(p, sourceIn), sourceIn));
        p = checked(ClipEdits::remove(p, containing(p, seam)));
        p = checked(ClipEdits::move(p, containing(p, sourceIn), seam - sourceIn));
        if (mode == 1)
        {
            // A second independent source file placed against the first on cam1.
            auto media = std::make_shared<MediaRegistry>(*p.media);
            auto asset = *media->findAsset(original.tracks[0].clips.items()[0].assetId);
            asset.assetId = newId(); asset.relativePath = "media/takes/second/cam1.mp4";
            asset.contentIdentity = "independent-second-camera-file";
            Take take; take.number = 2; take.createdAt = "2026-09-10"; take.state = TakeState::complete;
            take.logicalLength = asset.logicalLength; take.cam1AssetId = asset.assetId;
            media->assets.push_back(asset); media->takes.push_back(take); p.media = media;
            for (auto& c : p.tracks[0].clips.edit()) if (c.timelineStartSample == seam)
                c.assetId = asset.assetId;
        }
    }
    return p;
}
inline void writeReport(const juce::String& name, const juce::String& csv, const juce::var& summary)
{
    const auto directory = juce::SystemStats::getEnvironmentVariable("RECORDER_CUT_SEAM_REPORT_DIR", {});
    if (directory.isEmpty()) return;
    const juce::File root(directory); require(root.createDirectory().wasOk(), "Create seam report directory");
    require(root.getChildFile(name + ".csv").replaceWithText(csv), "Write seam frame trace");
    require(root.getChildFile(name + ".json").replaceWithText(juce::JSON::toString(summary)), "Write seam summary");
}
inline void run(unsigned mode, bool realMedia)
{
    recorder_audio_fixture::Fixture fixture;
    const auto project = edited(fixture.project, mode);
    const auto plan = RenderPlanCompiler::compile(project);
    MediaIndex index;
    std::array<std::shared_ptr<const VideoIndex>, 2> sources;
    const auto indexBegin = qpcNow();
    if (realMedia)
    {
        const juce::File directory(juce::SystemStats::getEnvironmentVariable("RECORDER_CUT_SEAM_MEDIA", {}));
        sources[0] = index.openVideo(directory.getChildFile("cam1.mp4"), 48000);
        sources[1] = index.openVideo(directory.getChildFile("cam-second.mp4"), 48000);
    }
    else sources = {syntheticIndex(), syntheticIndex()};
    const double indexMs = 1000.0 * (qpcNow() - indexBegin) / qpcFrequency();
    std::vector<PlaybackVideoClip> clips;
    for (const auto& c : plan->activeClips) if (c.trackId == project.tracks[0].trackId)
    {
        const auto which = c.assetId == fixture.project.tracks[0].clips.items()[0].assetId ? 0 : 1;
        auto mapping = c; mapping.mediaGeneration = Sample(sources[which]->generation);
        // The original fixture may have a shorter finalized tail than 10s.
        mapping.lengthSamples = (std::min)(mapping.lengthSamples, sources[which]->length - mapping.sourceIn);
        clips.push_back({mapping, 0, sources[which]});
    }
    require(clips.size() == 2 && clips[0].mapping.timelineStartSample + clips[0].mapping.lengthSamples == seam
        && clips[1].mapping.timelineStartSample == seam && clips[1].mapping.sourceIn == sourceIn, "Compiler introduced a gap/source offset at cut");
    auto stats = std::make_shared<DecodeStats>();
    VideoPlaybackEngine::DecoderFactory factory;
    if (!realMedia) factory = [stats](auto source) { return std::make_unique<DelayedDecoder>(source, stats); };
    TimelineAudioRenderer audio(48000, unsigned(step));
    auto bindings = openAudioSources(*compileAudioRenderPlan(project), fixture.root);
    std::shared_ptr<const PlaybackAudioSource> realAudio;
    if (realMedia)
    {
        const juce::File directory(juce::SystemStats::getEnvironmentVariable("RECORDER_CUT_SEAM_MEDIA", {}));
        const auto root = directory.getParentDirectory().getParentDirectory().getParentDirectory();
        const auto recorded = index.openWavJournal(root, juce::Uuid(directory.getFileName()));
        require(!recorded.empty(), "Copied real project has no finalized PCM");
        realAudio = wavAudioSource(recorded.front());
        for (auto& binding : bindings) binding.source = realAudio;
    }
    const AudioSourceMask mask{AudioSourceMask::Kind::materialTrack, project.tracks[2].trackId, {}};
    audio.setPlan(plan, bindings, mask);
    const Sample start = seam - 24 * step;
    const auto stopped = qpcNow();
    VideoPlaybackEngine video(factory); video.prepare(clips); video.seek(start, 1); audio.prepare(start, 1);
    awaitFrame([&] { require(video.status().wasOk(), video.status().getErrorMessage().toRawUTF8()); return video.ready(start, 1) && audio.ready(); });
    const double firstReadyMs = 1000.0 * (qpcNow() - stopped) / qpcFrequency();
    PlaybackDisplayState display;
    unsigned missing = 0, black = 0, held = 0, orderErrors = 0, wrongPts = 0, audioUnderruns = 0, audioErrors = 0;
    Sample previousBegin = -1;
    double boundaryDelayMs = -1;
    juce::String csv = "relativeFrame,timelineSample,expectedSourceSample,expectedPts,selectedPts,displayedPts,displayedBegin,source,black,held,arrivalDelayMs\n";
    const auto clockStart = std::chrono::steady_clock::now();
    for (int frame = -24; frame <= 10; ++frame)
    {
        std::this_thread::sleep_until(clockStart + std::chrono::microseconds((frame + 24) * 1000000 / 60));
        const auto sample = seam + frame * step;
        video.requestFrames(sample, 1, true);
        const auto selection = video.displaySelection(0, true);
        const auto picture = display.select(selection);
        display.submitted(picture); // synthetic output receipt; never a DXGI claim
        if (selection.frame) video.presented(0, *selection.frame, qpcNow());
        const auto& c = clips[frame >= 0 ? 1 : 0];
        const auto expectedSource = c.mapping.sourceIn + sample - c.mapping.timelineStartSample;
        const auto expectedPts = c.source->packets[c.source->frameAt(expectedSource)].pts;
        const auto arrival = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clockStart).count() - 400.0;
        if (frame >= 0 && selection.frame && selection.frame->clipId == clips[1].mapping.clipId && boundaryDelayMs < 0) boundaryDelayMs = arrival;
        if (frame >= -10)
        {
            missing += !selection.frame; black += !picture; held += picture && !selection.frame;
            orderErrors += picture && picture->begin < previousBegin;
            wrongPts += selection.frame && (selection.frame->pts != expectedPts || selection.frame->source != c.source);
            if (picture) previousBegin = picture->begin;
            csv += juce::String(frame) + "," + juce::String(sample) + "," + juce::String(expectedSource) + "," + juce::String(expectedPts) + ","
                + (selection.frame ? juce::String(selection.frame->pts) : "empty") + "," + (picture ? juce::String(picture->pts) : "empty") + ","
                + (picture ? juce::String(picture->begin) : "empty") + "," + (picture && picture->source == sources[0] ? "A" : picture ? "B" : "empty")
                + "," + juce::String(int(!picture)) + "," + juce::String(int(picture && !selection.frame)) + "," + juce::String(arrival, 3) + "\n";
        }
        float left[step]{}, right[step]{};
        if (!audio.queue().consume(sample, 1, left, right, unsigned(step))) ++audioUnderruns;
        float realLeft[step]{}, realRight[step]{};
        if (realAudio) realAudio->read(sample < seam ? sample : sourceIn + sample - seam, unsigned(step), realLeft, realRight);
        for (Sample i = 0; i < step; ++i)
        {
            const auto at = sample + i, sourceSample = at < seam ? at : sourceIn + at - seam;
            float gain = 1;
            if (at >= seam - 144 && at < seam) gain = float(seam - 1 - at) / 143.0f;
            if (at >= seam && at < seam + 144) gain = float(at - seam) / 143.0f;
            const float expectedLeft = (realAudio ? realLeft[i] : fixture.sample(0, sourceSample)) * gain;
            const float expectedRight = (realAudio ? realRight[i] : fixture.sample(0, sourceSample)) * gain;
            if (left[i] != expectedLeft || right[i] != expectedRight) ++audioErrors;
        }
    }
    auto summary = jsonObject();
    jsonSet(summary, "measurement", realMedia ? "real H264 D3D11VA decode; synthetic present and PCM sink; no HWND/ASIO/capture" : "80ms IDR prefix fixture; production engine/picture policy and PCM queue; no devices");
    jsonSet(summary, "frames", 21); jsonSet(summary, "missingExactFrames", missing); jsonSet(summary, "blackSubmissions", black); jsonSet(summary, "heldFrames", held);
    jsonSet(summary, "orderErrors", orderErrors); jsonSet(summary, "wrongPts", wrongPts); jsonSet(summary, "boundaryDelayMs", boundaryDelayMs);
    jsonSet(summary, "firstReadyMs", firstReadyMs); jsonSet(summary, "indexMs", indexMs);
    jsonSet(summary, "audioUnderruns", audioUnderruns); jsonSet(summary, "audioOracleErrors", audioErrors);
    jsonSet(summary, "prefixAttempts", stats->prefixes.load()); jsonSet(summary, "prefixCancellations", stats->cancelled.load()); jsonSet(summary, "engine", video.telemetry());
    const juce::String name = (realMedia ? "real-" : "synthetic-") + juce::String(mode);
    writeReport(name, csv, summary);
    std::cout << "SEAM " << name << " black=" << black << " missing=" << missing << " held=" << held << " delayMs=" << boundaryDelayMs << " firstReadyMs=" << firstReadyMs << '\n';
    require(black == 0 && missing == 0 && orderErrors == 0 && wrongPts == 0, "Cut seam lost an exact ordered picture (see seam CSV)");
    require(audioUnderruns == 0 && audioErrors == 0, "Cut audio differs from PCM/MicroFade oracle");
    require(firstReadyMs < 2000, "First frame preparation exceeded 2 seconds");
    require(int(video.telemetry()["cameras"][0]["peakReadyFrames"]) <= int(VideoPlaybackEngine::maximumReadyFrames), "Unbounded cut preroll pictures");
}
inline void scrub()
{
    auto source = syntheticIndex(); auto stats = std::make_shared<DecodeStats>();
    PlaybackVideoClip a; a.source = source; a.mapping.clipId = newId(); a.mapping.mediaGeneration = 1; a.mapping.lengthSamples = seam;
    auto b = a; b.mapping.clipId = newId(); b.mapping.timelineStartSample = seam; b.mapping.sourceIn = sourceIn; b.mapping.lengthSamples = seam;
    VideoPlaybackEngine video([stats](auto s) { return std::make_unique<DelayedDecoder>(s, stats); });
    video.prepare({a, b}); video.seek(seam - 12 * step, 1); awaitFrame([&] { return video.ready(seam - 12 * step, 1); });
    PlaybackDisplayState display; display.submitted(display.select(video.displaySelection(0)));
    unsigned black = 0, held = 0; std::uint64_t generation = 1;
    juce::String csv = "relativeFrame,timelineSample,generation,displayedPts,black,held\n";
    for (int direction : {1, -1}) for (int n = -10; n <= 10; ++n)
    {
        const int relative = n * direction; const auto target = seam + relative * step;
        video.seek(target, ++generation);
        const auto selection = video.displaySelection(0); const auto picture = display.select(selection);
        black += !picture; held += picture && !selection.frame; display.submitted(picture);
        csv += juce::String(relative) + "," + juce::String(target) + "," + juce::String(generation) + ","
            + (picture ? juce::String(picture->pts) : "empty") + "," + juce::String(int(!picture)) + "," + juce::String(int(picture && !selection.frame)) + "\n";
        awaitFrame([&] { return video.ready(target, generation); });
        const auto exact = video.displaySelection(0); display.submitted(display.select(exact));
        require(exact.frame && exact.frame->pts == source->packets[source->frameAt(relative < 0 ? target : sourceIn + relative * step)].pts, "Scrub release PTS is not sourceIn-containing frame");
    }
    auto report = jsonObject(); jsonSet(report, "blackSubmissions", black); jsonSet(report, "heldFrames", held); jsonSet(report, "scrubRequests", 42);
    writeReport("scrub", csv, report);
    std::cout << "SEAM scrub black=" << black << " held=" << held << '\n';
    require(black == 0, "Scrub clears the previous picture before the new frame is ready");
}
inline void addTests(recorder_test::Suite& suite)
{
    suite.test("cut seam: both camera prerolls cancel on a new source epoch without poisoning the new plan", []
    {
        auto old = syntheticIndex(); auto fresh = syntheticIndex(); fresh->generation = 2; fresh->epoch->value = 2;
        auto stats = std::make_shared<DecodeStats>(); std::vector<PlaybackVideoClip> clips;
        for (unsigned camera = 0; camera < 2; ++camera)
        {
            PlaybackVideoClip a; a.source = old; a.camera = camera; a.mapping.clipId = newId(); a.mapping.mediaGeneration = 1; a.mapping.lengthSamples = seam;
            auto b = a; b.mapping.clipId = newId(); b.mapping.timelineStartSample = seam; b.mapping.sourceIn = sourceIn;
            clips.push_back(a); clips.push_back(b);
        }
        VideoPlaybackEngine video([stats](auto s) { return std::make_unique<DelayedDecoder>(s, stats); });
        video.prepare(clips); video.seek(seam - step, 1);
        awaitFrame([&] { return stats->prefixes >= 2; });
        ++old->epoch->value;
        for (auto& c : clips) { c.source = fresh; c.mapping.mediaGeneration = 2; }
        video.handoff(clips, 1600, 2); awaitFrame([&] { return video.ready(1600, 2); });
        require(video.status().wasOk() && stats->cancelled >= 2, "Old preroll failed to cancel cleanly");
        for (unsigned camera = 0; camera < 2; ++camera)
        {
            const auto f = video.displaySelection(camera).frame;
            require(f && f->source == fresh && f->generation == 2 && f->pts == 2, "Old prefix published over the replacement lane");
        }
    });
    suite.test("cut seam: true gap stays empty while its upcoming first picture is prepared", []
    {
        auto source = syntheticIndex(); auto stats = std::make_shared<DecodeStats>();
        PlaybackVideoClip a; a.source = source; a.mapping.clipId = newId(); a.mapping.mediaGeneration = 1; a.mapping.lengthSamples = 1600;
        auto b = a; b.mapping.clipId = newId(); b.mapping.timelineStartSample = seam; b.mapping.sourceIn = sourceIn; b.mapping.lengthSamples = seam;
        VideoPlaybackEngine video([stats](auto s) { return std::make_unique<DelayedDecoder>(s, stats); }); video.prepare({a, b});
        video.seek(0, 1); awaitFrame([&] { return video.ready(0, 1); });
        PlaybackDisplayState display; display.submitted(display.select(video.displaySelection(0)));
        video.requestFrames(36000, 1, true);
        require(video.displaySelection(0).gap && !display.select(video.displaySelection(0)), "Preroll painted into a real gap");
        awaitFrame([&] { return video.ready(seam, 1); });
        require(video.displaySelection(0).gap && !display.select(video.displaySelection(0)), "Decoded next frame filled the gap early");
        video.requestFrames(seam, 1, true); const auto selection = video.displaySelection(0);
        require(selection.frame && selection.frame->pts == sourceIn / step && display.select(selection), "Gap exit discarded its preroll");
    });
    suite.test("cut seam: hold policy preserves startup, real gaps and source epoch invalidation", []
    {
        auto source = syntheticIndex();
        auto f = std::make_shared<PlaybackVideoFrame>(); f->source = source; f->generation = 1;
        PlaybackDisplayState display; PlaybackDisplaySelection selection; selection.gap = false; selection.generation = 1;
        require(!display.select(selection), "Startup invented a picture");
        display.submitted(f); selection.generation = 2;
        require(display.select(selection) == f && !f->current(2), "Hold changed old frame publication rights");
        selection.gap = true; require(!display.select(selection), "Real gap retained old pixels");
        selection.gap = false; require(!display.select(selection), "Picture resurrected after an explicit gap");
        display.submitted(f); ++source->epoch->value;
        require(!display.select(selection), "Replaced file epoch retained old pixels");
    });
    suite.test("cut seam: fractional sourceIn uses the containing PTS and exact half-open boundary", []
    {
        auto source = syntheticIndex(); auto stats = std::make_shared<DecodeStats>();
        PlaybackVideoClip a; a.source = source; a.mapping.clipId = newId(); a.mapping.mediaGeneration = 1; a.mapping.lengthSamples = 1601;
        auto b = a; b.mapping.clipId = newId(); b.mapping.timelineStartSample = 1601; b.mapping.sourceIn = 49001; b.mapping.lengthSamples = 4800;
        VideoPlaybackEngine video([stats](auto s) { return std::make_unique<DelayedDecoder>(s, stats); }); video.prepare({a, b});
        for (Sample at : {Sample{1600}, Sample{1601}, Sample{2199}, Sample{2200}})
        {
            const auto gen = video.seek(at); awaitFrame([&] { return video.ready(at, gen); });
            const auto f = video.displaySelection(0).frame;
            const auto expected = at < 1601 ? at : 49001 + at - 1601;
            require(f && f->pts == source->packets[source->frameAt(expected)].pts && f->begin <= at && at < f->end,
                "Fractional cut rounded into the next source frame or introduced an empty sample");
        }
    });
    suite.test("cut seam: split-delete-move same source, 80ms prefix, +/-10 exact frames and PCM", [] { run(0, false); });
    suite.test("cut seam: adjacent independent files, 80ms prefix, +/-10 exact frames and PCM", [] { run(1, false); });
    suite.test("cut seam: ripple range delete, 80ms prefix, +/-10 exact frames and PCM", [] { run(2, false); });
    suite.test("cut seam: forward/reverse scrub holds picture until exact release", [] { scrub(); });
    if (juce::SystemStats::getEnvironmentVariable("RECORDER_CUT_SEAM_MEDIA", {}).isNotEmpty())
        for (unsigned mode = 0; mode < 3; ++mode) suite.test("cut seam: copied real H264, headless output", [mode] { run(mode, true); });
}
}
