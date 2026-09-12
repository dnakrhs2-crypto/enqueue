#include <juce_gui_extra/juce_gui_extra.h>
#include "TimelineView.h"
#include "TimelineView.automation.h"
#include "RecordView.h"
#include "ShortcutSettingsPanel.h"
#include "app/RecorderSession.h"
#include "model/SafeFileWrite.h"

namespace gocue::recorder
{
namespace
{
// Opt-in validation window. Real decoders/PCM run against a copied project;
// the clock and recording state are synthetic and no device is enumerated/opened.
struct ValidationOutput : IAudioOutput
{
    Sample sample = 0;
    AudioOutputInfo open(const AudioOutputConfig&) override { return {}; }
    void start(IAudioOutputClient&) override {}
    void close() noexcept override {}
    Sample latestOutputSample() const noexcept override { return sample; }
    juce::Result status() const override { return juce::Result::ok(); }
    void drainTiming() override {}
};
class TimelineUxWindow : public juce::DocumentWindow, private juce::Timer
{
public:
    TimelineUxWindow(juce::var config, std::function<void(int)> done)
        : DocumentWindow(ko("타임라인 입력 검증 · 장치 연결 없음"), Palette::background, closeButton), view(document),
          configuration(std::move(config)), completion(std::move(done))
    {
        if (configuration["project"].toString().isNotEmpty())
        {
            const auto result = document.openCheckpoint(juce::File(configuration["project"].toString()));
            if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString());
        }
        else document.adopt(makeTimelineUiFixture(), {}, {});
        settings.cameraEnabled = {true, true}; settings.physicalInputs = {0, 1};
        surface.addAndMakeVisible(view); surface.timeline = &view;
        setUsingNativeTitleBar(true); setContentNonOwned(&surface, true); setResizable(true, false);
        setResizeLimits(960, 640, 4096, 2160); centreWithSize(1180, 780);
        surface.recordTab.onClick = [this] { timeline = false; refresh(); };
        surface.timelineTab.onClick = [this] { timeline = true; refresh(); };
        surface.startButton.onClick = [this] { startRecording(); };
        surface.stopButton.onClick = [this] { stopRecording(); };
        surface.markerButton.onClick = [this] { view.edits.addMarker(); };
        surface.settingsButton.onClick = [this]
        {
            class Window : public juce::DocumentWindow
            {
            public:
                Window() : DocumentWindow(ko("설정 · 단축키"), Palette::background, closeButton) { setUsingNativeTitleBar(true); }
                void closeButtonPressed() override { setVisible(false); }
            };
            settingsWindow = std::make_unique<Window>(); settingsWindow->setContentOwned(new ShortcutSettingsPanel(settings), true);
            settingsWindow->centreAroundComponent(this, 650, 500); settingsWindow->setVisible(true);
        };
        view.transport.play.onClick = [this] { if (transport) transport->play(); };
        view.transport.stop.onClick = [this] { if (transport) transport->stop(); };
        view.transport.beginning.onClick = [this] { if (transport) transport->goToStart(); };
        view.onScrub = [this](Sample at, bool released) { cursor = at; if (transport) transport->scrub(at, released, qpcNow()); };
        view.onListeningChanged = [this] { rebuild = true; };
        view.onGlobalKey = [this](const juce::KeyPress& key, juce::Component* origin)
        {
            const auto command = shortcutCommand(settings.shortcuts, key, origin); if (!command) return false;
            switch (*command)
            {
                case RecorderCommand::recordStart: if (!recording) startRecording(); break;
                case RecorderCommand::recordStop: if (recording) stopRecording(); break;
                case RecorderCommand::playStop: if (transport && !recording) { if (transport->snapshot().state == TransportState::playing) transport->stop(); else transport->play(); } break;
                case RecorderCommand::split: view.invoke(TimelineAction::split); break;
                case RecorderCommand::marker: view.edits.addMarker(); break;
                default: break;
            }
            return true;
        };
        refresh(); view.zoomToFit(); setVisible(true);
        if (document.getFile() != juce::File()) preparePlayback();
        started = qpcNow(); startTimer(10);
    }
    ~TimelineUxWindow() override { stopTimer(); stopLive(); releasePlayback(); clearContentComponent(); }
    void closeButtonPressed() override { writeReport(); }
private:
    struct Surface : RecordView
    {
        TimelineView* timeline = nullptr;
        void resized() override { RecordView::resized(); if (timeline) timeline->setBounds(timelineBounds()); }
    };
    void releasePlayback() { if (renderer) renderer->stopWorker(); video.stop(); transport.reset(); renderer.reset(); }
    void preparePlayback()
    {
        releasePlayback(); const auto& p = document.getProject(); const auto folder = document.getFile().getParentDirectory();
        std::vector<PlaybackVideoClip> videos; std::vector<PlaybackAudioTrack> audioTracks; MediaIndex index;
        for (const auto& track : p.tracks)
        {
            PlaybackAudioTrack audio; audio.trackId = track.trackId; audio.mute = track.mute; audio.solo = track.solo;
            for (const auto& clip : track.clips.items()) if (p.isActive(clip))
            {
                const auto* asset = p.media->findAsset(clip.assetId); if (!asset) continue;
                RenderClip mapping; mapping.clipId = clip.clipId; mapping.trackId = clip.trackId; mapping.assetId = clip.assetId;
                mapping.sourceIn = clip.sourceIn; mapping.timelineStartSample = clip.timelineStartSample; mapping.lengthSamples = clip.lengthSamples; mapping.mediaGeneration = asset->mediaGeneration;
                if (asset->kind == AssetKind::camera)
                {
                    auto source = std::make_shared<VideoIndex>(*index.openVideo(folder.getChildFile(asset->relativePath), p.Fs));
                    source->epoch = std::make_shared<MediaEpoch>(); source->generation = std::uint64_t(asset->mediaGeneration); source->epoch->value.store(source->generation);
                    videos.push_back({mapping, track.kind == TrackKind::cam2 ? 1u : 0u, source});
                }
                else audio.clips.push_back({mapping, RecorderSession::indexRecordedAudio(*asset, folder, p.Fs, track.trackId)});
            }
            if (!audio.clips.empty()) audioTracks.push_back(std::move(audio));
        }
        renderer = std::make_unique<TimelineAudioRenderer>(p.Fs, 480); renderer->setPlan(std::move(audioTracks), p.activeTimelineEnd());
        transport = std::make_unique<TimelineTransport>(p.Fs, qpcFrequency(), renderer->queue(), p.activeTimelineEnd());
        video.prepare(std::move(videos)); const auto hosts = surface.nativeHosts();
        if (!bool(configuration["offscreenPlayback"]))
            for (unsigned i = 0; i < 2; ++i) if (hosts[i]) video.attachPlaybackView(i, hosts[i]);
        transport->seek(cursor); rebuild = false;
    }
    void startRecording()
    {
        if (recording) return;
        releasePlayback(); recording = true; recordStart = qpcNow(); placement = document.getProject().activeTimelineEnd();
        const auto hosts = surface.nativeHosts();
        for (unsigned i = 0; i < 2; ++i)
        {
            pools[i] = std::make_unique<VideoSurfacePool>(320, 180); liveTelemetry[i] = std::make_shared<CaptureTelemetry>(Rational{30, 1});
            live[i] = std::make_unique<PreviewPresenter>(static_cast<HWND>(hosts[i]), *pools[i], liveTelemetry[i], 320, 180); live[i]->start();
        }
        document.setRecordingStructureLock(true); refresh(); view.reveal(placement);
    }
    void stopLive()
    {
        for (unsigned i = 0; i < 2; ++i) if (live[i])
        { live[i]->stop(); presented[i] += unsigned(liveTelemetry[i]->presented.load()); consumed[i] += unsigned(pools[i]->snapshot().consumed); live[i].reset(); pools[i].reset(); }
    }
    void stopRecording() { stopLive(); recording = false; document.setRecordingStructureLock(false); if (document.getFile() != juce::File()) preparePlayback(); refresh(); }
    void refresh()
    {
        const auto& p = document.getProject(); const auto elapsed = recording ? Sample(double(qpcNow() - recordStart) / qpcFrequency() * p.Fs) : 0;
        auto ui = mapUiState(p, settings, recording ? TakeController::State::recording : TakeController::State::idle, recording, false, true, true);
        surface.update(ui, p, settings, recording ? ko("녹화 중") : ko("입력 검증"), ko("검증 창 · 녹화 상태와 출력 시계는 합성입니다"), elapsed, 100000000000, timeline);
        view.setVisible(timeline); view.setBounds(surface.timelineBounds());
        view.setRecordingPreview(recording, placement, elapsed, {{bool(live[0]), bool(live[1])}, {1, 2}}, settings);
        view.refresh(recording, recording ? placement + elapsed : cursor, {});
        view.transport.setState(!recording, transport && transport->snapshot().state == TransportState::playing, cursor, p.Fs);
        for (unsigned i = 0; i < 2; ++i)
        {
            if (recording && pools[i])
            {
                const auto slot = pools[i]->acquireWrite();
                if (slot != VideoSurfacePool::none)
                {
                    auto& frame = pools[i]->surface(slot); const auto number = ++liveFrames[i];
                    for (unsigned y = 0; y < frame.height; ++y) for (unsigned x = 0; x < frame.width; ++x)
                        frame.y()[y * frame.width + x] = std::uint8_t((x / 40) * 24 + 32 + (x >= number * 4 % 300 && x < number * 4 % 300 + 16 ? 35 : 0));
                    for (std::size_t n = 0; n < frame.nv12.size() / 3; n += 2) { frame.uv()[n] = i ? 180 : 96; frame.uv()[n + 1] = i ? 96 : 180; }
                    frame.stamp.frame = number; frame.stamp.callback = frame.stamp.normaliseEnd = qpcNow(); frame.stamp.generation = 1; pools[i]->publish(slot);
                }
                surface.setCamera(i, ko(i ? "캠2 · 합성 프리뷰 " : "캠1 · 합성 프리뷰 ") + juce::String(liveFrames[i]), {}, true); continue;
            }
            const auto selection = video.displaySelection(i);
            surface.setCamera(i, ko(i ? "캠2" : "캠1"), ko("영상 없음"), selection.frame != nullptr);
        }
    }
    void timerCallback() override
    {
        try
        {
            if (rebuild && document.getFile() != juce::File()) preparePlayback();
            const auto now = qpcNow();
            if (transport)
            {
                transport->service(*renderer, video, output, now);
                BlockStamp stamp{}; stamp.sampleRate = document.getProject().Fs; stamp.numSamples = 480; stamp.samplePosition = output.sample;
                stamp.callbackQpc = now; stamp.sequence = ++callbacks; stamp.flags = samplePositionValid | latenciesValid;
                float left[480]{}, right[480]{}; transport->processOutput(stamp, left, right); output.sample += 480;
                for (const auto value : left) audioEnergy += double(value) * value;
                cursor = TimelineTransport::audibleCursor(transport->snapshot(), document.getProject().Fs, qpcFrequency(), now);
            }
            const auto ms = juce::Time::getMillisecondCounter();
            if (!lastUi || ms - lastUi >= 33) { lastUi = !lastUi ? ms : lastUi + (ms - lastUi) / 33 * 33; refresh(); }
            if (view.rowPaintCount != lastPaintCount)
            { if (lastPaint) paintIntervals.push_back(double(now - lastPaint) * 1000 / qpcFrequency()); lastPaint = now; lastPaintCount = view.rowPaintCount; }
            const auto seconds = int(configuration["seconds"]);
            if (seconds > 0 && double(now - started) / qpcFrequency() >= seconds) writeReport();
        }
        catch (const std::exception& e) { failure = e.what(); writeReport(); }
    }
    void writeReport()
    {
        stopLive();
        stopTimer(); auto* report = new juce::DynamicObject(); juce::var result(report);
        if (transport && transport->status().failed()) failure = transport->status().getErrorMessage();
        const bool missingPresentation = document.getFile() != juce::File() && !bool(configuration["offscreenPlayback"]) && !video.lastPresentation(0).qpc;
        const bool incomplete = missingPresentation || (liveFrames[0] && (!presented[0] || !presented[1])) || (callbacks && audioEnergy == 0);
        report->setProperty("status", failure.isNotEmpty() ? "FAIL" : incomplete ? "PARTIAL" : "PASS"); report->setProperty("error", failure);
        report->setProperty("offscreenPlayback", bool(configuration["offscreenPlayback"]));
        if (incomplete) report->setProperty("limitation", "Native presentation was not acknowledged on this desktop; window painting alone does not certify live preview or audible playback.");
        report->setProperty("runId", configuration["runId"]); report->setProperty("captureDevicesOpened", 0);
        report->setProperty("syntheticOutputCallbacks", int(callbacks)); report->setProperty("renderedAudioEnergy", audioEnergy);
        if (transport) report->setProperty("transport", transport->telemetry());
        report->setProperty("rowPaintCount", int(view.rowPaintCount)); report->setProperty("video", video.telemetry());
        report->setProperty("cam1LivePresented", int(presented[0])); report->setProperty("cam2LivePresented", int(presented[1]));
        report->setProperty("cam1LiveConsumed", int(consumed[0])); report->setProperty("cam2LiveConsumed", int(consumed[1]));
        report->setProperty("editRevision", double(document.getProject().editRevision));
        if (!paintIntervals.empty())
        {
            std::sort(paintIntervals.begin(), paintIntervals.end()); double total = 0; unsigned late = 0;
            for (auto interval : paintIntervals) { total += interval; if (interval > 50) ++late; }
            report->setProperty("paintIntervalMeanMs", total / paintIntervals.size()); report->setProperty("paintIntervalP95Ms", paintIntervals[paintIntervals.size() * 95 / 100]);
            report->setProperty("paintIntervalsOver50Ms", int(late));
        }
        const auto written = gocue::SafeFileWrite::writeTextVerified(juce::File(configuration["report"].toString()), juce::JSON::toString(result, false));
        completion(written.wasOk() && failure.isEmpty() ? 0 : 1);
    }
    RecorderDocument document;
    Surface surface;
    TimelineView view;
    UserSettings settings;
    ValidationOutput output;
    VideoPlaybackEngine video;
    std::unique_ptr<TimelineAudioRenderer> renderer;
    std::unique_ptr<TimelineTransport> transport;
    std::array<std::unique_ptr<VideoSurfacePool>, 2> pools;
    std::array<std::shared_ptr<CaptureTelemetry>, 2> liveTelemetry;
    std::array<std::unique_ptr<PreviewPresenter>, 2> live;
    std::array<unsigned, 2> liveFrames{}, presented{}, consumed{};
    std::unique_ptr<juce::DocumentWindow> settingsWindow;
    juce::var configuration;
    std::function<void(int)> completion;
    Sample cursor = 0, placement = 0;
    std::int64_t started = 0, recordStart = 0, lastPaint = 0;
    std::uint32_t lastUi = 0;
    unsigned callbacks = 0, lastPaintCount = 0;
    std::vector<double> paintIntervals;
    double audioEnergy = 0;
    bool timeline = true, recording = false, rebuild = false;
    juce::String failure;
};
}
std::unique_ptr<juce::DocumentWindow> createTimelineUxWindow(const juce::var& config, std::function<void(int)> done)
{ return std::make_unique<TimelineUxWindow>(config, std::move(done)); }
}
