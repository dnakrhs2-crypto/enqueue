#include <juce_gui_extra/juce_gui_extra.h>
#include "TestSupport.h"
#include "AudioRenderFixtures.h"
#include "RecordedGapFixtures.h"
#include "ui/UiState.h"
#include "ui/AudioSettingsPanel.h"
#include "media/PeakCache.h"
#include "media/ThumbnailCache.h"
#include "playback/TimelineTransport.h"
#include "app/RecorderSession.h"
#include "storage/RecoveryScanner.h"
#include "ui/MainComponent.h"
#include "StabilityTestAccess.h"
#include "export/MaterialExporter.h"
// The headless app harness in ShortcutExceptionTests.cpp compiles MainComponent;
// compile its shared import widget here without starting the product application.
#include "../src/ui/AudioImportPanel.cpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <optional>

using namespace gocue::recorder;
using namespace recorder_test;
namespace recorder_import_test { juce::File writeWav(const juce::File&, std::uint32_t, int, Sample); }
namespace gocue::recorder
{
struct ImportUiTestAccess
{
    static void stopTimer(MainComponent& main) { main.stopTimer(); }
    static void tick(MainComponent& main) { main.audioImporter.timerCallback(); main.timerCallback(); }
    static bool busy(const MainComponent& main) { return main.importBusy(); }
    static void click(MainComponent& main, const juce::File& file)
    { main.audioImporter.chooseFileForTesting = [file] { return file; }; main.recordView.importButton.onClick(); }
    static void cancel(MainComponent& main) { main.audioImporter.cancelImport(); }
    static void ready(MainComponent& main) { main.refresh(); }
    static bool enabled(MainComponent& main) { return main.recordView.importButton.isEnabled(); }
    static juce::String message(MainComponent& main) { return main.banner; }
    static void startRecording(MainComponent& main) { main.recordView.startButton.onClick(); }
    static void seek(MainComponent& main, Sample at) { main.session.scrub(at, true); }
    static void play(MainComponent& main) { main.timelineView.transport.play.onClick(); }
    static void pause(MainComponent& main) { main.timelineView.transport.stop.onClick(); } // the transport has no separate pause button any more; stop keeps the position
    static bool buttonPlaced(MainComponent& main)
    {
        const auto& r = main.recordView;
        return r.importButton.isVisible() && r.importButton.getWidth() >= 140
            && r.importButton.getY() == r.markerButton.getY()
            && r.markerButton.getRight() < main.audioImporter.getX()
            && main.audioImporter.getWidth() >= 400 && main.audioImporter.getRight() < r.importButton.getX();
    }
};
}
namespace
{
using ImportAccess = ImportUiTestAccess;
void waitUntil(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(predicate(), "Worker timeout");
}
template<class Predicate> void waitImportUi(MainComponent& main, Predicate done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done() && std::chrono::steady_clock::now() < deadline)
    { ImportAccess::tick(main); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    require(done(), "Import UI worker timeout");
}
void paintImportedWave(MainComponent& main)
{
    auto& view = StabilityTestAccess::timeline(main); view.zoomToFit();
    juce::Image picture(juce::Image::ARGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType{});
    juce::Graphics graphics(picture); view.paintEntireComponent(graphics, true);
    require(view.lastPaintWaveColumns > 0, "Actual timeline paint did not draw imported waveform");
}
// Same processBlock/output mapping used by ASIO, driven by a synthetic clock.
// Capture actual output while the app's transport and renderer workers run.
void hearImportedAudio(MainComponent& main, int channels)
{
    auto& session = StabilityTestAccess::session(main); StabilityTestAccess::openAudio(session);
    ImportAccess::seek(main, 4800); ImportAccess::play(main);
    std::array<float, 480> left{}, right{}; float* output[]{left.data(), right.data()};
    double energy = 0, difference = 0; unsigned sequence = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (energy < 1 && std::chrono::steady_clock::now() < deadline)
    {
        ImportAccess::tick(main);
        BlockStamp stamp{}; stamp.flags = samplePositionValid | latenciesValid;
        stamp.sampleRate = 48000; stamp.numSamples = 480; stamp.sequence = sequence;
        stamp.samplePosition = Sample(sequence++) * 480; stamp.callbackQpc = qpcNow();
        session.audioEngine().processBlock(stamp, nullptr, 0, nullptr, output, 2);
        for (size_t i = 0; i < left.size(); ++i) { energy += left[i] * left[i] + right[i] * right[i]; difference += std::abs(left[i] - right[i]); }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(session.error.isEmpty(), session.error.toRawUTF8());
    require(energy > 1, "Imported PCM never reached the app's selected ASIO output path");
    require(channels == 1 ? difference < 1e-5 : difference > .1, "Mono duplication/stereo channel identity lost");
    ImportAccess::pause(main);
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
        asset.availableRanges = {{0, frames}};
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
template<class Control> Control& audioControl(AudioSettingsPanel& panel, const juce::String& id)
{
    auto* control = dynamic_cast<Control*>(panel.findChildWithID(id));
    require(control != nullptr, "Product audio settings control missing"); return *control;
}
RecorderAudioEngine::DeviceInfo audioSettingsDevice(int channels = 8)
{
    RecorderAudioEngine::DeviceInfo d; d.name = "synthetic-native-PCM"; d.synthetic = true;
    d.sampleRate = 48000; d.bufferFrames = 256; d.availableBuffers = {128, 256, 512};
    d.physicalInputs = channels; d.physicalOutputs = 2; d.outputNames = {"L", "R"};
    for (int i = 0; i < channels; ++i) d.inputNames.add("Analog " + juce::String(i + 1));
    return d; // metadata injection only: never open an ASIO driver or a camera
}
CalibrationProfile reviewCalibration(const UserSettings& s, unsigned camera = 0)
{
    CalibrationProfile p; p.key = calibrationKey(s, camera); p.quality = CalibrationQuality::physicalMeasured;
    p.measuredUtc = "2026-09-10T00:00:00Z"; p.method = "synthetic full-key regression"; p.measurementCount = 1; return p;
}
void checkRejectedSelection(bool queued)
{
    juce::ScopedJuceInitialiser_GUI runtime; ReviewFolder f; RecorderSettings saved(f.root);
    auto applied = reviewSettings(); applied.physicalInputs[1] = 2; applied.stereoSlots[0] = true;
    require(saved.set(applied).wasOk() && saved.save().get().wasOk(), "Save applied settings");
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
    // Force a stale/disabled menu choice: moving the stereo pair to 2+3 conflicts with slot 2's input 3.
    combo->setSelectedId(3, juce::sendNotificationSync);
    if (queued)
    {
        require(pending.has_value() && combo->getSelectedId() == 3, "Queued choice must remain until retry");
        require(audioControl<juce::Label>(panel, "microphoneHint1").getText().contains(ko("마이크 2")), "Queued overlap must be explained immediately");
        applied.physicalInputs[0] = 3; applied.physicalInputs[1] = 5; // preceding configuration has now completed
        applySyntheticSettings(session.audioEngine(), applied);
        require(saved.set(applied).wasOk() && saved.save().get().wasOk(), "Persist preceding applied configuration");
        const auto next = *pending; pending.reset(); result = panel.configure(session, next, saved.get());
    }
    require(result.failed() && edits == 1 && completions == 0 && !session.configuring(), "Synchronous rejection must not launch device work or recursive edits");
    const auto visible = panel.read(saved.get()); RecorderSettings disk(f.root); require(disk.load().wasOk(), "Reload accepted settings");
    require(combo->getSelectedId() == applied.physicalInputs[0] + 2 && visible.stereoSlots == applied.stereoSlots
        && audioControl<juce::TextButton>(panel, "microphoneStereo1").getToggleState()
        && !audioControl<juce::TextButton>(panel, "microphoneMono1").getToggleState()
        && visible.physicalInputs == applied.physicalInputs && disk.get().physicalInputs == applied.physicalInputs
        && disk.get().stereoSlots == applied.stereoSlots && saved.get().physicalInputs == applied.physicalInputs,
        "Rejected stereo selection survived in visible or saved settings");
    const auto hint = audioControl<juce::Label>(panel, "microphoneHint1").getText();
    require(hint.contains(ko("변경 취소")) && hint.contains(ko("마이크 2")), "Restoring controls erased the slot-specific rejection reason");
    require(audioControl<juce::Label>(panel, "microphoneSummary1").getText().contains(
        ko("입력 ") + juce::String(applied.physicalInputs[0] + 1) + "+" + juce::String(applied.physicalInputs[0] + 2)), "Rejected selection survived in the summary");
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
    for (const auto rate : {44100u, 48000u}) for (const auto channels : {1, 2})
    {
        const auto name = "Main import button -> waveform/ASIO PCM/save/reopen/materials/final source: " + std::to_string(rate) + "/" + std::to_string(channels);
        suite.test(name.c_str(), [=]
        {
            juce::ScopedJuceInitialiser_GUI runtime; ReviewFolder folder; RecorderDocument document;
            const auto file = folder.root.getChildFile("project/project.recorder");
            require(document.saveCheckpoint(file).wasOk(), "Save import project");
            const auto wav = recorder_import_test::writeWav(folder.root.getChildFile("fixtures"), rate, channels, Sample(rate) * 2 + 137);
            AudioImportControl hashControl; const auto sourceHash = AudioImport::hashFile(wav, hashControl);
            Id assetId;
            {
                RecorderSettings settings(folder.root.getChildFile("settings")); MainComponent main(document, settings); ImportAccess::stopTimer(main);
                require(ImportAccess::buttonPlaced(main) && ImportAccess::enabled(main), "Main import button location/accessibility");
                ImportAccess::click(main, wav);
                require(ImportAccess::busy(main) && (main.lifecycleState()->snapshot() & RecorderLifecycle::fileWork), "Import must acquire lifecycle file gate");
                require(!ImportAccess::enabled(main) && !main.lifecycleState()->canShutdown(), "Busy import/update gate");
                const auto project = document.getProject().projectId;
                main.createProject("blocked", folder.root.getChildFile("blocked"), 30); main.openProject(wav);
                ImportAccess::startRecording(main);
                require(document.getProject().projectId == project && !StabilityTestAccess::session(main).recording(), "Import allowed project replacement/recording");
                waitImportUi(main, [&] { return !ImportAccess::busy(main); });
                require(document.getProject().tracks.size() == 1 && document.getProject().media->assets.size() == 1, "Main handler did not publish import");
                const auto& track = document.getProject().tracks.front(); const auto clip = track.clips.items().front(); assetId = clip.assetId;
                require(track.kind == TrackKind::importAudio && clip.timelineStartSample == 0
                    && clip.lengthSamples == rescaleRound(Sample(rate) * 2 + 137, 48000, rate), "Import track, placement or sample mapping");
                require(ImportAccess::message(main).contains(rate == 48000 ? juce::String::fromUTF8("변환 없음") : juce::String::fromUTF8("44100 → 48000")), "Korean sample-rate result missing");
                require(ImportAccess::message(main).contains(juce::String::fromUTF8("타임라인 끝")), "Placement rule not shown");
                paintImportedWave(main); hearImportedAudio(main, channels);
                require(document.saveCheckpoint(file).wasOk(), "Save imported project");
            }
            // Remove a copied cache file to require a real rebuild on reopen.
            const auto manifests = file.getParentDirectory().getChildFile("cache/imported-audio").findChildFiles(juce::File::findFiles, false, "*.json");
            require(!manifests.isEmpty(), "Persistent cache manifest missing");
            const auto generation = juce::JSON::parse(manifests[0])["generation"].toString();
            require(manifests[0].getSiblingFile(generation).getChildFile("audio.wav").deleteFile(), "Remove derived cache fixture");
            RecorderDocument reopened; require(reopened.openCheckpoint(file).wasOk(), "Reopen imported project");
            require(reopened.getProject().media->findAsset(assetId) && reopened.getProject().tracks.front().kind == TrackKind::importAudio, "Reopen lost imported asset/track");
            {
                RecorderSettings settings(folder.root.getChildFile("reopen-settings")); MainComponent main(reopened, settings); ImportAccess::stopTimer(main);
                auto& session = StabilityTestAccess::session(main); session.projectChanged(); StabilityTestAccess::tab(main, true);
                waitImportUi(main, [&]
                {
                    auto& view = StabilityTestAccess::timeline(main); view.zoomToFit();
                    juce::Image picture(juce::Image::ARGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType{});
                    juce::Graphics graphics(picture); view.paintEntireComponent(graphics, true); return view.lastPaintWaveColumns > 0;
                });
                hearImportedAudio(main, channels);
            }
            ExportActivity activity; ExportControl control(activity); ExportJob job(reopened.getProject(), file.getParentDirectory());
            MaterialExportOptions materials; materials.includeImports = true;
            const auto outputs = MaterialExporter::outputs(job, materials);
            require(outputs.size() == size_t(channels), "Imported materials not offered for every channel");
            rejects([&] { MaterialExporter::outputs(job, {}); }); // import-only project has no materials unless included
            const auto manifest = MaterialExporter::run(job, materials, control);
            require(manifest["files"].size() == channels, "Imported materials not published");
            for (const auto& output : outputs)
            {
                auto reader = AudioImport::openReader(job.outputDirectory.getChildFile(output.name)); juce::AudioBuffer<float> pcm(1, 1024);
                require(reader->read(&pcm, 0, 1024, 4800, true, false) && pcm.getMagnitude(0, 1024) > .1f, "Exported imported material is silent");
            }
            auto videoProject = reopened.getProject(); Track camera; camera.kind = TrackKind::cam1;
            videoProject.tracks.push_back(camera); // an empty camera lane renders black, with no capture hardware
            ExportJob finalJob(videoProject, file.getParentDirectory());
            const auto selection = FinalVideoExporter::audioSource(finalJob, "import:" + assetId);
            FinalVideoExporter::validateSelection(finalJob, {TrackKind::cam1, selection});
            require(TimelineExporter::assetIds(finalJob, selection).contains(juce::var(assetId)), "Final export omitted imported source");
            ExportAudioRenderer finalAudio(finalJob, TimelineExporter::openSources(finalJob, selection, control), selection);
            std::array<float, 512> left{}, right{}; finalAudio.render(4800, 512, left.data(), right.data());
            require(*std::max_element(left.begin(), left.end()) > .1f, "Final export imported source is silent");
            require(AudioImport::hashFile(wav, hashControl) == sourceHash, "External WAV changed");
        });
    }
    suite.test("Main drop placement is captured before asynchronous import; imported clips split, move, delete and persist", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; ReviewFolder folder; RecorderDocument document;
        const auto file = folder.root.getChildFile("project/project.recorder"); require(document.saveCheckpoint(file).wasOk(), "Project fixture");
        const auto wav = recorder_import_test::writeWav(folder.root.getChildFile("fixtures"), 48000, 1, 96000);
        RecorderSettings settings(folder.root.getChildFile("settings")); MainComponent main(document, settings); ImportAccess::stopTimer(main);
        ImportAccess::click(main, wav); waitImportUi(main, [&] { return !ImportAccess::busy(main); });
        StabilityTestAccess::tab(main, false); main.filesDropped({wav.getFullPathName()}, 10, 10);
        waitImportUi(main, [&] { return !ImportAccess::busy(main); });
        require(document.getProject().tracks[1].clips.items()[0].timelineStartSample == 96000, "Recording view must append at captured end");
        ImportAccess::seek(main, 12347); main.filesDropped({wav.getFullPathName()}, 10, 500);
        ImportAccess::seek(main, 54321); waitImportUi(main, [&] { return !ImportAccess::busy(main); });
        const auto clip = document.getProject().tracks[2].clips.items()[0];
        require(clip.timelineStartSample == 12347 && ImportAccess::message(main).contains(juce::String::fromUTF8("재생헤드")), "Drop followed a later cursor/mouse location");
        auto& view = StabilityTestAccess::timeline(main); view.edits.clickClip(clip.clipId);
        ImportAccess::seek(main, 12347 + 48000); ImportAccess::ready(main);
        require(view.invoke(TimelineAction::split, 12347 + 48000, true).wasOk(), "Imported split");
        require(document.getProject().tracks[2].clips.items().size() == 2, "Split did not produce two clips");
        const auto second = document.getProject().tracks[2].clips.items()[1]; view.edits.clickClip(second.clipId);
        require(view.invoke(TimelineAction::move, second.timelineStartSample + 4800, true).wasOk(), "Imported move");
        require(document.getProject().findClip(second.clipId)->timelineStartSample == second.timelineStartSample + 4800, "Imported move offset");
        require(view.invoke(TimelineAction::remove).wasOk(), "Imported delete");
        require(document.getProject().tracks[2].clips.items().size() == 1, "Delete did not remove selected import piece");
        require(document.undo().wasOk() && document.redo().wasOk(), "Imported edit undo/redo");
        require(document.saveCheckpoint(file).wasOk(), "Save edits"); RecorderDocument reopened;
        require(reopened.openCheckpoint(file).wasOk() && reopened.getProject().tracks[2].clips.items().size() == 1
            && reopened.getProject().tracks[2].clips.items()[0].lengthSamples == 48000, "Imported cut edits not retained");
        ExportActivity activity; ExportControl control(activity); ExportJob job(reopened.getProject(), file.getParentDirectory());
        const auto mask = FinalVideoExporter::audioSource(job, "import:" + clip.assetId);
        TimelineAudioRenderer renderer(48000, 512); renderer.setPlan(job.audioPlan->timeline, TimelineExporter::openSources(job, mask, control), mask);
        std::array<float, 512> left{}, right{};
        renderer.renderAudio(clip.timelineStartSample + 4096, 512, left.data(), right.data());
        require(*std::max_element(left.begin(), left.end()) > .1f, "Kept imported piece became silent after edits/reopen");
        renderer.renderAudio(clip.timelineStartSample + 48000 + 4096, 512, left.data(), right.data());
        require(std::all_of(left.begin(), left.end(), [](float sample) { return sample == 0; }), "Deleted imported piece still renders audio");
    });
    suite.test("Main import rejects unsupported/corrupt files and recording/finalizing gates without publication", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; ReviewFolder folder; RecorderDocument document;
        require(document.saveCheckpoint(folder.root.getChildFile("project/project.recorder")).wasOk(), "Project fixture");
        const auto wav = recorder_import_test::writeWav(folder.root.getChildFile("fixtures"), 48000, 1, 4096);
        RecorderSettings settings(folder.root.getChildFile("settings")); MainComponent main(document, settings); ImportAccess::stopTimer(main);
        main.filesDropped({wav.withFileExtension("flac").getFullPathName()}, 0, 0);
        require(ImportAccess::message(main).contains(juce::String::fromUTF8("지원하지 않는")) && !ImportAccess::busy(main), "Unsupported drop needs Korean reason");
        main.filesDropped({wav.getFullPathName(), wav.getFullPathName()}, 0, 0);
        require(ImportAccess::message(main).contains(juce::String::fromUTF8("하나씩")), "Multiple files silently ignored");
        for (const auto gate : {RecorderLifecycle::recording, RecorderLifecycle::finalizing})
        {
            main.lifecycleState()->set(gate, true); ImportAccess::ready(main);
            require(!ImportAccess::enabled(main), "Recording/finalization left import button enabled");
            ImportAccess::click(main, wav); main.filesDropped({wav.getFullPathName()}, 0, 0);
            require(!ImportAccess::busy(main) && document.getProject().media->assets.empty(), "Lifecycle gate allowed import");
            main.lifecycleState()->end(gate);
        }
        const auto bad = wav.getSiblingFile("broken.wav"); require(bad.replaceWithText("broken WAV"), "Corrupt fixture");
        ImportAccess::click(main, bad); waitImportUi(main, [&] { return !ImportAccess::busy(main); });
        require(document.getProject().media->assets.empty() && ImportAccess::message(main).contains(juce::String::fromUTF8("실패")), "Corrupt import not rolled back/reported");
        require(main.lifecycleState()->canShutdown(), "Failure leaked lifecycle gate");
    });
    suite.test("Main import cancel, close request and direct destruction join without late publication", []
    {
        for (int mode = 0; mode < 3; ++mode)
        {
            juce::ScopedJuceInitialiser_GUI runtime; ReviewFolder folder; RecorderDocument document;
            const auto file = folder.root.getChildFile("project/project.recorder"); require(document.saveCheckpoint(file).wasOk(), "Project fixture");
            const auto wav = recorder_import_test::writeWav(folder.root.getChildFile("fixtures"), 44100, 2, 44100 * 4);
            RecorderSettings settings(folder.root.getChildFile("settings")); auto main = std::make_unique<MainComponent>(document, settings); ImportAccess::stopTimer(*main);
            ImportAccess::click(*main, wav); auto lifecycle = main->lifecycleState();
            if (mode == 0)
            {
                ImportAccess::cancel(*main); waitImportUi(*main, [&] { return !ImportAccess::busy(*main); });
                require(ImportAccess::message(*main).contains(juce::String::fromUTF8("취소")) && lifecycle->canShutdown(), "Cancel reason/gate missing");
            }
            else if (mode == 1)
            {
                bool closed = false; main->requestClose([&] { closed = true; }); waitImportUi(*main, [&] { return closed; });
                require(!(lifecycle->snapshot() & RecorderLifecycle::fileWork), "Close callback ran before file barrier");
            }
            main.reset();
            require(document.getProject().media->assets.empty() && document.getProject().tracks.empty(), "Cancelled/closed worker published late");
            require(file.getParentDirectory().getChildFile("media/imports").findChildFiles(juce::File::findDirectories, false).isEmpty(), "Cancelled import left copied asset");
            require(wav.existsAsFile(), "Close deleted original input");
        }
    });
    for (unsigned channels : {1u, 2u}) for (const auto gap : recorder_audio_fixture::recordedGaps)
    {
        const auto name = "App session playback: " + std::to_string(channels) + " channels, " + recorder_audio_fixture::gapName(gap);
        suite.test(name.c_str(), [=]
        {
            recorder_audio_fixture::RecordedGapFixture f(channels, gap);
            const auto plan = RenderPlanCompiler::compile(f.project);
            PlaybackAudioTrack track; track.trackId = f.project.tracks[1].trackId;
            std::vector<AudioSourceBinding> bindings;
            for (const auto& clip : plan->activeClips) if (clip.trackId == track.trackId)
            {
                const auto& asset = *f.project.media->findAsset(clip.assetId);
                const auto wav = RecorderSession::indexRecordedAudio(asset, f.root, f.project.Fs, track.trackId);
                require(wav->current() && wav->generation == std::uint64_t(asset.mediaGeneration)
                    && wav->length == asset.logicalLength && wav->channels == channels, "App source identity, logical length and layout");
                if (asset.availableRanges.empty()) require(wav->chunks.empty(), "All-gap assets must not open even an existing WAV");
                track.clips.push_back({clip, wav}); bindings.push_back({asset.assetId, wavAudioSource(wav, true)});
            }
            require(track.clips.size() == 2, "Index both damaged and healthy takes before preparing playback");
            TimelineAudioRenderer renderer(f.project.Fs, 256); renderer.setPlan({track}, f.totalFrames);
            std::vector<float> l(f.totalFrames), r(f.totalFrames); renderer.renderAudio(0, unsigned(f.totalFrames), l.data(), r.data());
            f.verifyPcm(l, r);
            const auto compiled = recorder_audio_fixture::render(f.project, bindings, {}, {0, f.totalFrames}, 127);
            const auto alternate = recorder_audio_fixture::render(f.project, bindings, {}, {0, f.totalFrames}, 509);
            f.verifyPcm(compiled.left, compiled.right);
            require(compiled.left == alternate.left && compiled.right == alternate.right, "Gap playback is independent of render block size");
        });
    }
    suite.test("App playback indexes stereo writer PCM across the 30-second chunk boundary", [] { checkAppPlayback({2}); });
    suite.test("App playback indexes mixed 1/2/1 slots and agrees with export sources", [] { checkAppPlayback({1, 2, 1}); });
    for (unsigned channels : {1u, 2u})
    {
        const auto name = "App playback indexes recovered prefix and torn sample tail: " + std::to_string(channels) + " channels";
        suite.test(name.c_str(), [=]
        {
            ReviewFolder f; RecorderProject initial; initial.Fs = 8000;
            RecoveryScanner::writeCheckpoint(f.root.getChildFile("project.recorder"), initial);
            const auto c = writeReviewTake(f.root, {channels}, 13); const auto original = f.root.getChildFile(WavTrackWriter::chunkPath(c.takeId, 1, 1));
            juce::MemoryBlock torn; require(original.loadFileAsData(torn), "Read recorded PCM fixture");
            // Mono odd-length PCM has a RIFF pad byte; tear PCM, not just padding.
            torn.setSize(44 + 13 * channels * 3 - 1);
            require(original.replaceWithData(torn.getData(), torn.getSize()), "Tear final channel sample");
            RecoveryReport recovered; const auto result = RecoveryScanner().run(f.root, recovered); require(result.wasOk(), result.getErrorMessage().toRawUTF8());
            require(recovered.project.media->takes.size() == 1, "Recover recorded take");
            const auto* asset = recovered.project.media->findAsset(recovered.project.media->takes[0].microphoneAssetIds[0]);
            require(asset && asset->originalFormat.channels == int(channels) && asset->logicalLength == 13 && asset->gaps.size() == 1
                && asset->gaps[0].start == 12 && asset->gaps[0].length == 1, "Recover complete channel frames and explicit tail gap");
            const auto source = RecorderSession::indexRecordedAudio(*asset, f.root, 8000, "recovered-mic");
            require(source->channels == channels && source->length == 13 && source->chunks[0].validBytes == 44 + 12 * channels * 3, "Recovered app durable watermark");
            TimelineAudioRenderer renderer(8000, 16); PlaybackAudioTrack track; track.trackId = "recovered-mic";
            RenderClip clip; clip.trackId = track.trackId; clip.lengthSamples = 13; clip.mediaGeneration = asset->mediaGeneration;
            clip.gaps = asset->gaps; track.clips.push_back({clip, source}); renderer.setPlan({track}, 13);
            float left[13]{}, right[13]{}; renderer.renderAudio(0, 13, left, right);
            for (unsigned i = 0; i < 13; ++i) require(left[i] == (i < 12 ? float(reviewPcm(i, 0)) / 8388608.0f : 0)
                && right[i] == (i < 12 ? float(reviewPcm(i, channels == 2 ? 1 : 0)) / 8388608.0f : 0), "Recovered mono/stereo prefix/tail playback");
            juce::MemoryBlock after; require(original.loadFileAsData(after) && after == torn, "Playback/recovery altered original WAV");
        });
    }
    for (unsigned channels : {1u, 2u})
    {
        const auto name = "App playback after recovery of a missing take plus healthy take: " + std::to_string(channels) + " channels";
        suite.test(name.c_str(), [=]
        {
            ReviewFolder f; RecorderProject initial; initial.Fs = 8000;
            RecoveryScanner::writeCheckpoint(f.root.getChildFile("project.recorder"), initial);
            const auto lost = writeReviewTake(f.root, {channels}, 13);
            const auto missing = f.root.getChildFile(WavTrackWriter::chunkPath(lost.takeId, 1, 1));
            require(missing.deleteFile(), "Remove isolated take WAV before real recovery");
            writeReviewTake(f.root, {channels}, 13);
            RecoveryReport recovered; const auto result = RecoveryScanner().run(f.root, recovered);
            require(result.wasOk(), result.getErrorMessage().toRawUTF8());
            require(recovered.project.media->takes.size() == 2 && recovered.project.activeTimelineEnd() == 26, "Recover both takes and preserve placement");
            PlaybackAudioTrack track; unsigned empty = 0, healthy = 0;
            for (const auto& lane : recovered.project.tracks) if (lane.kind == TrackKind::mic)
            {
                track.trackId = lane.trackId;
                for (const auto& c : lane.clips.items())
                {
                    const auto& a = *recovered.project.media->findAsset(c.assetId);
                    const auto source = RecorderSession::indexRecordedAudio(a, f.root, 8000, lane.trackId);
                    if (a.availableRanges.empty())
                    {
                        ++empty; require(a.chunks.empty() && a.relativePath.isNotEmpty() && a.gaps.size() == 1
                            && a.gaps[0].start == 0 && a.gaps[0].length == 13 && source->chunks.empty(), "Real recovery retains only a placeholder path for an all-gap take");
                    }
                    else ++healthy;
                    RenderClip clip; clip.clipId = c.clipId; clip.assetId = c.assetId; clip.trackId = lane.trackId;
                    clip.timelineStartSample = c.timelineStartSample; clip.lengthSamples = c.lengthSamples;
                    clip.mediaGeneration = a.mediaGeneration; clip.gaps = a.gaps; track.clips.push_back({clip, source});
                }
            }
            require(empty == 1 && healthy == 1, "Missing and healthy recovery paths both indexed");
            TimelineAudioRenderer renderer(8000, 16); renderer.setPlan({track}, 26);
            float l[26]{}, r[26]{}; renderer.renderAudio(0, 26, l, r);
            for (const auto& c : track.clips) for (unsigned i = 0; i < 13; ++i)
            {
                const bool silence = c.source->chunks.empty(); const auto at = c.mapping.timelineStartSample + i;
                require(l[at] == (silence ? 0 : float(reviewPcm(i, 0)) / 8388608.0f)
                    && r[at] == (silence ? 0 : float(reviewPcm(i, channels == 2 ? 1 : 0)) / 8388608.0f), "Recovered missing take is silent and healthy PCM plays");
            }
            require(!missing.exists(), "Indexing must not recreate missing media");
        });
    }
    for (unsigned channels : {1u, 2u})
    {
        const auto name = "App single-file WAV fallback respects durable availability: " + std::to_string(channels) + " channels";
        suite.test(name.c_str(), [=]
        {
            ReviewFolder f; const auto c = writeReviewTake(f.root, {channels}, 13);
            MediaAsset asset; asset.kind = AssetKind::mic; asset.mediaGeneration = 1; asset.logicalLength = 17;
            asset.originalFormat.channels = int(channels); asset.relativePath = WavTrackWriter::chunkPath(c.takeId, 1, 1);
            asset.availableRanges = {{0, 5}, {5, 8}}; asset.gaps = {{13, 4}};
            const auto wav = RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic");
            require(wav->length == 17 && wav->chunks.size() == 1 && wav->chunks[0].validSamples == 13
                && wav->chunks[0].validBytes == 44 + 13 * channels * 3, "Single-file fallback uses only committed frames and bytes");
            float l[17]{}, r[17]{}; wavAudioSource(wav, true)->read(0, 17, l, r);
            for (unsigned i = 0; i < 17; ++i)
                require(l[i] == (i < 13 ? float(reviewPcm(i, 0)) / 8388608.0f : 0)
                    && r[i] == (i < 13 ? float(reviewPcm(i, channels == 2 ? 1 : 0)) / 8388608.0f : 0), "Single-file logical tail supplies silence");
            for (const auto& ranges : std::vector<std::vector<SampleRange>>{{{1, 12}}, {{0, 0}}, {{0, -1}}, {{0, 18}},
                    {{0, 5}, {6, 7}}, {{0, 8}, {7, 6}}, {{0, 17}}})
            {
                asset.availableRanges = ranges;
                rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic"); });
            }
            asset.availableRanges = {{0, 13}}; asset.relativePath = "media/missing.wav";
            rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic"); });
            asset.availableRanges.clear(); asset.gaps = {{0, 17}}; asset.chunks = {{"media/stale.wav", {0, 13}}};
            require(RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic")->chunks.empty(), "No available PCM means silence even with stale chunk metadata");
            rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 0, "mic"); });
        });
    }
    suite.test("App WAV index rejects invalid channel metadata, duration and chunk ranges", []
    {
        ReviewFolder f; const auto c = writeReviewTake(f.root, {2}, 13);
        MediaAsset asset; asset.kind = AssetKind::mic; asset.mediaGeneration = 1; asset.logicalLength = 13;
        asset.availableRanges = {{0, 13}};
        asset.chunks = {{WavTrackWriter::chunkPath(c.takeId, 1, 1), {0, 13}}};
        for (int channels : {-1, 0, 1, 3}) { asset.originalFormat.channels = channels; rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic"); }); }
        asset.originalFormat.channels = 2; asset.logicalLength = 12; rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic"); });
        asset.logicalLength = 13; asset.chunks[0].sourceRange.length = -1; rejects([&] { RecorderSession::indexRecordedAudio(asset, f.root, 8000, "mic"); });
    });
    suite.test("Audio controls round trip every 0.1.4 mono/stereo input mapping with immediate edits", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; RecorderProject project; auto empty = reviewSettings();
        empty.physicalInputs.assign(8, -1); const auto device = audioSettingsDevice();
        AudioSettingsPanel panel(empty, project, device); unsigned edits = 0; auto requested = empty;
        panel.onChanged = [&](UserSettings next) { ++edits; requested = std::move(next); };
        for (unsigned slot = 0; slot < 8; ++slot) for (int physical = -1; physical < 8; ++physical) for (bool stereo : {false, true})
        {
            if (stereo && (physical < 0 || physical == 7)) continue;
            panel.setSettings(empty); edits = 0; requested = empty;
            const auto number = juce::String(slot + 1);
            auto& input = audioControl<juce::ComboBox>(panel, "microphoneInput" + number);
            auto& mono = audioControl<juce::TextButton>(panel, "microphoneMono" + number);
            auto& pair = audioControl<juce::TextButton>(panel, "microphoneStereo" + number);
            require(input.getNumItems() == 9 && input.getItemId(0) == 1 && input.getItemId(8) == 9,
                "Physical input menu still mixes stereo pairs with mono entries");
            input.setSelectedId(physical + 2, juce::sendNotificationSync);
            if (stereo) { require(pair.isEnabled(), "Valid adjacent pair unavailable"); pair.setToggleState(true, juce::sendNotificationSync); }
            auto expected = empty; expected.physicalInputs[slot] = physical; expected.stereoSlots[slot] = stereo;
            const auto visible = panel.read(empty);
            require(visible.physicalInputs == expected.physicalInputs && visible.stereoSlots == expected.stereoSlots
                && requested.physicalInputs == expected.physicalInputs && requested.stereoSlots == expected.stereoSlots
                && visible.validate(8).wasOk(), "New controls changed the persisted left-index/stereo contract");
            require(edits == (physical >= 0 ? 1u : 0u) + unsigned(stereo), "Control edit was delayed, duplicated or omitted");
            require(pair.getToggleState() == stereo && mono.getToggleState() == (physical >= 0 && !stereo), "Channel buttons disagree with read()");
            if (stereo)
            {
                require(audioControl<juce::Label>(panel, "microphoneHint" + number).getText().contains(
                    ko("오른쪽 = ") + juce::String(physical + 2) + " · Analog " + juce::String(physical + 2)), "Automatic right channel name is missing");
                input.setSelectedId(1, juce::sendNotificationSync);
                const auto disabled = panel.read(empty);
                require(disabled.physicalInputs[slot] == -1 && !disabled.stereoSlots[slot] && !pair.isEnabled()
                    && !mono.isEnabled() && !mono.getToggleState(), "Use none retained a highlighted or enabled channel selector");
            }
        }
    });
    suite.test("0.1.4 XML mixed slots restore both controls and preserve engine mapping", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; ReviewFolder f; RecorderSettings saved(f.root);
        // Frozen 0.1.4 field layout; no new serializer or migration is involved.
        juce::PropertySet properties; properties.setValue("schemaVersion", 1); properties.setValue("productId", ProductIdentity::internalId());
        properties.setValue("asioDeviceId", "synthetic-native-PCM"); properties.setValue("audioDefaultsApplied", true);
        properties.setValue("physicalInputs", "[0,2,-1,4,6,-1,-1,7]");
        properties.setValue("stereoSlot0", true); properties.setValue("stereoSlot3", true);
        properties.setValue("outputLeft", juce::var(0)); properties.setValue("outputRight", 1);
        require(saved.getFile().getParentDirectory().createDirectory().wasOk()
            && saved.getFile().replaceWithText(properties.createXml("RECORDER_SETTINGS")->toString()), "Write 0.1.4 XML fixture");
        const auto loaded = saved.load(); if (loaded.failed()) throw std::runtime_error(loaded.getErrorMessage().toStdString());
        const auto legacy = saved.get(); RecorderProject project; const auto device = audioSettingsDevice();
        AudioSettingsPanel panel(reviewSettings(), project, device); unsigned edits = 0;
        panel.onChanged = [&](UserSettings) { ++edits; };
        panel.setSettings(legacy); panel.setDeviceInfo(device);
        const auto visible = panel.read(legacy);
        require(edits == 0 && visible.physicalInputs == legacy.physicalInputs && visible.stereoSlots == legacy.stereoSlots,
            "Loading an applied configuration changed its selection or submitted another edit");
        for (unsigned slot = 0; slot < 8; ++slot)
        {
            const auto number = juce::String(slot + 1);
            require(audioControl<juce::ComboBox>(panel, "microphoneInput" + number).getSelectedId() == legacy.physicalInputs[slot] + 2
                && audioControl<juce::TextButton>(panel, "microphoneStereo" + number).getToggleState() == legacy.stereoSlots[slot]
                && audioControl<juce::TextButton>(panel, "microphoneMono" + number).getToggleState() == (legacy.physicalInputs[slot] >= 0 && !legacy.stereoSlots[slot]), "Mixed legacy slot selection is not displayed");
        }
        RecorderAudioEngine engine; applySyntheticSettings(engine, visible);
        require(engine.openSynthetic(48000, 256, 8, 2).wasOk()
            && engine.calibrationInputMapping() == std::vector<int>({1,0,1, 2,2,-1, 4,4,5, 5,6,-1, 8,7,-1}), "Legacy L/R mapping changed at engine boundary");
        require(saved.set(visible).wasOk() && saved.save().get().wasOk(), "Save round-tripped UI settings");
        RecorderSettings reloaded(f.root); require(reloaded.load().wasOk() && reloaded.get().physicalInputs == legacy.physicalInputs
            && reloaded.get().stereoSlots == legacy.stereoSlots, "UI round trip changed saved XML mapping");
    });
    suite.test("Last input disables stereo with a visible reason and permits explicit mono", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; RecorderProject project;
        for (int count : {0, 1, 8})
        {
            auto s = reviewSettings(); s.physicalInputs.assign(8, -1); if (count) s.physicalInputs[0] = count - 1;
            AudioSettingsPanel panel(s, project, audioSettingsDevice(count));
            auto& input = audioControl<juce::ComboBox>(panel, "microphoneInput1");
            auto& mono = audioControl<juce::TextButton>(panel, "microphoneMono1");
            auto& stereo = audioControl<juce::TextButton>(panel, "microphoneStereo1");
            require(!stereo.isEnabled() && mono.isEnabled() == (count > 0) && input.getNumItems() == count + 1, "Absent right input did not disable stereo");
            const auto hint = audioControl<juce::Label>(panel, "microphoneHint1").getText();
            require(hint.contains(count ? ko("마지막 입력") : ko("물리 입력을 선택")), "Disabled selector has no visible reason");
            if (count > 1)
            {
                input.setSelectedId(count, juce::sendNotificationSync); stereo.setToggleState(true, juce::sendNotificationSync);
                require(!input.isItemEnabled(count + 1), "Stereo may move its left channel to the last physical input");
                mono.setToggleState(true, juce::sendNotificationSync);
                require(input.isItemEnabled(count + 1), "Changing to mono did not release the last input");
                input.setSelectedId(count + 1, juce::sendNotificationSync);
                require(panel.read(s).physicalInputs[0] == count - 1 && !panel.read(s).stereoSlots[0], "Explicit last-input mono changed layout");
            }
        }
    });
    suite.test("Occupied left and right inputs show slot numbers across pages and release immediately", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; RecorderProject project; auto s = reviewSettings();
        s.physicalInputs = {0, -1, -1, -1, 4, -1, -1, -1}; s.stereoSlots[0] = s.stereoSlots[4] = true; s.microphoneArmed[4] = false;
        AudioSettingsPanel panel(s, project, audioSettingsDevice());
        auto& target = audioControl<juce::ComboBox>(panel, "microphoneInput2");
        for (int id : {2, 3, 6, 7}) require(!target.isItemEnabled(id), "An occupied L/R input is still selectable");
        require(target.getItemText(2).contains(ko("마이크 1")) && target.getItemText(6).contains(ko("마이크 5")), "Disabled choices omit their owning slots");
        auto& own = audioControl<juce::ComboBox>(panel, "microphoneInput1");
        require(own.isItemEnabled(2) && own.isItemEnabled(3), "A slot conflicts with its own channels");
        target.setSelectedId(4, juce::sendNotificationSync); // input 3, with input 4 available on its right
        audioControl<juce::TextButton>(panel, "microphoneStereo2").setToggleState(true, juce::sendNotificationSync);
        require(!target.isItemEnabled(5) && target.getItemText(4).contains(ko("마이크 5")), "Candidate stereo right channel ignored the hidden unarmed slot");
        audioControl<juce::ComboBox>(panel, "microphoneInput5").setSelectedId(1, juce::sendNotificationSync);
        require(target.isItemEnabled(5) && target.isItemEnabled(6) && target.isItemEnabled(7), "Disabling a slot did not release both physical channels");
    });
    suite.test("Stereo conflicts are explained before apply and include every overlapping slot", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; RecorderProject project; const auto s = reviewSettings();
        AudioSettingsPanel panel(s, project, audioSettingsDevice());
        require(!audioControl<juce::TextButton>(panel, "microphoneStereo1").isEnabled()
            && audioControl<juce::Label>(panel, "microphoneHint1").getText().contains(ko("마이크 2")), "Occupied adjacent input needs a visible stereo explanation");
        unsigned callbacks = 0;
        panel.onChanged = [&](UserSettings next)
        {
            ++callbacks;
            const auto hint = audioControl<juce::Label>(panel, "microphoneHint3").getText();
            require(hint.contains(ko("마이크 1")), "Overlap explanation arrived after onChanged");
            if (next.stereoSlots[2]) require(hint.contains(ko("마이크 2")), "Stereo overlap only reports one of its channels");
        };
        // Inject stale popup/toggle notifications; ordinary UI choices are already disabled.
        audioControl<juce::ComboBox>(panel, "microphoneInput3").setSelectedId(2, juce::sendNotificationSync);
        audioControl<juce::TextButton>(panel, "microphoneStereo3").setToggleState(true, juce::sendNotificationSync);
        require(callbacks == 2 && panel.read(s).validate(8).failed(), "Stale input event bypassed the existing validation contract");
    });
    suite.test("Four-slot pages fit the fixed panel and expose all eight summaries without applying", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; RecorderProject project; auto s = reviewSettings();
        s.physicalInputs = {0, 2, -1, 6, -1, -1, -1, 7}; s.stereoSlots[0] = true;
        gocue::livemix::LiveMixLookAndFeel lookAndFeel;
        AudioSettingsPanel panel(s, project, audioSettingsDevice()); panel.setLookAndFeel(&lookAndFeel);
        unsigned edits = 0; panel.onChanged = [&](UserSettings) { ++edits; };
        for (int width : {500, 684}) for (unsigned page = 0; page < 2; ++page)
        {
            panel.setSize(width, 588); audioControl<juce::TextButton>(panel, "microphonePage" + juce::String(page + 1)).onClick();
            for (unsigned slot = 0; slot < 8; ++slot)
            {
                const auto number = juce::String(slot + 1); auto& input = audioControl<juce::ComboBox>(panel, "microphoneInput" + number);
                auto& summary = audioControl<juce::Label>(panel, "microphoneSummary" + number);
                auto& hint = audioControl<juce::Label>(panel, "microphoneHint" + number);
                auto& mono = audioControl<juce::TextButton>(panel, "microphoneMono" + number);
                auto& stereo = audioControl<juce::TextButton>(panel, "microphoneStereo" + number);
                require(input.isVisible() == (slot / 4 == page) && summary.isVisible() == input.isVisible(), "Page hides a slot or shows the wrong slot number");
                if (!input.isVisible()) continue;
                require(panel.getLocalBounds().contains(hint.getBounds()) && input.getWidth() >= 280
                    && summary.getBottom() <= input.getY() && input.getRight() < mono.getX() && mono.getRight() <= stereo.getX()
                    && input.getBottom() <= hint.getY(), "New audio rows overflow or overlap at the supported panel size");
                require(summary.getText().startsWith(ko("마이크 ") + number + ko(" · ")), "Slot summary lost its logical microphone number");
            }
            juce::Image image(juce::Image::ARGB, width, 588, true, juce::SoftwareImageType{}); juce::Graphics graphics(image);
            graphics.fillAll(Palette::background); panel.paintEntireComponent(graphics, true);
            const auto snapshotPath = juce::SystemStats::getEnvironmentVariable("RECORDER_AUDIO_UI_SNAPSHOTS", {});
            if (snapshotPath.isNotEmpty())
            {
                require(juce::File::isAbsolutePath(snapshotPath), "Snapshot directory must be absolute");
                const juce::File directory(snapshotPath); juce::MemoryOutputStream png;
                require(directory.createDirectory().wasOk() && juce::PNGImageFormat().writeImageToStream(image, png)
                    && directory.getChildFile("audio-" + juce::String(width) + "-page" + juce::String(page + 1) + ".png")
                        .replaceWithData(png.getData(), png.getDataSize()), "Write injected-device panel render");
            }
        }
        require(edits == 0 && panel.read(s).physicalInputs == s.physicalInputs
            && audioControl<juce::TextButton>(panel, "microphonePage2").getButtonText().contains(ko("1개 사용")), "Paging applies settings or omits active slots on the other page");
    });
    suite.test("Device selection retains first-run defaults and later explicit none choices", []
    {
        juce::ScopedJuceInitialiser_GUI runtime; RecorderProject project; auto s = reviewSettings(); s.physicalInputs[1] = 2; s.stereoSlots[0] = true;
        AudioSettingsPanel panel(s, project, audioSettingsDevice()); auto requested = s; unsigned edits = 0;
        panel.onChanged = [&](UserSettings next) { ++edits; requested = std::move(next); };
        auto& devices = audioControl<juce::ComboBox>(panel, "audioDevice");
        devices.addItem("injected-new-device", 99999); devices.setSelectedId(99999, juce::sendNotificationSync);
        require(edits == 1 && requested.asioDeviceId == "injected-new-device" && requested.physicalInputs.empty()
            && requested.stereoSlots == std::array<bool,8>{} && !requested.audioDefaultsApplied
            && requested.output.left == -1 && requested.output.right == -1, "New device retained the previous device's map/defaults marker");
        auto device = audioSettingsDevice(); device.name = requested.asioDeviceId;
        require(applyAudioDefaults(requested, device) && requested.physicalInputs[0] == 0 && !requested.stereoSlots[0], "First apply no longer supplies mono input 1");
        panel.setSettings(requested); panel.setDeviceInfo(device);
        require(audioControl<juce::ComboBox>(panel, "microphoneInput1").getSelectedId() == 2
            && audioControl<juce::TextButton>(panel, "microphoneMono1").getToggleState(), "Applied first-run defaults are not visible");
        audioControl<juce::ComboBox>(panel, "microphoneInput1").setSelectedId(1, juce::sendNotificationSync);
        require(requested.physicalInputs[0] == -1 && requested.audioDefaultsApplied && !applyAudioDefaults(requested, device), "Explicit none reapplied first-run defaults");
        panel.setBusy(true);
        require(audioControl<juce::ComboBox>(panel, "microphoneInput1").isEnabled(), "Configuring prevents queued microphone edits");
    });
    suite.test("Rejected stereo input restores visible, saved and engine settings synchronously", [] { checkRejectedSelection(false); });
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
        asset.availableRanges = {{0, 5}}; asset.gaps = {{5, 3}};
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
    suite.test("Synthetic audio settings owner blocks native device creation for hardware-free validation", []
    {
        auto owner = std::make_unique<AsioTimingBridge>(1000000);
        require(owner->registerTap(), "Reserve the synthetic ASIO owner before any device request");
        RecorderAudioEngine engine;
        const auto blocked = engine.openDevice("recorder-test-no-physical-device", 48000, 256);
        require(blocked.failed() && (!AsioTimingBridge::hookCompiled() || blocked.getErrorMessage().contains("ASIO owner"))
            && engine.deviceInfo().sampleRate == 0, "The injected owner must reject before native device creation");
        // TestMain runs ui-wiring after ASIO ownership tests and before lifecycle.
        // That existing suite can otherwise open the PC's first real ASIO driver.
        // Opt-in keeps the same synthetic owner until process exit, so full-suite
        // validation uses its device-unavailable path without modifying the engine.
        if (juce::SystemStats::getEnvironmentVariable("RECORDER_TEST_NO_HARDWARE", {}) == "1")
        {
            static std::unique_ptr<AsioTimingBridge> fullRunOwner;
            fullRunOwner = std::move(owner);
            std::cout << "Injected ASIO owner retained: native device creation blocked for remaining suites\n";
        }
    });
    return suite.result("ui-wiring");
}
