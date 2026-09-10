#include "TestSupport.h"
#include "AudioRenderFixtures.h"
#include "ui/UiState.h"
#include "media/PeakCache.h"
#include "media/ThumbnailCache.h"
#include "playback/TimelineTransport.h"
#include "app/RecorderSession.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
void waitUntil(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(predicate(), "Worker timeout");
}
}
int runUiWiringTests()
{
    Suite suite;
    suite.test("Stereo settings round trip, legacy mono defaults and right-channel validation", []
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("stereo-settings-" + newId());
        {
            RecorderSettings settings(root); UserSettings value; value.physicalInputs = {0, 2}; value.stereoSlots[0] = true;
            require(settings.set(value).wasOk() && settings.save().get().wasOk(), "Save stereo settings");
            RecorderSettings loaded(root); require(loaded.load().wasOk() && loaded.get().stereoSlots[0] && !loaded.get().stereoSlots[1], "Stereo round trip");
            value.physicalInputs[1] = 1; require(value.validate().failed(), "Right reused by mono");
            value.physicalInputs = {0}; require(value.validate(1).failed() && value.validate(2).wasOk(), "Device right-channel bound");
            value.physicalInputs = {-1}; require(value.validate().failed(), "Unselected stereo slot rejected");
            value.physicalInputs = {255}; require(value.validate().failed(), "Right channel overflow rejected");
            auto xml = juce::parseXML(settings.getFile()); require(xml != nullptr, "Settings XML");
            juce::PropertySet props; props.restoreFromXml(*xml);
            for (int i = 0; i < 8; ++i) props.removeValue("stereoSlot" + juce::String(i));
            require(settings.getFile().replaceWithText(props.createXml("RECORDER_SETTINGS")->toString()), "Legacy settings fixture");
            require(loaded.load().wasOk() && !loaded.get().stereoSlots[0] && loaded.get().physicalInputs == std::vector<int>({0,2}), "Missing fields stay mono");
        }
        root.deleteRecursively();
    });
    suite.test("recording state keeps live views and locks structure / transport", []
    {
        RecorderProject p; UserSettings s;
        for (const auto state : {TakeController::State::preparing, TakeController::State::armed, TakeController::State::recording, TakeController::State::stopping})
        { const auto ui = mapUiState(p, s, state, true, false, true, true); require(ui.live && ui.structureLocked && !ui.canRecord && !ui.canTransport, "Recording lock lost"); }
        const auto ui = mapUiState(p, s, TakeController::State::recording, true, false, true, true);
        require(ui.takeStatus == juce::String::fromUTF8("녹화 중"), "Live label lost");
    });
    suite.test("camera2 off and zero microphones warn but permit camera recording", []
    {
        RecorderProject p; UserSettings s;
        const auto ui = mapUiState(p, s, TakeController::State::idle, false, false, true, true);
        require(ui.camera2Off && ui.armedMicrophones == 0 && ui.canRecord && !ui.timebaseFixed, "No-mic flow incorrectly blocked");
        require(ui.warning == juce::String::fromUTF8("녹음 중인 마이크가 없습니다"), "Warning missing");
        s.physicalInputs = {-1, 3}; s.microphoneArmed[1] = true;
        require(mapUiState(p, s, TakeController::State::idle, false, false, true, true).armedMicrophones == 1, "Sparse mapping");
    });
    suite.test("audio settings reject duplicate stereo and allow explicit mono", []
    {
        RecorderProject p; UserSettings s; RecorderAudioEngine::DeviceInfo d;
        s.asioDeviceId = d.name = "fixture"; d.sampleRate = 48000; d.physicalInputs = 2; d.physicalOutputs = 2;
        s.output.left = s.output.right = 0; require(validateAudioSettings(s, d, p).failed(), "Duplicate L/R accepted");
        s.output.mono = true; s.output.monoChannel = 0; require(validateAudioSettings(s, d, p).wasOk(), "Mono rejected");
        s.physicalInputs = {1, 1}; require(validateAudioSettings(s, d, p).failed(), "Duplicate input accepted");
        s.physicalInputs = {2}; require(validateAudioSettings(s, d, p).failed(), "Missing input accepted");
        s.physicalInputs = {-1, 1}; require(validateAudioSettings(s, d, p).wasOk(), "Sparse input rejected");
        auto registry = std::make_shared<MediaRegistry>(); registry->assets.emplace_back(); p.media = registry; d.sampleRate = 44100;
        require(validateAudioSettings(s, d, p).failed(), "Fixed project Fs mismatch accepted");
    });
    suite.test("camera panels validate identity, mode and duplicate slots", []
    {
        CameraDevice c; c.symbolicLink = "fixture"; c.modes.push_back(CameraMode::parse("NV12 1920x1080 30/1"));
        UserSettings s; s.cameraEnabled = {true, true}; s.cameraDeviceIds = {"fixture", "fixture"}; s.cameraModes = {juce::String(c.modes[0].text()), juce::String(c.modes[0].text())};
        require(validateCameraSettings(s, {c}).failed(), "Duplicate camera accepted"); s.cameraEnabled[1] = false;
        require(validateCameraSettings(s, {c}).wasOk(), "One camera rejected"); s.cameraModes[0] = "NV12 1920x1080 60/1";
        require(validateCameraSettings(s, {c}).failed(), "Unenumerated mode accepted");
    });
    suite.test("peaks preserve signed extrema, channel identity, partial bins and stop tail", []
    {
        PeakCache cache(48000, 2, 3);
        const std::int32_t pcm[] = {-8388608, 0, 4194304, 8388607, -1, -4194304, 8388607, -8388608};
        cache.append(pcm, 2, 0); cache.append(pcm + 4, 2, 2); const auto immediate = cache.snapshot();
        require(!immediate.complete && immediate.samples == 4 && immediate.bins.size() == 2, "Partial waveform unavailable");
        require(immediate.bins[0][0].minimum == -1 && immediate.bins[0][0].maximum == .5f && immediate.bins[0][1].minimum == -.5f, "Peak channel/extrema mismatch");
        cache.finish(); require(cache.snapshot().complete, "Tail not complete"); rejects([&] { cache.append(pcm, 1, 4); });
    });
    suite.test("bounded peak compaction retains every sample and early transients", []
    {
        PeakCache cache(48000, 1, 1); std::vector<std::int32_t> pcm(PeakCache::maximumBins * 3, 17); pcm[0] = -8388608; pcm[1] = 8388607;
        cache.append(pcm.data(), unsigned(pcm.size()), 0); const auto s = cache.snapshot();
        require(s.bins.size() <= PeakCache::maximumBins && s.samples == pcm.size() && s.samplesPerBin == 4, "Peak memory/duration cap");
        require(s.bins[0][0].minimum == -1 && s.bins[0][0].maximum > .99f, "Compaction lost transient");
        rejects([&] { cache.append(pcm.data(), 1, 1); });
    });
    suite.test("thumbnail queue defers recording, deduplicates and caps pending work", []
    {
        ThumbnailCache cache; cache.setRecording(true); std::atomic<int> jobs{0};
        require(cache.enqueue("first", [&](const auto&) { ++jobs; }), "Queue first");
        require(!cache.enqueue("first", [&](const auto&) { ++jobs; }), "Duplicate enqueued");
        for (unsigned i = 1; i < ThumbnailCache::maximumPending; ++i) require(cache.enqueue(juce::String(i), [&](const auto&) { ++jobs; }), "Queue capacity too small");
        require(!cache.enqueue("overflow", [](const auto&) {}), "Queue unbounded");
        require(!cache.mayRun() && jobs.load() == 0 && cache.pending() == ThumbnailCache::maximumPending, "Recording ran derived work");
        cache.setRecording(false); waitUntil([&] { return jobs.load() == int(ThumbnailCache::maximumPending); });
    });
    suite.test("WAV writer publishes immediate peaks from written PCM, independently of UI", []
    {
        const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-ui-wav-" + juce::Uuid().toString());
        WavTrackWriter::Config c; c.projectDirectory = folder; c.takeId = juce::Uuid(); c.sampleRate = 48000; c.framesPerBlock = 16; c.mics = 1;
        c.devices.push_back({"fixture", "Fixture input 1", 1, 0, 0});
        c.peakCache = std::make_shared<PeakCache>(48000, 1, 4); c.checkpointSink = [](const auto&) { return juce::Result::ok(); };
        WavTrackWriter writer(c); require(writer.start().wasOk(), "Writer prepare"); const std::int32_t data[] = {0, 8388607, -8388608, 1, 123};
        require(writer.tryPush(data, 5, 0), "Writer push"); waitUntil([&] { return c.peakCache->snapshot().samples == 5; });
        require(!c.peakCache->snapshot().complete, "Writer peak arrived only after stop");
        require(writer.stop(5, juce::Uuid()).wasOk(), "Writer stop"); require(c.peakCache->snapshot().complete, "Writer tail incomplete");
        MediaAsset asset; asset.kind = AssetKind::mic; asset.mediaGeneration = 2; asset.logicalLength = 8;
        asset.chunks.push_back({WavTrackWriter::chunkPath(c.takeId, 1, 1), {0, 5}});
        const auto source = RecorderSession::indexRecordedAudio(asset, folder, 48000, "mic");
        require(source->current() && source->generation == 2 && source->chunks[0].validBytes == 59, "Finalized media/header generation mismatch");
        TimelineAudioRenderer renderer(48000, 16); PlaybackAudioTrack track; track.trackId = "mic";
        RenderClip clip; clip.trackId = "mic"; clip.lengthSamples = 8; clip.mediaGeneration = 2; track.clips.push_back({clip, source});
        renderer.setPlan({track}, 8); float left[8]{}, right[8]{}; renderer.renderAudio(0, 8, left, right);
        require(left[1] > .99f && left[2] == -1 && right[2] == -1 && left[5] == 0 && left[7] == 0, "Written PCM or missing-tail silence failed");
        const auto file = folder.getChildFile("cache.json"); require(PeakCache::write(file, c.peakCache->snapshot()).wasOk(), "Persist peaks");
        require(PeakCache::read(file).samples == 5, "Reopen peaks");
    });
    suite.test("anonymous legacy WAV mappings retain first PCM samples and independent sources", []
    {
        recorder_audio_fixture::Fixture f(8000);
        std::vector<PlaybackAudioTrack> tracks;
        TimelineAudioRenderer renderer(8000, 16);
        for (unsigned mic = 0; mic < 2; ++mic)
        {
            const auto& asset = f.project.media->assets[2 + mic];
            PlaybackAudioTrack track; track.trackId = "anonymous-mic-" + juce::String(mic);
            RenderClip clip; clip.lengthSamples = 8; clip.mediaGeneration = asset.mediaGeneration;
            require(clip.clipId.isEmpty() && clip.assetId.isEmpty(), "Exercise legacy mapping without IDs");
            track.clips.push_back({clip, RecorderSession::indexRecordedAudio(asset, f.root, 8000, track.trackId)});
            renderer.setPlan({track}, 10); float left[10]{}, right[10]{}; renderer.renderAudio(0, 10, left, right);
            for (unsigned i = 0; i < 10; ++i)
            {
                const float expected = i < 8 ? f.sample(mic, i) : 0;
                require(left[i] == expected && right[i] == expected, "Anonymous PCM became silence/faded or tail padding changed");
            }
            tracks.push_back(std::move(track));
        }
        renderer.setPlan(std::move(tracks), 10); float left[10]{}, right[10]{}; renderer.renderAudio(0, 10, left, right);
        for (unsigned i = 0; i < 8; ++i)
        {
            const auto expected = f.sample(0, i) * .5f + f.sample(1, i) * .5f;
            require(left[i] == expected && right[i] == expected, "Anonymous WAV bindings aliased different sources");
        }
    });
    suite.test("single ASIO owner dispatches transport and monitor does not alter PCM", []
    {
        RecorderAudioEngine engine; OutputMapping out; out.mono = true; out.monoChannel = 0;
        require(engine.setOutputMap(out).wasOk() && engine.openSynthetic(48000, 16, 0, 1).wasOk(), "Synthetic device");
        struct Client : IAudioOutputClient { int calls = 0; void processOutput(const BlockStamp& s, float* l, float* r) noexcept override { ++calls; std::fill_n(l, s.numSamples, .25f); std::fill_n(r, s.numSamples, .75f); } } client;
        engine.setPlaybackClient(&client); BlockStamp stamp; stamp.numSamples = 16; stamp.sampleRate = 48000; stamp.callbackQpc = qpcNow(); stamp.flags = samplePositionValid | latenciesValid;
        float samples[16]{}; float* output[] = {samples}; engine.processBlock(stamp, nullptr, 0, nullptr, output, 1);
        require(client.calls == 1 && samples[0] == .5f, "Transport did not reach selected mono ASIO output");
        engine.setPlaybackClient(nullptr); stamp.samplePosition += 16; stamp.callbackQpc += 1000; ++stamp.sequence; engine.processBlock(stamp, nullptr, 0, nullptr, output, 1);
        require(client.calls == 1 && samples[0] == 0, "Detached transport still called");
    });
    suite.test("dubbing exclusively owns shared output while every input meter stays live", []
    {
        struct Client : IAudioOutputClient
        {
            int calls = 0; float value = .25f;
            void processOutput(const BlockStamp& s, float* l, float* r) noexcept override
            { ++calls; std::fill_n(l, s.numSamples, value); std::fill_n(r, s.numSamples, value); }
        } playback, dubbing;
        dubbing.value = -.5f;
        RecorderAudioEngine engine; OutputMapping out; out.mono = true; out.monoChannel = 0;
        const std::array<int, 8> inputs{-1, -1, -1, 2, -1, -1, -1, -1};
        require(engine.setInputMap(inputs).wasOk() && engine.setOutputMap(out).wasOk()
                && engine.openSynthetic(48000, 16, 3, 1).wasOk(), "Synthetic sparse input/output");
        BlockStamp stamp; stamp.sampleRate = 48000; stamp.callbackQpc = qpcNow(); stamp.flags = samplePositionValid | latenciesValid;
        float input[32]{}, samples[32]{}; const float* source[] = {input}; float* output[] = {samples};
        const auto feed = [&](unsigned frames, float peak)
        {
            std::fill_n(input, frames, -peak); stamp.numSamples = frames;
            engine.processBlock(stamp, nullptr, 1, source, output, 1);
            const auto meters = engine.inputPeaks();
            for (unsigned mic = 0; mic < meters.size(); ++mic)
                require(meters[mic] == (mic == 3 ? peak : 0.0f), "Output selection changed unarmed input meters");
            stamp.samplePosition += frames; stamp.callbackQpc += 1000; ++stamp.sequence;
        };
        engine.setPlaybackClient(&playback); feed(16, .25f);
        require(playback.calls == 1 && samples[0] == .25f, "Normal playback output");
        engine.setDubbingOutputClient(&dubbing); feed(16, .75f);
        require(playback.calls == 1 && dubbing.calls == 1 && samples[0] == -.5f, "Both clients rendered the dubbing block");
        engine.setPlaybackClient(nullptr); feed(16, .5f);
        require(playback.calls == 1 && dubbing.calls == 2 && samples[0] == -.5f, "Normal detach interrupted dubbing");
        engine.setPlaybackClient(&playback); feed(32, .5f);
        require(playback.calls == 1 && dubbing.calls == 2, "Oversized output reached a prepared client");
        for (const auto sample : samples) require(sample == 0, "Oversized output was not silent");
        engine.setDubbingOutputClient(nullptr); feed(16, .125f);
        require(playback.calls == 2 && dubbing.calls == 2 && samples[0] == .25f, "Normal playback did not resume after dubbing detach");
        engine.setPlaybackClient(nullptr); feed(16, 0);
        require(playback.calls == 2 && dubbing.calls == 2 && samples[0] == 0, "Detached output still called a client");
    });
    return suite.result("ui-wiring");
}
