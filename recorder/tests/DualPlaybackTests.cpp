#include <juce_gui_extra/juce_gui_extra.h>
#include "ui/TimelineView.h"
#include "ui/RecordView.h"
#include "TestSupport.h"
#include "StabilityTestAccess.h"
#include "playback/TimelineTransport.h"
#include "ui/TimelineView.scale.h"
#include "support/Platform.h"
#include <chrono>
#include <thread>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
template<class F> void eventually(F f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) { require(std::chrono::steady_clock::now() < deadline, "Dual playback worker timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
std::shared_ptr<VideoIndex> source(unsigned fps)
{
    auto s = std::make_shared<VideoIndex>(); s->epoch = std::make_shared<MediaEpoch>(); s->timeBaseDen = int(fps); s->width = 1920; s->height = 1080;
    for (unsigned i = 0; i < fps * 12; ++i) s->packets.push_back({i, i, 100 + i * 1000, 1, 1000, i % fps == 0, i % fps == 0});
    s->validateAndBuild(); return s;
}
PlaybackVideoClip clip(std::shared_ptr<const VideoIndex> s, unsigned camera, Sample at, Sample in, Sample length)
{
    PlaybackVideoClip c; c.source = std::move(s); c.camera = camera; c.mapping.clipId = newId();
    c.mapping.mediaGeneration = Sample(c.source->generation); c.mapping.timelineStartSample = at; c.mapping.sourceIn = in; c.mapping.lengthSamples = length; return c;
}
struct Decoder : IVideoFrameDecoder
{
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t, const std::function<bool()>&) override { return {}; }
};
VideoPlaybackEngine::DecoderFactory factory() { return [](auto) { return std::make_unique<Decoder>(); }; }
struct Output : IAudioOutput
{
    Sample at = 0; unsigned callbacks = 0;
    AudioOutputInfo open(const AudioOutputConfig&) override { return {}; }
    void start(IAudioOutputClient&) override {}
    void close() noexcept override {}
    Sample latestOutputSample() const noexcept override { return at; }
    juce::Result status() const override { return juce::Result::ok(); }
    void drainTiming() override {}
    BlockStamp tick(TimelineTransport& transport)
    {
        BlockStamp s{}; s.flags = samplePositionValid | latenciesValid; s.sampleRate = 48000; s.numSamples = 480;
        s.samplePosition = at; s.callbackQpc = 1000000 + at * 1000 / 48; s.outputLatencySamples = 960;
        float left[480]{}, right[480]{}; transport.processOutput(s, left, right); at += 480; ++callbacks; return s;
    }
};
}
int runDualPlaybackTests()
{
    Suite suite;
    suite.test("alignment: visible block and playhead coordinates round trip after zoom and scroll", []
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        RecorderDocument document; document.newProject("alignment");
        require(document.setTimebase(48000, {60,1}).wasOk(), "Set alignment timebase");
        TimelineView view(document); view.setBounds(0, 0, 1280, 480);
        for (double factor : {1.0, .2, 5.0})
        {
            view.zoom(factor); view.reveal(480000);
            for (Sample t : {Sample{0}, Sample{1}, Sample{799}, Sample{800}, Sample{12345}, Sample{63840}, Sample{480479}, Sample{480480}})
            {
                view.refresh(false, t, {});
                const auto restored = StabilityTestAccess::sample(view, StabilityTestAccess::x(view, t));
                require(restored == t, "Block/playhead x coordinate changed its timeline sample");
            }
        }
    });
    suite.test("1000 dual seeks floor independent 30/60 fps sources at one timeline time", []
    {
        auto a = source(60), b = source(30);
        const auto ca = clip(a, 0, 24000, 800, 240000), cb = clip(b, 1, 24000, 48000, 240000);
        VideoPlaybackEngine video(factory()); video.prepare({ca, cb});
        std::uint64_t random = 26;
        for (std::uint64_t g = 1; g <= 1000; ++g)
        {
            random = random * 6364136223846793005ull + 1; const auto target = 24000 + Sample((random >> 1) % 240000);
            video.seek(target, g); eventually([&] { return video.ready(target, g); });
            for (unsigned camera = 0; camera < 2; ++camera)
            {
                const auto f = video.displaySelection(camera).frame; const auto& c = camera ? cb : ca;
                require(f && f->generation == g && f->clipId == c.mapping.clipId, "Wrong dual frame publication");
                require(f->pts == c.source->packets[c.source->frameAt(c.mapping.sourceIn + target - c.mapping.timelineStartSample)].pts, "Source frame mapping drift");
                require(f->begin <= target && target < f->end, "Containing frame must be half-open");
            }
        }
        const auto t = video.telemetry();
        for (int camera = 0; camera < 2; ++camera)
        { require(int(t["cameras"][camera]["peakReadyFrames"]) <= 3, "Display-ready queue exceeds 3"); require(int(t["cameras"][camera]["frameCacheFrames"]) <= 4, "Frame cache unbounded"); }
    });
    suite.test("internal gaps and shorter camera tails preserve the other source time", []
    {
        const auto s = source(60); auto a = clip(s, 0, 0, 0, 48000), b = clip(s, 1, 0, 48000, 24000);
        a.mapping.gaps = {{8000, 3200}};
        VideoPlaybackEngine video(factory()); video.prepare({a, b});
        for (const Sample target : {Sample{7999}, Sample{8000}, Sample{11199}, Sample{11200}, Sample{23999}, Sample{24000}, Sample{48000}})
        {
            const auto g = video.seek(target); eventually([&] { return video.ready(target, g); });
            const auto ca = video.displaySelection(0), cb = video.displaySelection(1);
            require(ca.gap == ((target >= 8000 && target < 11200) || target >= 48000), "Internal gap/tail containment wrong");
            require(cb.gap == (target >= 24000), "Camera tail silently extended");
            if (!cb.gap) require(cb.frame->pts == (48000 + target) / 800, "Other camera shifted to fill a gap");
        }
        rejects([&] { a.mapping.gaps = {{47000, 2000}}; video.prepare({a}); });
    });
    suite.test("ASIO audible cursor drives both lanes through one transport and underrun recovery", []
    {
        auto a = source(60), b = source(30);
        TimelineAudioRenderer audio(48000, 480); audio.setPlan({}, 96000);
        VideoPlaybackEngine video(factory()); video.prepare({clip(a, 0, 0, 0, 96000), clip(b, 1, 0, 0, 96000)});
        TimelineTransport transport(48000, 1000000, audio.queue(), 96000); Output output;
        transport.seek(0); transport.play();
        for (int i = 0; i < 80; ++i)
        {
            const auto stamp = output.tick(transport); transport.service(audio, video, output, stamp.callbackQpc);
            const auto t = video.telemetry();
            require(Sample(t["cameras"][0]["targetSample"]) == Sample(t["cameras"][1]["targetSample"]), "Cameras use separate cursors");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(output.callbacks == 80, "Video lanes duplicated the output callback");
        require(transport.status().wasOk(), "Common transport failed");
        const auto s = transport.snapshot(); const auto audible = TimelineTransport::audibleCursor(s, 48000, 1000000, s.callbackQpc);
        require(audible <= s.submittedEnd && audible >= s.timelineOrigin, "Audible cursor ignores output/software queue bounds");
        // Force a discontinuity after accepted samples. Callback remains nonblocking;
        // ordinary playback reprepares both cameras after the accepted tail drains.
        output.at += 480; const auto stamp = output.tick(transport);
        transport.service(audio, video, output, stamp.callbackQpc);
        require(transport.snapshot().underruns > 0, "Injected ASIO discontinuity did not enter buffering");
    });
    suite.test("revision handoff waits for callback acknowledgement and replaces video and audio length", []
    {
        const auto s = source(60);
        TimelineAudioRenderer audio(48000, 480); audio.setPlan({}, 96000);
        VideoPlaybackEngine video(factory()); video.prepare({clip(s, 0, 0, 0, 96000), clip(s, 1, 0, 0, 96000)});
        TimelineTransport transport(48000, 1000000, audio.queue(), 96000); Output output;
        transport.seek(0); auto stamp = output.tick(transport); transport.service(audio, video, output, stamp.callbackQpc);
        RecorderProject p; auto plan = std::make_shared<CompiledRenderPlan>(p, std::vector<RenderClip>{}); plan->timelineEnd = 48000;
        transport.stagePlan(plan, {}, {clip(s, 0, 0, 48000, 48000), clip(s, 1, 0, 96000, 48000)});
        const auto generation = transport.generation();
        transport.service(audio, video, output, stamp.callbackQpc);
        require(audio.length() == 96000, "Plan changed before callback quiescence");
        stamp = output.tick(transport); transport.service(audio, video, output, stamp.callbackQpc);
        eventually([&] { return video.ready(0, generation); });
        require(audio.length() == 48000 && video.displaySelection(1).frame->pts == 120, "A/V replacement mismatch");
        require(transport.status().wasOk(), "Plan handoff failed"); rejects([&] { transport.seek(48001); });
    });
    suite.test("10000 clips and long Korean names use visible intervals and logical DPI layout", []
    {
        const auto report = timelineScaleReport(10000);
        require(int(report["clipCount"]) == 10000 && int(report["maxVisibleClips"]) <= 24, "Viewport scanned all clips");
        require(int(report["binarySearchComparisons"]) < 120000, "Interval queries are not logarithmic");
        for (const auto& layout : *report["layouts"].getArray()) require(bool(layout["sideBySide"]), "DPI stacked the views");
        for (double dpi : {1.0, 1.5, 2.0})
        {
            const auto boxes = TimelineLayout::cameras(960, 250); const auto a = TimelineLayout::physical(boxes[0], dpi), b = TimelineLayout::physical(boxes[1], dpi);
            require(a.x >= 0 && b.x + b.width <= int(960 * dpi) && a.width == b.width, "DPI clipping or double scaling");
            require(std::abs(a.width * 9 - a.height * 16) <= 24, "16:9 fit changed across DPI");
        }
        const auto columns = TimelineLayout::waveColumns(300, 600, -100000, 1000000);
        require(columns.second - columns.first == 300, "Waveform work scales with source length");
        const auto rows = TimelineLayout::visibleRows(174, 318, 1000); require(rows.first == 2 && rows.second == 4, "Vertical track virtualization wrong");
        RecorderProject p; Track t;
        for (const auto start : {Sample{0}, Sample{100}, Sample{200}})
        { Clip c; c.trackId = t.trackId; c.timelineStartSample = start; c.lengthSamples = start ? 50 : 1000; t.clips.edit().push_back(std::move(c)); }
        TimelineVisibleIndex overlapping; overlapping.rebuild(p, {t});
        const auto ghosts = overlapping.visible(0, 800, 900);
        require(ghosts.size() && (*ghosts.begin())->timelineStartSample == 0, "Overlapping invalid preview hid an enclosing ghost");
    });
    suite.test("actual UI components lay out and paint 10000 clips at 960x640 without a native window", []
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        RecorderDocument document; document.newProject("r26");
        const auto longName = juce::String::fromUTF8("아주 긴 한글 장치 이름 · 스튜디오 카메라와 마이크 입력 1234567890");
        RecorderProject fixture = document.getProject();
        {
            auto& p = fixture;
            auto media = std::make_shared<MediaRegistry>(); p.tracks.clear();
            for (unsigned row = 0; row < 4; ++row)
            {
                MediaAsset a; a.kind = row < 2 ? AssetKind::camera : AssetKind::mic;
                a.relativePath = row < 2 ? "media/cam" + juce::String(row) + ".mp4" : "media/mic" + juce::String(row) + ".wav";
                a.contentIdentity = "r26-headless-" + a.assetId;
                a.mediaGeneration = 1; a.logicalLength = 2500 * 96000; a.availableRanges = {{0, a.logicalLength}};
                a.originalFormat.codec = row < 2 ? "h264" : "pcm_s24le"; a.originalFormat.width = row < 2 ? 1920 : 0; a.originalFormat.height = row < 2 ? 1080 : 0;
                a.originalFormat.sampleRate = row < 2 ? 0 : 48000; a.originalFormat.channels = row < 2 ? 0 : 1; a.originalFormat.bitsPerSample = row < 2 ? 0 : 24;
                a.originalFormat.fps = p.fps;
                a.sourceUnitsNumerator = row < 2 ? p.fps.numerator : 48000;
                a.sourceUnitsDenominator = 48000ull * (row < 2 ? p.fps.denominator : 1);
                Track t; t.kind = row == 0 ? TrackKind::cam1 : row == 1 ? TrackKind::cam2 : TrackKind::mic; t.name = longName;
                t.microphoneIndex = row < 2 ? -1 : int(row - 2);
                for (unsigned i = 0; i < 2500; ++i)
                { Clip c; c.trackId = t.trackId; c.assetId = a.assetId; c.timelineStartSample = c.sourceIn = Sample(i) * 96000; c.lengthSamples = 48000; t.clips.edit().push_back(std::move(c)); }
                media->assets.push_back(std::move(a)); p.tracks.push_back(std::move(t));
            }
            Take take; take.number = 1; take.createdAt = "2026-09-09T00:00:00Z"; take.state = TakeState::complete;
            take.logicalLength = 2500 * 96000; take.cam1AssetId = media->assets[0].assetId; take.cam2AssetId = media->assets[1].assetId;
            take.microphoneAssetIds = {media->assets[2].assetId, media->assets[3].assetId}; take.capture.physicalInputs = {0, 1};
            media->takes.push_back(std::move(take)); p.media = std::move(media);
        }
        const auto edit = document.adopt(std::move(fixture), {}, {});
        require(edit.wasOk(), edit.getErrorMessage().toRawUTF8());
        RecordView root; TimelineView timeline(document); root.addAndMakeVisible(timeline);
        root.update({}, document.getProject(), {}, {}, {}, 0, 0, true);
        root.setCamera(0, longName, juce::String::fromUTF8("영상 없음"), false);
        root.setCamera(1, longName, juce::String::fromUTF8("영상 없음"), false);
        root.setSize(960, 640); timeline.setBounds(root.timelineBounds()); timeline.refresh(false, 120000000, {});
        // Minimal reveal keeps the requested sample near the right edge. Leave
        // enough visible room for both trial ghost positions used below.
        timeline.reveal(120144000);
        for (const auto& asset : document.getProject().media->assets) if (asset.kind == AssetKind::mic)
        {
            PeakSnapshot peaks; peaks.sampleRate = 48000; peaks.channels = 1; peaks.samples = std::uint64_t(asset.logicalLength); peaks.samplesPerBin = 16384; peaks.complete = true;
            peaks.bins.resize(16384); for (auto& bin : peaks.bins) bin[0] = {-.6f, .7f}; timeline.setLoadedPeaks(asset.assetId, std::move(peaks), 0);
        }
        std::vector<juce::Rectangle<int>> hosts;
        std::function<void(juce::Component&)> inspect = [&](juce::Component& c)
        {
            if (auto* host = dynamic_cast<juce::HWNDComponent*>(&c))
            { require(host->getHWND() == nullptr, "Headless test unexpectedly created HWND"); hosts.push_back(root.getLocalArea(host, host->getLocalBounds())); }
            if (auto* label = dynamic_cast<juce::Label*>(&c); label && label->getText() == longName)
                require(label->getTooltip() == longName && label->getWidth() <= TimelineLayout::headerWidth, "Long track label lost bounded text/full tooltip");
            for (auto* child : c.getChildren()) inspect(*child);
        };
        inspect(root); require(hosts.size() == 2 && hosts[0].getRight() < hosts[1].getX() && hosts[0].getY() == hosts[1].getY(), "Actual camera cards stacked/overlapped");
        timeline.revealTrack(2);
        for (double dpi : {1.0, 1.5, 2.0})
        {
            juce::Image picture(juce::Image::ARGB, int(960 * dpi), int(640 * dpi), true, juce::SoftwareImageType{});
            juce::Graphics graphics(picture); graphics.addTransform(juce::AffineTransform::scale(float(dpi)));
            root.paintEntireComponent(graphics, true);
            require(picture.isValid() && picture.getWidth() == int(960 * dpi) && picture.getHeight() == int(640 * dpi), "DPI applied twice or snapshot clipped");
            require(timeline.lastPaintVisitedClips > 0 && timeline.lastPaintVisitedClips <= 44, "Real paint traversed invisible clips");
            require(timeline.lastPaintWaveColumns > 0 && timeline.lastPaintWaveColumns <= 960 * 2, "Waveform work exceeds visible columns");
        }
        // Exercise the production preview paint path as well as its normal rows.
        const auto normalVisits = timeline.lastPaintVisitedClips;
        const auto& moving = document.getProject().tracks[2].clips.items()[1250];
        timeline.edits.clickClip(moving.clipId); require(timeline.edits.beginDrag(TimelineAction::move), "Headless clip drag did not begin");
        for (const auto delta : {Sample{48000}, Sample{72000}})
        {
            const auto* preview = timeline.edits.dragTo(moving.timelineStartSample + delta, true);
            require(preview && (preview->status.wasOk() == (delta == 48000)), "Drag fixture did not exercise valid and overlapping previews");
            timeline.selectionChanged();
            juce::Image picture(juce::Image::ARGB, 960, 640, true, juce::SoftwareImageType{}); juce::Graphics graphics(picture);
            root.paintEntireComponent(graphics, true);
            require(timeline.lastPaintVisitedClips == normalVisits + 1, "Preview paint traversed invisible clips or lost the visible ghost");
        }
        timeline.edits.cancelDrag(); timeline.selectionChanged();
        require(root.getPeer() == nullptr, "DPI layout test opened a native window");
    });
    return suite.result("dual-playback");
}
