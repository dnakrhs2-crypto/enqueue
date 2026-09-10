#include "TestSupport.h"
#include "AudioRenderFixtures.h"
#include "ui/UiState.h"
#include "ui/AudioSettingsPanel.h"
#include "media/PeakCache.h"
#include "media/ThumbnailCache.h"
#include "playback/TimelineTransport.h"
#include "app/RecorderSession.h"
#include "storage/RecoveryScanner.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <optional>

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
struct ReviewFolder
{
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-stereo-review-" + newId());
    ~ReviewFolder()
    {
        if (root.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory)
            && root.getFileName().startsWith("recorder-stereo-review-")) root.deleteRecursively();
    }
};
std::int32_t reviewPcm(Sample frame, unsigned channel)
{ return std::int32_t((frame % 29 + 1) * (channel + 1) * 65536) * (channel % 2 ? -1 : 1); }
WavTrackWriter::Config writeReviewTake(const juce::File& folder, const std::vector<unsigned>& slots, Sample frames)
{
    WavTrackWriter::Config c; c.projectDirectory = folder; c.sampleRate = 8000; c.framesPerBlock = 1024;
    c.mics = unsigned(slots.size()); c.slotChannels = slots;
    unsigned channels = 0;
    for (unsigned i = 0; i < slots.size(); ++i)
    {
        JournalDeviceMapping m{"fixture", "Synthetic input", int(i + 1), int(channels), int(channels)};
        if (slots[i] == 2) { m.rightPhysicalIndex = int(channels + 1); m.rightActiveIndex = int(channels + 1); }
        c.devices.push_back(m); channels += slots[i];
    }
    WavTrackWriter writer(c); require(writer.start().wasOk(), "Prepare review WAV writer");
    std::vector<std::int32_t> block(c.framesPerBlock * channels);
    for (Sample at = 0; at < frames; at += c.framesPerBlock)
    {
        const auto count = unsigned((std::min)(Sample(c.framesPerBlock), frames - at));
        for (unsigned n = 0; n < count; ++n) for (unsigned ch = 0; ch < channels; ++ch) block[n * channels + ch] = reviewPcm(at + n, ch);
        waitUntil([&] { return writer.queueFrames() < c.sampleRate; });
        require(writer.tryPush(block.data(), count, std::uint64_t(at)), "Push synthetic interleaved PCM");
    }
    require(writer.stop(frames, juce::Uuid()).wasOk(), "Finalize review WAV writer"); return c;
}
void checkAppPlayback(const std::vector<unsigned>& slots)
{
    ReviewFolder f; constexpr Sample boundary = 8000 * 30, frames = boundary + 7;
    const auto c = writeReviewTake(f.root, slots, frames);
    std::vector<PlaybackAudioTrack> tracks; AudioRenderPlan exportPlan;
    RecorderProject exportProject; exportProject.Fs = c.sampleRate; exportPlan.timeline = RenderPlanCompiler::compile(exportProject);
    unsigned channel = 0;
    for (unsigned i = 0; i < slots.size(); ++i)
    {
        MediaAsset asset; asset.kind = AssetKind::mic; asset.mediaGeneration = 2; asset.logicalLength = frames; asset.originalFormat.channels = int(slots[i]);
        asset.chunks = {{WavTrackWriter::chunkPath(c.takeId, i + 1, 1), {0, boundary}},
                        {WavTrackWriter::chunkPath(c.takeId, i + 1, 2), {boundary, 7}}};
        const auto source = RecorderSession::indexRecordedAudio(asset, f.root, c.sampleRate, asset.assetId);
        require(source->channels == slots[i] && source->generation == 2 && source->current() && source->length == frames
            && source->chunks.size() == 2 && source->chunks[0].validBytes == 44 + std::uint64_t(boundary) * slots[i] * 3
            && source->chunks[1].validBytes == 44 + 7 * slots[i] * 3, "App index lost stereo format or chunk watermark");
        PlaybackAudioTrack track; track.trackId = asset.assetId;
        RenderClip clip; clip.trackId = track.trackId; clip.lengthSamples = frames; clip.mediaGeneration = 2; track.clips.push_back({clip, source});
        TimelineAudioRenderer renderer(c.sampleRate, 16); renderer.setPlan({track}, frames);
        for (const Sample start : {Sample{0}, boundary - 5})
        {
            float left[12]{}, right[12]{}; renderer.renderAudio(start, 12, left, right);
            for (unsigned n = 0; n < 12; ++n)
                require(left[n] == float(reviewPcm(start + n, channel)) / 8388608.0f
                    && right[n] == float(reviewPcm(start + n, channel + (slots[i] == 2 ? 1 : 0))) / 8388608.0f,
                    "App playback lost first PCM, L/R identity or samples across 30-second boundary");
        }
        tracks.push_back(track); exportPlan.sources.push_back(asset); channel += slots[i];
    }
    const auto exported = openAudioSources(exportPlan, f.root);
    TimelineAudioRenderer mixed(c.sampleRate, 16); mixed.setPlan(tracks, frames);
    float left[12]{}, right[12]{}; mixed.renderAudio(boundary - 5, 12, left, right);
    for (unsigned n = 0; n < 12; ++n)
    {
        float expectedL = 0, expectedR = 0; unsigned ch = 0;
        for (unsigned i = 0; i < slots.size(); ++i)
        {
            float l[12]{}, r[12]{}; exported[i].source->read(boundary - 5, 12, l, r);
            require(l[n] == float(reviewPcm(boundary - 5 + n, ch)) / 8388608.0f
                && r[n] == float(reviewPcm(boundary - 5 + n, ch + (slots[i] == 2 ? 1 : 0))) / 8388608.0f,
                "Export and app source construction disagree");
            expectedL += l[n] / float(slots.size()); expectedR += r[n] / float(slots.size()); ch += slots[i];
        }
        require(std::abs(left[n] - expectedL) < 1e-6f && std::abs(right[n] - expectedR) < 1e-6f, "Mixed take playback does not average logical slots");
    }
}
void applySyntheticSettings(RecorderAudioEngine& engine, const UserSettings& s)
{
    std::array<int, 8> map; map.fill(-1); std::copy(s.physicalInputs.begin(), s.physicalInputs.end(), map.begin());
    require(engine.setInputMap(map, s.stereoSlots).wasOk() && engine.setOutputMap(s.output).wasOk(), "Apply synthetic mapping");
    for (unsigned i = 0; i < map.size(); ++i) require(engine.arm(i, map[i] >= 0 && s.microphoneArmed[i]).wasOk(), "Apply synthetic arms");
}
UserSettings reviewSettings()
{
    UserSettings s; s.asioDeviceId = "synthetic-native-PCM"; s.physicalInputs = {0, 1, -1, -1, -1, -1, -1, -1};
    s.output.left = 0; s.output.right = 1; s.audioDefaultsApplied = true;
    s.cameraDeviceIds[0] = "fixture-camera"; s.cameraModes[0] = "NV12 1920x1080 30/1"; return s;
}
CalibrationProfile reviewCalibration(const UserSettings& s, unsigned camera = 0)
{
    CalibrationProfile p; p.key = calibrationKey(s, camera); p.quality = CalibrationQuality::physicalMeasured;
    p.measuredUtc = "2026-09-10T00:00:00Z"; p.method = "synthetic full-key regression"; p.measurementCount = 1; return p;
}
void checkRejectedSelection(bool queued)
{
    juce::ScopedJuceInitialiser_GUI runtime; ReviewFolder f; RecorderSettings saved(f.root);
    auto applied = reviewSettings(); require(saved.set(applied).wasOk() && saved.save().get().wasOk(), "Save applied settings");
    RecorderDocument document; RecorderSession session(document); applySyntheticSettings(session.audioEngine(), applied);
    require(session.audioEngine().openSynthetic(48000, 256, 8, 2).wasOk(), "Synthetic settings device");
    AudioSettingsPanel panel(applied, document.getProject(), session.audioEngine().deviceInfo());
    auto* combo = dynamic_cast<juce::ComboBox*>(panel.findChildWithID("microphoneInput1")); require(combo != nullptr, "Product microphone combo");
    unsigned edits = 0, completions = 0; std::optional<UserSettings> pending; auto result = juce::Result::ok();
    session.onConfigured = [&](const auto&, const auto&) { ++completions; };
    panel.onChanged = [&](UserSettings next)
    {
        ++edits;
        if (queued) pending = next;
        else result = panel.configure(session, std::move(next), saved.get());
    };
    combo->setSelectedId(1000, juce::sendNotificationSync); // input 1+2 conflicts with slot 2's mono input 2
    if (queued)
    {
        require(pending.has_value() && combo->getSelectedId() == 1000, "Queued choice must remain until retry");
        applied.physicalInputs[0] = 2; applied.physicalInputs[1] = 3; // preceding configuration has now completed
        applySyntheticSettings(session.audioEngine(), applied);
        require(saved.set(applied).wasOk() && saved.save().get().wasOk(), "Persist preceding applied configuration");
        const auto next = *pending; pending.reset(); result = panel.configure(session, next, saved.get());
    }
    require(result.failed() && edits == 1 && completions == 0 && !session.configuring(), "Synchronous rejection must not launch device work or recursive edits");
    const auto visible = panel.read(saved.get()); RecorderSettings disk(f.root); require(disk.load().wasOk(), "Reload accepted settings");
    require(combo->getSelectedId() == applied.physicalInputs[0] + 2 && !visible.stereoSlots[0]
        && visible.physicalInputs == applied.physicalInputs && disk.get().physicalInputs == applied.physicalInputs
        && disk.get().stereoSlots == applied.stereoSlots && saved.get().physicalInputs == applied.physicalInputs,
        "Rejected stereo selection survived in visible or saved settings");
    require(session.audioEngine().deviceInfo().synthetic && session.audioEngine().calibrationInputMapping() == calibrationKey(visible, 0).inputMapping,
        "Visible settings and applied engine mapping disagree");
    bool actualRate = false;
    for (auto* child : panel.getChildren()) if (auto* label = dynamic_cast<juce::Label*>(child))
        if (label->getText().startsWith(juce::String::fromUTF8("실제 48000 Hz"))) actualRate = true;
    require(actualRate, "Rejected edit left the connecting status on screen");
}
}
int runUiWiringTests()
{
    Suite suite;
    suite.test("App playback indexes stereo writer PCM across the 30-second chunk boundary", [] { checkAppPlayback({2}); });
    suite.test("App playback indexes mixed 1/2/1 slots and agrees with export sources", [] { checkAppPlayback({1, 2, 1}); });
    suite.test("App playback indexes recovered stereo prefix and silences torn right-sample tail", []
    {
        ReviewFolder f; RecorderProject initial; initial.Fs = 8000;
        RecoveryScanner::writeCheckpoint(f.root.getChildFile("project.recorder"), initial);
        const auto c = writeReviewTake(f.root, {2}, 13); const auto original = f.root.getChildFile(WavTrackWriter::chunkPath(c.takeId, 1, 1));
        juce::MemoryBlock torn; require(original.loadFileAsData(torn), "Read stereo fixture"); torn.setSize(torn.getSize() - 1);
        require(original.replaceWithData(torn.getData(), torn.getSize()), "Tear final right sample");
        RecoveryReport recovered; const auto result = RecoveryScanner().run(f.root, recovered); require(result.wasOk(), result.getErrorMessage().toRawUTF8());
        require(recovered.project.media->takes.size() == 1, "Recover stereo take");
        const auto* asset = recovered.project.media->findAsset(recovered.project.media->takes[0].microphoneAssetIds[0]);
        require(asset && asset->originalFormat.channels == 2 && asset->logicalLength == 13 && asset->gaps.size() == 1
            && asset->gaps[0].start == 12 && asset->gaps[0].length == 1, "Recover complete stereo frames and explicit tail gap");
        const auto source = RecorderSession::indexRecordedAudio(*asset, f.root, 8000, "recovered-mic");
        require(source->channels == 2 && source->length == 13 && source->chunks[0].validBytes == 44 + 12 * 6, "Recovered app durable watermark");
        TimelineAudioRenderer renderer(8000, 16); PlaybackAudioTrack track; track.trackId = "recovered-mic";
        RenderClip clip; clip.trackId = track.trackId; clip.lengthSamples = 13; clip.mediaGeneration = asset->mediaGeneration;
        clip.gaps = asset->gaps; track.clips.push_back({clip, source}); renderer.setPlan({track}, 13);
        float left[13]{}, right[13]{}; renderer.renderAudio(0, 13, left, right);
        for (unsigned i = 0; i < 13; ++i) require(left[i] == (i < 12 ? float(reviewPcm(i, 0)) / 8388608.0f : 0)
            && right[i] == (i < 12 ? float(reviewPcm(i, 1)) / 8388608.0f : 0), "Recovered stereo prefix/tail playback");
        juce::MemoryBlock after; require(original.loadFileAsData(after) && after == torn, "Playback/recovery altered original WAV");
    });
    suite.test("App WAV index rejects invalid channel metadata, duration and chunk ranges", []
    {
        ReviewFolder f; const auto c = writeReviewTake(f.root, {2}, 13);
        MediaAsset asset; asset.kind = AssetKind::mic; asset.mediaGeneration = 1; asset.logicalLength = 13;
        asset.chunks = {{WavTrackWriter::chunkPath(c.takeId, 1, 1), {0, 13}}};
        for (int channels : {-1, 0, 1, 3}) { asset.originalFormat.channels = channels; rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic"); }); }
        asset.originalFormat.channels = 2; asset.logicalLength = 12; rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic"); });
        asset.logicalLength = 13; asset.chunks[0].sourceRange.length = -1; rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic"); });
    });
    suite.test("Rejected stereo combo restores visible, saved and engine settings synchronously", [] { checkRejectedSelection(false); });
    suite.test("Rejected queued stereo edit restores the latest completed settings", [] { checkRejectedSelection(true); });
    suite.test("Calibration display and engine agree on armed sparse slot/L/R keys", []
    {
        auto s = reviewSettings(); s.physicalInputs = {0, -1, 2, 4}; s.stereoSlots[2] = true; s.microphoneArmed[3] = false;
        RecorderDocument d; RecorderSession session(d); applySyntheticSettings(session.audioEngine(), s);
        require(session.audioEngine().openSynthetic(48000, 256, 8, 2).wasOk(), "Synthetic calibration mapping");
        const auto p = reviewCalibration(s); require(p.key.inputMapping == std::vector<int>({1, 0, -1, 3, 2, 3})
            && p.key.inputMapping == session.audioEngine().calibrationInputMapping(), "Shared key differs from engine armed mappings");
        require(session.setCalibrationProfiles({p}).wasOk() && session.calibrationMatches(s) == CalibrationMatch::matched, "Full session profile should display measured");
        for (unsigned change = 0; change < 4; ++change)
        {
            auto next = s;
            if (change == 0) next.stereoSlots[2] = false;
            if (change == 1) next.physicalInputs[2] = 5;
            if (change == 2) next.microphoneArmed[2] = false;
            if (change == 3) { next.physicalInputs[1] = 2; next.physicalInputs[2] = -1; next.stereoSlots[1] = true; next.stereoSlots[2] = false; }
            require(session.calibrationMatches(next) == CalibrationMatch::settingsChanged, "Changed input pair/arm/logical slot still displays measured");
        }
        auto unarmed = s; unarmed.physicalInputs[3] = 7;
        require(session.calibrationMatches(unarmed) == CalibrationMatch::matched, "Unarmed input should not invalidate an unchanged capture key");
        require(session.audioEngine().arm(2, false).wasOk() && session.calibrationMatches(s) == CalibrationMatch::settingsChanged, "Actual engine arm mismatch ignored");
    });
    suite.test("Calibration display checks complete camera, rate, buffer and output keys", []
    {
        const auto s = reviewSettings(); const auto p = reviewCalibration(s);
        require(calibrationMatches(s, {p}) == CalibrationMatch::matched, "Matching measured profile");
        for (unsigned change = 0; change < 9; ++change)
        {
            auto next = s;
            if (change == 0) next.preferredSampleRate = 44100;
            if (change == 1) next.bufferSize = 512;
            if (change == 2) std::swap(next.output.left, next.output.right);
            if (change == 3) next.asioDeviceId = "different-driver";
            if (change == 4) next.cameraDeviceIds[0] = "different-camera";
            if (change == 5) next.cameraModes[0] = "NV12 1920x1080 60/1";
            if (change == 6) next.cameraModes[0] = "invalid mode";
            if (change == 7) { next.cameraEnabled[1] = true; next.cameraDeviceIds[1] = "second-camera"; next.cameraModes[1] = s.cameraModes[0]; }
            if (change == 8) { next.output.mono = true; next.output.monoChannel = 0; next.output.left = next.output.right = -1; }
            require(calibrationMatches(next, {p}) == CalibrationMatch::settingsChanged, "Incomplete calibration key comparison");
        }
        auto both = s; both.cameraEnabled[1] = true; both.cameraDeviceIds[1] = "second-camera"; both.cameraModes[1] = s.cameraModes[0];
        require(calibrationMatches(both, {p, reviewCalibration(both, 1)}) == CalibrationMatch::matched, "Both enabled cameras require matching profiles");
    });
    suite.test("Legacy calibration without inputMapping requires explicit remeasurement", []
    {
        auto s = reviewSettings(); auto legacy = reviewCalibration(s); legacy.key.inputMapping.clear();
        legacy = CalibrationProfile::deserialize(legacy.serialize());
        RecorderDocument d; RecorderSession session(d); require(session.setCalibrationProfiles({legacy}).wasOk(), "Load legacy calibration profile");
        require(session.calibrationMatches(s) == CalibrationMatch::inputMappingUnverified
            && calibrationStatusText(session.calibrationMatches(s)) == juce::String::fromUTF8("동기 보정 · 입력 매핑 확인 전 · 재측정 필요"), "Legacy input mapping incorrectly displays measured");
        s.calibration.calibrationDate = "2026-09-09"; s.calibration.asioDeviceId = s.asioDeviceId;
        s.calibration.cameraDeviceIds = s.cameraDeviceIds; s.calibration.cameraModes = s.cameraModes;
        require(calibrationMatches(s, {}) == CalibrationMatch::inputMappingUnverified, "Legacy display snapshot is not a complete calibration key");
        s.physicalInputs.clear(); require(session.calibrationMatches(s) == CalibrationMatch::inputMappingUnverified, "Empty armed set must not silently verify a legacy mapping");
        s.calibration.calibrationDate.clear(); require(calibrationMatches(s, {}) == CalibrationMatch::unmeasured, "Absent calibration result");
    });
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
        // 0.1.3: a fixed-project rate mismatch is not an input error (the device is reopened at the project rate on 적용);
        // recording is blocked by RecorderSession::readyToRecord and the banner names both rates (rateMismatchText).
        require(validateAudioSettings(s, d, p).wasOk(), "Fixed project Fs mismatch must not block applying settings");
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
        asset.originalFormat.channels = 1;
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
