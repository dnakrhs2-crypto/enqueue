#include "TestSupport.h"
#include "record/TakeController.h"
#include <chrono>
#include <thread>

using namespace gocue::recorder;
using recorder_test::require;
namespace
{
void ok(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
template<class F> void until(F f)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!f()) { require(std::chrono::steady_clock::now() < end, "Take worker timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
struct DoubleState
{
    std::atomic<bool> finishing{false}, release{true}, failed{false};
    std::atomic<std::int64_t> available{0};
    std::atomic<unsigned> audioPackets{0}, videoOffers{0};
};
class VideoDouble final : public ITakeVideoStream
{
public:
    explicit VideoDouble(std::shared_ptr<DoubleState> s) : state(std::move(s)) {}
    void prepare(const juce::File& file, NvencProfile, Rational, const AVCodecContext& audio) override
    {
        require(audio.codec_id == AV_CODEC_ID_AAC && audio.sample_rate == 48000, "Real AAC setup supplied to video double");
        output = file;
        // Explicit test placeholder, never reported by production probes as media.
        DurableFile f; ok(f.open(output.getSiblingFile("cam1.recording.mp4"), DurableFile::OpenMode::createNew));
        const char text[] = "lifecycle-test-double"; ok(f.write(text, sizeof(text))); ok(f.flushData()); ok(f.close());
    }
    void startAt(ClockMapping clock, std::int64_t origin, unsigned, std::function<std::int64_t()>) override
    { require(clock.valid && origin >= 0, "Video receives valid N0/clock"); }
    void offer(const VideoSurface&) noexcept override { ++state->videoOffers; }
    void audioPacket(const AVPacket&) override { ++state->audioPackets; }
    bool ready() const noexcept override { return true; }
    void sourceFailed(std::int64_t sample) noexcept override { if (!state->failed.exchange(true)) state->available = sample; }
    void endAt(std::int64_t length) noexcept override { if (!state->failed.load()) state->available = length; }
    void audioDone() noexcept override {}
    void finish() override
    {
        state->finishing = true; until([&] { return state->release.load(); });
        if (!output.existsAsFile()) require(output.getSiblingFile("cam1.recording.mp4").moveFileTo(output), "Double final rename");
    }
    bool failed() const noexcept override { return state->failed.load(); }
    std::int64_t availableSamples() const noexcept override { return state->available.load(); }
    bool thumbnailReady() const noexcept override { return false; }
    juce::var report() const override { auto v = jsonObject(); jsonSet(v, "source", "lifecycle test double; not encoded media"); return v; }
private:
    std::shared_ptr<DoubleState> state;
    juce::File output;
};
struct Fixture
{
    RecorderDocument document;
    RecorderAudioEngine audio;
    std::shared_ptr<DoubleState> video = std::make_shared<DoubleState>();
    TakeController controller{document, audio, [this] { return std::make_unique<VideoDouble>(video); }};
    TakeController::Config config;
    std::int64_t position = 0, qpc = qpcNow(); std::uint64_t sequence = 0;
    unsigned microphones; bool stereo;
    Fixture(unsigned mics = 1, bool stereoSlot = false) : microphones(mics), stereo(stereoSlot)
    {
        std::array<int, 8> map{-1,-1,-1,-1,-1,-1,-1,-1};
        if (mics) map[5] = 2; // sparse logical mic06, physical input3
        std::array<bool, 8> slots{}; slots[5] = stereo;
        ok(audio.setInputMap(map, slots)); ok(audio.openSynthetic(8000, 80, 8, 2)); if (mics) ok(audio.arm(5, true));
        document.newProject("Take lifecycle", 8000, {60,1});
        config.projectDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("TakeControllerTests-" + juce::Uuid().toString());
        config.synthetic = true; config.projectFps = 60; config.cameraMode.width = 1920; config.cameraMode.height = 1080;
        config.cameraMode.fps = {30,1};
        for (int i = 0; i < 4; ++i) feed(); until([&] { return audio.clockReady(); });
    }
    ~Fixture() { video->release = true; }
    void feed(unsigned Fs = 8000)
    {
        std::array<std::uint8_t, 240> pcm{};
        std::array<float, 80> left{}, right{}, input{};
        for (unsigned i = 0; i < 80; ++i) WavTrackWriter::packPcm24(123456 + int(position + i), pcm.data() + i * 3);
        auto rightPcm = pcm; for (unsigned i = 0; i < 80; ++i) WavTrackWriter::packPcm24(-345678 - int(position + i), rightPcm.data() + i * 3);
        NativeInputView views[]{{pcm.data(), 0, 2, nativeFormatForAsio(17)}, {rightPcm.data(), 1, 3, nativeFormatForAsio(17)}};
        const float* inputs[] = {input.data(), input.data()}; float* outputs[] = {left.data(), right.data()};
        BlockStamp stamp{}; stamp.flags = samplePositionValid; stamp.sequence = sequence++; stamp.samplePosition = position;
        stamp.sampleRate = Fs; stamp.numSamples = 80; stamp.callbackQpc = qpc + position * qpcFrequency() / 8000;
        audio.processBlock(stamp, microphones ? views : nullptr, microphones ? (stereo ? 2 : 1) : 0, inputs, outputs, 2); position += 80;
    }
    void arm()
    {
        ok(controller.prepare(config)); require(controller.state() == TakeController::State::preparing, "Preparation is asynchronous");
        until([&] { controller.tick(); return controller.state() == TakeController::State::armed; });
    }
    std::int64_t begin()
    {
        arm(); const auto origin = position + 81; ok(controller.start(origin));
        until([&] { return audio.startCommitted(); });
        while (audio.startSample() < 0) { feed(); controller.tick(); }
        require(controller.state() == TakeController::State::recording, "Callback acknowledgement enters recording"); return origin;
    }
    void complete()
    {
        until([&] { controller.tick(); return controller.state() == TakeController::State::done || controller.state() == TakeController::State::partialFailure; });
    }
};
}
int runTakeControllerTests()
{
    recorder_test::Suite suite;
    suite.test("Stereo sparse microphone asset, capture snapshot and take manifest", []
    {
        Fixture f(1, true); const auto start = f.begin(); ok(f.controller.stop(start + 1601));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        require(f.controller.state() == TakeController::State::done, "Stereo take finalized");
        const auto& project = f.document.getProject(); const auto& take = project.media->takes.back();
        const auto* asset = project.media->findAsset(take.microphoneAssetIds[0]);
        require(asset && asset->originalFormat.channels == 2 && asset->logicalLength == 1601, "Stereo mic asset");
        require(take.capture.physicalInputs == std::vector<int>{2} && take.capture.physicalInputsRight == std::vector<int>{3}, "Capture pair persists");
        RecorderProject loaded; ok(RecorderSerializer::fromJson(RecorderSerializer::toJson(project), loaded));
        require(loaded.media->takes.back().capture.physicalInputsRight[0] == 3, "Project stereo round trip");
        auto manifest = juce::JSON::parse(f.config.projectDirectory.getChildFile("media/takes/" + f.config.takeId.toDashedString() + "/take.json"));
        require(int(manifest["microphones"][0]["leftPhysical"]) == 2 && int(manifest["microphones"][0]["rightPhysical"]) == 3
            && int(manifest["microphones"][0]["channels"]) == 2, "Take manifest stereo identity");
    });
    suite.test("Sample-defined CFR ceil including 44.1k and partial-frame stops", []
    {
        require(TakeController::frameCount(48000,48000,{60,1}) == 60, "One-second frame count");
        require(TakeController::frameCount(48001,48000,{60,1}) == 61, "One extra sample requires final frame padding");
        require(TakeController::frameCount(44101,44100,{30,1}) == 31, "44.1k boundary");
        require(TakeController::frameCount(0,48000,{60000,1001}) == 0, "Empty interval has zero frames");
        recorder_test::rejects([] { TakeController::frameCount(-1,48000,{60,1}); });
    });
    suite.test("First take persists the project rate it fixes before TakeStarted, so a crash mid-take still recovers", []
    {
        Fixture f; f.config.projectDirectory.createDirectory(); const auto file = f.config.projectDirectory.getChildFile("project.recorder");
        { RecorderDocument seed; seed.newProject("Adopt", 48000, {60,1}); ok(RecorderSerializer::writeCheckpoint(file, *seed.snapshot())); }
        ok(f.document.openCheckpoint(file)); require(f.document.getProject().Fs == 48000 && !f.document.isDirty(), "Saved provisional project at 48000");
        f.arm();
        RecorderProject onDisk; ok(RecorderSerializer::readCheckpoint(file, onDisk));
        require(onDisk.Fs == 8000 && f.document.getProject().Fs == 8000 && !f.document.isDirty(), "Checkpoint on disk carries the fixed rate before the take starts");
        const auto origin = f.position + 81; ok(f.controller.start(origin)); until([&] { return f.audio.startCommitted(); });
        JournalReplay replay; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), replay));
        require(!replay.records.empty() && replay.records.front().kind == JournalKind::TakeStarted && unsigned(int(replay.records.front().payload["pcm"]["sampleRate"])) == onDisk.Fs, "Journal rate matches the persisted project");
        while (f.audio.startSample() < 0) { f.feed(); f.controller.tick(); }
        require(f.controller.state() == TakeController::State::recording, "Recording after the early checkpoint");
        ok(f.controller.stop(origin + 401)); while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); }
        f.complete(); require(f.controller.state() == TakeController::State::done, "Take completes after the early checkpoint");
    });
    suite.test("Lifecycle / immediate placement independent of finalizer / sparse mic lane / journal order", []
    {
        Fixture f; const auto n0 = f.begin();
        require(f.controller.structureEditingLocked(), "Structure locked during recording");
        require(f.document.performEdit("Forbidden", [](EditState& e) { e.name = "changed"; }).failed(), "Document enforces lock");
        require(f.document.undo().failed(), "Undo locked");
        bool placementNotified = false, placementLocked = false;
        f.document.onChanged = [&]
        {
            if (!placementNotified && !f.document.getProject().media->takes.empty())
            { placementNotified = true; placementLocked = f.document.isRecordingStructureLocked(); }
        };
        const auto length = 1601; f.video->release = false; ok(f.controller.stop(n0 + length));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); }
        f.controller.tick();
        require(f.controller.state() == TakeController::State::finalizing, "Stop enters finalizing without joining worker");
        require(!f.controller.structureEditingLocked() && f.document.getProject().media->takes.size() == 1, "Clip placed before finalizer completes");
        f.document.onChanged = {};
        require(placementNotified && placementLocked, "Coordinator unlocked user edits before publishing recorded take");
        require(f.document.getProject().media->takes[0].logicalLength == length, "Logical end remains exact Nstop");
        require(f.controller.placementMetadata().ready && f.controller.placementMetadata().Nstop - f.controller.placementMetadata().N0 == length,
                "Duration/peak/thumbnail cache available before finalizer drain");
        bool mic06 = false; for (const auto& t : f.document.getProject().tracks) if (t.kind == TrackKind::mic) mic06 = t.microphoneIndex == 5;
        require(mic06, "Logical microphone lane retained");
        f.video->release = true; f.complete();
        require(f.controller.state() == TakeController::State::done, "Successful take done");
        require(f.controller.placementMetadata().peaksComplete && f.controller.placementMetadata().peaks[5] > 0, "Final raw peak cache after drain");
        const auto report = f.controller.report(); require(double(report["stopToPlacementMs"]) < 250, "Synthetic stop to placement below 250ms");
        const auto folder = f.config.projectDirectory.getChildFile("media/takes/" + f.config.takeId.toDashedString());
        require(folder.getChildFile("cam1.mp4").existsAsFile() && folder.getChildFile("audio/mic06/000001.wav").existsAsFile(), "Required take layout");
        require(folder.getChildFile("take.json").existsAsFile() && folder.getChildFile("index").isDirectory(), "Manifest/index created");
        require(!folder.getChildFile("cam2.mp4").exists() && !folder.getChildFile("cam2.recording.mp4").exists(), "No cam2 artifacts");
        JournalReplay replay; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), replay));
        require(replay.records.front().kind == JournalKind::TakeStarted && replay.records.back().kind == JournalKind::TakeFinalized, "Lifecycle journal ends ordered");
        int starts = 0, stops = 0, finals = 0; bool sawStop = false;
        for (const auto& r : replay.records)
        {
            starts += r.kind == JournalKind::TakeStarted; stops += r.kind == JournalKind::TakeStopped; finals += r.kind == JournalKind::TakeFinalized;
            if (r.kind == JournalKind::TakeStopped)
            {
                sawStop = true; require(r.payload["placementEditId"].toString() == juce::Uuid(f.document.lastEditTransaction()).toDashedString(), "TakeStopped linked to actual placement transaction");
                require(std::int64_t(r.payload["Nstop"]) == n0 + length, "Journal Nstop");
            }
            if (r.kind == JournalKind::TakeFinalized) require(sawStop, "Finalized follows stopped");
        }
        require(starts == 1 && stops == 1 && finals == 1, "Exactly one lifecycle record per event");
        RecorderProject reopened; ok(RecorderSerializer::readCheckpoint(f.config.projectDirectory.getChildFile("project.recorder"), reopened));
        require(reopened.media->takes[0].state == TakeState::complete && reopened.validate().wasOk(), "Finalized project roundtrip");
    });
    suite.test("Independent audio active end controls placement; markers excluded; zero microphones allowed", []
    {
        Fixture f(0); MediaAsset imported; imported.kind = AssetKind::importAudio; imported.relativePath = "media/imports/test.wav";
        imported.contentIdentity = imported.assetId; imported.logicalLength = 3000; imported.availableRanges = {{0,3000}};
        imported.originalFormat.codec = "pcm_s24le"; imported.originalFormat.sampleRate = 8000; imported.originalFormat.channels = 1; imported.originalFormat.bitsPerSample = 24;
        ok(f.document.registerMedia({imported}));
        ok(f.document.performEdit("Place independent audio", [&](EditState& e)
        {
            Track t; t.kind = TrackKind::importAudio; Clip c; c.trackId = t.trackId; c.assetId = imported.assetId; c.timelineStartSample = 1000; c.lengthSamples = 3000;
            t.clips.edit().push_back(c); e.tracks.push_back(t); Marker m; m.sample = 100000; e.markers.push_back(m);
        }));
        const auto n0 = f.begin(); require(f.controller.placementSample() == 4000, "Audio end, not marker, is reserved");
        require(f.controller.warning().isNotEmpty(), "No-mic warning"); ok(f.controller.stop(n0 + 401));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        require(f.controller.state() == TakeController::State::done, "Output-only recording succeeds");
        const auto& take = f.document.getProject().media->takes[0]; require(take.placementSample == 4000 && take.microphoneAssetIds.empty(), "Camera-only take placement");
        require(f.video->audioPackets.load() > 0, "Zero-mic reference has silent AAC packets");
    });
    suite.test("Camera failure leaves audio running and registers explicit camera tail gap", []
    {
        Fixture f; const auto n0 = f.begin(); f.feed(); f.controller.cameraFailed();
        const auto cameraEnd = f.video->available.load();
        for (int i = 0; i < 4; ++i) { f.feed(); f.controller.tick(); }
        require(f.controller.state() == TakeController::State::recording && f.audio.error() == RecorderAudioEngine::Error::none, "Camera failure does not abort originals");
        const auto nstop = f.position + 39; ok(f.controller.stop(nstop));
        while (f.audio.stopSample() < 0) f.feed(); f.complete();
        require(f.controller.state() == TakeController::State::partialFailure, "Stream failure reported partial");
        const auto& take = f.document.getProject().media->takes[0]; const auto* camera = f.document.getProject().media->findAsset(take.cam1AssetId);
        require(!camera->gaps.empty() && camera->gaps[0].start == cameraEnd, "Camera gap starts at failed boundary");
        const auto* mic = f.document.getProject().media->findAsset(take.microphoneAssetIds[0]);
        require(mic->gaps.empty() && mic->logicalLength == nstop - n0, "Microphone retains complete interval");
    });
    suite.test("Repeated source discontinuity/type-change notifications keep the take lane collecting", []
    {
        Fixture f;
        auto telemetry = std::make_shared<CaptureTelemetry>(f.config.cameraMode.fps);
        f.config.cameraTelemetry = telemetry; f.config.cameraGeneration = 7;
        f.arm(); VideoSurface frame; frame.stamp.generation = 7;
        telemetry->loss(LossReason::sourceDiscontinuity); f.controller.offer(frame);
        require(!f.video->failed, "Initial source notification must not fail an armed camera");
        const auto n0 = f.position + 81; ok(f.controller.start(n0));
        until([&] { return f.audio.startCommitted(); });
        while (f.audio.startSample() < 0) { f.feed(); f.controller.tick(); }
        for (unsigned i = 0; i < 60; ++i)
        {
            telemetry->loss(i % 2 ? LossReason::sourceTypeChanged : LossReason::sourceDiscontinuity);
            frame.stamp.frame = i + 1; f.controller.offer(frame); f.feed(); f.controller.tick();
            require(!f.video->failed && f.controller.state() == TakeController::State::recording,
                    "Every recoverable notification must leave collection running");
        }
        require(f.video->videoOffers == 61, "Every post-notification frame reached the stream");
        ok(f.controller.stop(f.position + 39)); while (f.audio.stopSample() < 0) f.feed(); f.complete();
        const auto& p = f.document.getProject(); const auto& take = p.media->takes.front();
        require(f.controller.state() == TakeController::State::done && !f.controller.cameraDisconnected(0), "No partial failure");
        require(p.media->findAsset(take.cam1AssetId)->gaps.empty(), "Recoverable events must not create a camera tail gap");
        require(p.media->findAsset(take.microphoneAssetIds.front())->logicalLength == take.logicalLength, "Original WAV remains complete");
    });
    suite.test("ASIO epoch failure stops whole take and preserves last valid block", []
    {
        Fixture f; const auto n0 = f.begin(); f.feed(); const auto stop = f.audio.acceptedEnd(); f.feed(44100); f.complete();
        require(f.controller.state() == TakeController::State::partialFailure, "ASIO fault stops take");
        require(f.controller.logicalLength() == stop - n0, "Take ends at prior callback boundary");
    });
    suite.test("Controller reuses device for consecutive takes without prior metadata or journal state", []
    {
        Fixture f;
        for (int index = 0; index < 2; ++index)
        {
            f.config.takeId = juce::Uuid();
            const auto n0 = f.begin();
            require(!f.controller.placementMetadata().ready, "New take clears previous placement cache");
            require(f.controller.placementSample() == index * 401, "Next take follows previous active clip end");
            ok(f.controller.stop(n0 + 401));
            while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); }
            f.complete(); require(f.controller.state() == TakeController::State::done, "Each take finalizes successfully");
            const auto states = f.controller.report()["states"];
            require(states.size() == 7 && states[0].toString() == "idle" && states[6].toString() == "done", "State history belongs to this take");
        }
        require(f.document.getProject().media->takes.size() == 2, "Both completed takes retained");
        JournalReplay replay; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), replay));
        int starts = 0, stops = 0, finals = 0;
        for (const auto& r : replay.records)
        { starts += r.kind == JournalKind::TakeStarted; stops += r.kind == JournalKind::TakeStopped; finals += r.kind == JournalKind::TakeFinalized; }
        require(starts == 2 && stops == 2 && finals == 2, "Independent lifecycle commits for both takes");
    });
    return suite.result("TakeControllerTests");
}
