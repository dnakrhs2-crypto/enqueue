#include "TestSupport.h"
#include "record/TakeController.h"
#include "app/RecorderSession.h"
#include "media/MediaIndex.h"
#include "sync/CameraClockMapper.h"
#include <chrono>
#include <limits>
#include <thread>

using namespace gocue::recorder;
using recorder_test::require;
namespace gocue::recorder::exception_test { extern thread_local std::function<void(const char*)> beforeTakeWorker; }
namespace gocue::recorder
{
struct RecordingPlacementTestAccess
{
    static void configure(RecorderSession& session, TakeController::Config& config) { session.configurePlacement(config); }
    // Marker entry/name UI belongs to another session; test only atomic placement metadata.
    static void marker(RecorderSession& session, Marker marker) { session.recordedMarkers.push_back(std::move(marker)); }
};
}
namespace
{
struct FailTakeLaunch
{
    explicit FailTakeLaunch(const char* phase)
    { exception_test::beforeTakeWorker = [phase](const char* current) { if (std::string(current) == phase) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "injected thread creation failure"); }; }
    ~FailTakeLaunch() { exception_test::beforeTakeWorker = {}; }
};
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
    juce::var muxReport;
    std::function<void(const juce::File&)> afterFinish;
    Sample origin = -1, cameraLatency = 0;
    unsigned rate = 0;
    const ClockMapper* master = nullptr;
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
    void configureClock(const ClockMapper& clock, Sample latency) override { state->master = &clock; state->cameraLatency = latency; }
    void startAt(ClockMapping clock, std::int64_t origin, unsigned rate, std::function<std::int64_t()>) override
    { require(clock.valid && origin >= 0, "Video receives valid N0/clock"); state->origin = origin; state->rate = rate; }
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
        if (state->afterFinish) state->afterFinish(output);
    }
    bool failed() const noexcept override { return state->failed.load(); }
    std::int64_t availableSamples() const noexcept override { return state->available.load(); }
    bool thumbnailReady() const noexcept override { return false; }
    juce::var report() const override
    {
        auto v = jsonObject(); jsonSet(v, "source", "lifecycle test double; not encoded media");
        if (state->muxReport.isObject()) jsonSet(v, "mux", state->muxReport);
        return v;
    }
private:
    std::shared_ptr<DoubleState> state;
    juce::File output;
};
struct Fixture
{
    RecorderDocument document;
    std::shared_ptr<DoubleState> video = std::make_shared<DoubleState>();
    RecorderSession session{document, [this] { return std::make_unique<VideoDouble>(video); }};
    RecorderAudioEngine& audio = session.audioEngine();
    TakeController& controller = session.takeController();
    TakeController::Config config;
    std::int64_t position = 0, qpc = qpcNow(); std::uint64_t sequence = 0;
    unsigned microphones; bool stereo;
    bool saveOutput = false;
    std::vector<float> outputL, outputR;
    float monitorInput = 0;
    std::uint64_t resets = 0, xruns = 0, latencyChanges = 0;
    unsigned rate, block;
    Fixture(unsigned mics = 1, bool stereoSlot = false, int inputLatency = 0, int outputLatency = 0,
            unsigned sampleRate = 8000, unsigned blockFrames = 80)
        : microphones(mics), stereo(stereoSlot), rate(sampleRate), block(blockFrames)
    {
        std::array<int, 8> map{-1,-1,-1,-1,-1,-1,-1,-1};
        if (mics) map[5] = 2; // sparse logical mic06, physical input3
        if (mics > 1) map[7] = 5;
        std::array<bool, 8> slots{}; slots[5] = stereo;
        ok(audio.setInputMap(map, slots)); OutputMapping outputs; outputs.left = 0; outputs.right = 1; ok(audio.setOutputMap(outputs));
        ok(audio.openSynthetic(rate, block, 8, 2, inputLatency, outputLatency)); if (mics) ok(audio.arm(5, true));
        if (mics > 1) ok(audio.arm(7, true));
        document.newProject("Take lifecycle", rate, {60,1});
        config.projectDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("TakeControllerTests-" + juce::Uuid().toString());
        config.synthetic = true; config.projectFps = 60; config.cameraMode.width = 1920; config.cameraMode.height = 1080;
        config.cameraMode.fps = {30,1};
        config.outputMapping = {0, 1};
        for (int i = 0; i < 4; ++i) feed(); until([&] { return audio.clockReady(); });
    }
    ~Fixture() { video->release = true; }
    void feed(unsigned Fs = 0)
    {
        std::vector<std::uint8_t> pcm(block * 3);
        std::vector<float> left(block), right(block), input(block, monitorInput);
        for (unsigned i = 0; i < block; ++i) WavTrackWriter::packPcm24(123456 + int(position + i), pcm.data() + i * 3);
        auto rightPcm = pcm; for (unsigned i = 0; i < block; ++i) WavTrackWriter::packPcm24(-345678 - int(position + i), rightPcm.data() + i * 3);
        NativeInputView views[]{{pcm.data(), 0, 2, nativeFormatForAsio(17)}, {rightPcm.data(), 1, microphones > 1 ? 5 : 3, nativeFormatForAsio(17)}};
        const float* inputs[] = {input.data(), input.data()}; float* outputs[] = {left.data(), right.data()};
        BlockStamp stamp{}; stamp.flags = samplePositionValid; stamp.sequence = sequence++; stamp.samplePosition = position;
        stamp.sampleRate = Fs ? Fs : rate; stamp.numSamples = block; stamp.callbackQpc = qpc + rescaleRound(position, qpcFrequency(), rate);
        stamp.resets = resets; stamp.xruns = xruns; stamp.latencyChanges = latencyChanges;
        stamp.flags |= latenciesValid; stamp.inputLatencySamples = audio.deviceInfo().inputLatency; stamp.outputLatencySamples = audio.deviceInfo().outputLatency;
        audio.processBlock(stamp, microphones ? views : nullptr, microphones ? (stereo || microphones > 1 ? 2 : 1) : 0, inputs, outputs, 2); position += block;
        if (saveOutput) { outputL.insert(outputL.end(), left.begin(), left.end()); outputR.insert(outputR.end(), right.begin(), right.end()); }
    }
    void arm()
    {
        ok(controller.prepare(config)); require(controller.state() == TakeController::State::preparing, "Preparation is asynchronous");
        until([&] { controller.tick(); require(controller.state() != TakeController::State::partialFailure, controller.error().toRawUTF8()); return controller.state() == TakeController::State::armed; });
    }
    std::int64_t begin()
    {
        arm(); Sample origin = position + 81;
        if (config.listeningAudio)
        {
            origin += audio.deviceInfo().outputLatency;
            for (const auto& profile : config.calibration) if (profile) { origin += profile->outputResidualLatencySamples; break; }
        }
        ok(controller.start(origin));
        until([&] { return audio.startCommitted(); });
        while (audio.startSample() < 0) { feed(); controller.tick(); }
        require(controller.state() == TakeController::State::recording, "Callback acknowledgement enters recording"); return origin;
    }
    void complete()
    {
        until([&] { controller.tick(); return controller.state() == TakeController::State::done || controller.state() == TakeController::State::partialFailure; });
    }
};
void requireRanges(const std::vector<SampleRange>& ranges, Sample available)
{
    require(ranges.size() == (available ? 1u : 0u), "Unexpected available range count");
    if (available) require(ranges[0].start == 0 && ranges[0].length == available, "Available range exceeds confirmed media");
}
void verifyFinalizedAssets(Fixture& f, Sample audioSamples = -1, Sample videoSamples = -1)
{
    const auto snapshot = f.document.snapshot(); const auto& project = *snapshot;
    const auto& take = project.media->takes.back();
    const auto* camera = project.media->findAsset(take.cam1AssetId);
    require(camera && camera->relativePath.endsWith("/cam1.mp4") && camera->mediaGeneration >= 1, "Final camera path/generation was not published");
    require(f.config.projectDirectory.getChildFile(camera->relativePath).existsAsFile(), "Published final camera does not exist");
    require(!f.config.projectDirectory.getChildFile(camera->relativePath).getSiblingFile("cam1.recording.mp4").exists(), "Temporary camera survived rename");
    requireRanges(camera->availableRanges, videoSamples < 0 ? take.logicalLength : videoSamples);
    const auto wavReport = f.audio.telemetry()["wav"];
    if (audioSamples < 0) audioSamples = std::min({take.logicalLength, Sample(wavReport["writtenSamplesPerMic"]), Sample(wavReport["mediaDurableSamplesPerMic"])});
    for (const auto& id : take.microphoneAssetIds)
    {
        const auto* mic = project.media->findAsset(id);
        require(mic && mic->mediaGeneration >= 1 && mic->logicalLength == take.logicalLength, "Final WAV identity/generation/length missing");
        requireRanges(mic->availableRanges, audioSamples);
        require(mic->gaps.size() == (audioSamples < take.logicalLength ? 1u : 0u), "WAV loss was not represented as a gap");
        if (!mic->gaps.empty()) require(mic->gaps[0].start == audioSamples && mic->gaps[0].length == take.logicalLength - audioSamples, "WAV gap does not match durable tail");
        const auto indexed = MediaIndex::recordedAudio(*mic, f.config.projectDirectory, project.Fs, newId());
        Sample indexedSamples = 0;
        for (const auto& chunk : indexed->chunks) { require(chunk.file.existsAsFile(), "Indexed WAV chunk missing"); indexedSamples += chunk.validSamples; }
        require(indexed->length == take.logicalLength && indexedSamples == audioSamples, "WAV index overstates actual media");
    }
    const auto file = f.config.projectDirectory.getChildFile("project.recorder");
    ok(f.document.saveCheckpoint(file)); RecorderProject loaded; ok(RecorderSerializer::readCheckpoint(file, loaded));
    require(loaded.validate().wasOk() && loaded.media->findTake(take.takeId)->state == take.state, "Saved final take state changed");
    require(RecorderSerializer::fingerprint(RecorderSerializer::toJson(loaded)) == RecorderSerializer::fingerprint(RecorderSerializer::toJson(project)), "Saved metadata differs from confirmed document");
    require(loaded.media->findAsset(take.cam1AssetId)->relativePath == camera->relativePath, "Reloaded project retained the recording MP4 path");
    for (const auto& id : take.microphoneAssetIds)
    {
        const auto* mic = loaded.media->findAsset(id); requireRanges(mic->availableRanges, audioSamples);
        require(MediaIndex::recordedAudio(*mic, file.getParentDirectory(), loaded.Fs, newId())->length == take.logicalLength, "Saved WAV metadata cannot be indexed");
    }
}
struct FailWavTailFlush final : FileIoFaultAdapter
{
    unsigned channels = 1;
    std::atomic<unsigned> failures{0};
    juce::Result beforeIo(FileIoOperation op, const juce::File& file, std::uint64_t offset, std::size_t) override
    {
        if (file.hasFileExtension("wav") && op == FileIoOperation::flushData && offset > 44 + 8000 * channels * 3)
        { ++failures; return juce::Result::fail("injected WAV tail flush failure"); }
        return juce::Result::ok();
    }
};
}
#include "TimelineRecordingTests.h"
int runTakeControllerTests()
{
    recorder_test::Suite suite;
    addTimelineRecordingTests(suite);
    suite.test("Timeline cursor reserves off-grid placement, journal Pstart and one undo including markers", []
    {
        Fixture f; f.session.enterTimeline(true); constexpr Sample placement = 15 * 8000 + 37, length = 1601;
        f.session.scrub(placement, true); require(f.session.playhead() == placement, "Empty timeline accepts a later cursor");
        UserSettings names; names.microphoneNames[5] = "Recorded mic six"; f.session.updateMicrophoneSettings(names);
        RecordingPlacementTestAccess::configure(f.session, f.config);
        const auto before = RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(f.document.getProject()));
        const auto n0 = f.begin(); require(f.controller.placementSample() == placement, "Controller reserves session cursor");
        JournalReplay started; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), started));
        require(started.records.front().kind == JournalKind::TakeStarted && Sample(started.records.front().payload["Pstart"]) == placement,
            "Requested placement is durable before placement or finalization");
        Marker marker; marker.sample = placement + f.session.elapsed(); marker.name = "Recorded marker";
        RecordingPlacementTestAccess::marker(f.session, std::move(marker));
        require(f.document.getProject().tracks.empty() && f.document.getProject().markers.empty(), "Recording has not changed the edit structure");
        f.video->release = false; ok(f.controller.stop(n0 + length));
        until([&] { f.feed(); f.session.tick(); return f.controller.placementMetadata().ready; });
        require(f.session.playhead() == placement + length, "Placement-ready moves cursor to recorded end before finalizer drain");
        require(f.document.getHistory().undoDepth() == 1 && f.document.getProject().markers.size() == 1, "Placement and recorded marker share one history entry");
        require(f.document.getProject().tracks.back().name == "Recorded mic six", "Microphone name belongs to the same edit");
        f.video->release = true; f.complete(); ok(f.document.undo());
        require(RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(f.document.getProject())) == before, "One undo removes placement, names and marker together");
        const auto id = f.document.getProject().media->takes.back().takeId; ok(f.document.placeTake(id));
        require(f.document.getProject().tracks.back().microphoneIndex == 5
            && f.document.getProject().tracks.back().clips.items()[0].timelineStartSample == placement, "Registered take reinsertion retains sparse microphone and requested position");
    });
    suite.test("Recording tab appends at active end despite hidden cursor; project change resets cursor to end", []
    {
        Fixture f; auto n0 = f.begin(); ok(f.controller.stop(n0 + 401));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        f.session.projectChanged(); require(f.session.playhead() == 401, "Project change starts cursor at active end");
        f.session.enterTimeline(false); f.session.scrub(100, true);
        f.config.takeId = juce::Uuid(); RecordingPlacementTestAccess::configure(f.session, f.config);
        require(f.config.placementSample == 401, "Recording tab ignores hidden cursor inside an existing clip");
        n0 = f.begin(); ok(f.controller.stop(n0 + 211));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        require(f.document.getProject().tracks[0].clips.items().size() == 2 && f.document.getProject().activeTimelineEnd() == 612, "Recording tab retained previous take and appended");
        f.session.projectChanged(); require(f.session.playhead() == 612, "Project reload uses new active end");
        f.session.scrub(15 * 8000, true); f.session.play();
        require(!f.session.playing() && f.session.playhead() == 15 * 8000 && f.session.error.isEmpty(), "Play beyond end completes without error or cursor loss");
        f.session.scrub((std::numeric_limits<Sample>::max)(), true);
        require(f.session.playhead() == Sample{8000} * 24 * 60 * 60, "Scrub is bounded to 24 hours");
        f.session.scrub(-1, true); require(f.session.playhead() == 0, "Scrub clamps negative positions");
        f.session.scrub(100, true); f.session.goToStart(); require(f.session.playhead() == 0, "Go to start is unchanged");
    });
    suite.test("Controller reserves overwrite position while capture leaves the existing structure locked", []
    {
        Fixture f; auto n0 = f.begin(); ok(f.controller.stop(n0 + 1601));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        const auto before = RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(f.document.getProject()));
        const auto depth = f.document.getHistory().undoDepth();
        f.config.takeId = juce::Uuid(); f.config.placementSample = 401; n0 = f.begin();
        require(f.document.isRecordingStructureLocked() && RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(f.document.getProject())) == before,
            "Preparing/recording never carves the old clips");
        ok(f.controller.stop(n0 + 301)); while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        require(f.document.getProject().tracks[0].clips.items().size() == 3 && f.document.getHistory().undoDepth() == depth + 1,
            "Stop splits old clip and inserts overwrite in one edit");
        ok(f.document.undo()); require(RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(f.document.getProject())) == before,
            "Controller overwrite restores with one undo");
    });
    suite.test("Preparation launch failure restores idle and the structure lock", []
    {
        Fixture f;
        {
            FailTakeLaunch injection("prepare"); const auto result = f.controller.prepare(f.config);
            require(result.failed() && result.getErrorMessage().contains("injected thread creation failure"), "Launch exception must become Result");
            require(f.controller.state() == TakeController::State::idle && !f.document.isRecordingStructureLocked(), "Failed launch retained preparing/lock");
            require(f.controller.shutdownComplete() && !f.audio.shutdownBlocker()->load(), "Failed launch retained capture resources");
        }
        const auto start = f.begin(); ok(f.controller.stop(start + 401));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        require(f.controller.state() == TakeController::State::done, "Retry after failed launch did not finish");
    });
    for (const auto* phase : {"finalize", "save"})
        suite.test(phase == std::string("finalize") ? "Finalizer launch failure releases capture and preserves partial take" : "Checkpoint launch failure releases capture and preserves partial take", [phase]
        {
            Fixture f; const auto start = f.begin(); ok(f.controller.stop(start + 401));
            FailTakeLaunch injection(phase);
            while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
            require(f.controller.state() == TakeController::State::partialFailure, "Failed finalization stuck or falsely succeeded");
            require(!f.document.isRecordingStructureLocked() && !f.audio.shutdownBlocker()->load(), "Failed finalization retained locks");
            require(f.controller.shutdownComplete() && f.controller.error().contains("injected thread creation failure"), "Worker state/error lost");
            require(!f.document.getProject().media->takes.empty() && f.document.getProject().media->takes.back().state == TakeState::partial, "Partial take missing");
            verifyFinalizedAssets(f);
        });
    suite.test("Thrown checkpoint worker exception is collected and releases the journal", []
    {
        Fixture f; const auto start = f.begin();
        const auto manifest = f.config.projectDirectory.getChildFile("media/takes/" + f.config.takeId.toDashedString() + "/take.json");
        require(manifest.moveFileTo(manifest.getSiblingFile("take-before-failure.json")), "Preserve fixture manifest");
        require(manifest.createDirectory().wasOk(), "Inject metadata write exception");
        ok(f.controller.stop(start + 401));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        require(f.controller.state() == TakeController::State::partialFailure && f.controller.error().contains("Take worker failed"), "future.get exception did not reach failure state");
        require(!f.document.isRecordingStructureLocked() && !f.audio.shutdownBlocker()->load() && f.controller.shutdownComplete(), "Thrown worker retained lifecycle resources");
        require(f.document.isDirty() && f.document.getError().isNotEmpty(), "Thrown checkpoint was treated as saved");
        verifyFinalizedAssets(f);
    });
    for (bool stereo : {false, true})
        suite.test(stereo ? "Stereo failed cleanup publishes the durable WAV prefix" : "Mono finalization publishes the durable WAV prefix", [stereo]
        {
            FailWavTailFlush fault; fault.channels = stereo ? 2 : 1;
            Fixture f(1, stereo); f.config.faults = &fault;
            const auto start = f.begin(); ok(f.controller.stop(start + 8401));
            std::unique_ptr<FailTakeLaunch> injection;
            if (stereo) injection = std::make_unique<FailTakeLaunch>("finalize");
            while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
            require(fault.failures > 0 && f.controller.state() == TakeController::State::partialFailure, "WAV flush fault was not exercised");
            const auto report = f.audio.telemetry()["wav"];
            require(Sample(report["writtenSamplesPerMic"]) == 8401 && Sample(report["mediaDurableSamplesPerMic"]) == 8000, "Fixture must distinguish written data from durable data");
            verifyFinalizedAssets(f, 8000);
        });
    suite.test("Finalization limits WAV availability to the actual truncated chunk", []
    {
        Fixture f; bool truncated = false;
        f.video->afterFinish = [&](const juce::File& camera)
        {
            juce::FileOutputStream wav(camera.getParentDirectory().getChildFile("audio/mic06/000001.wav"));
            truncated = wav.openedOk() && wav.setPosition(44 + 97 * 3) && wav.truncate().wasOk();
        };
        const auto start = f.begin(); ok(f.controller.stop(start + 401));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        require(truncated && f.controller.state() == TakeController::State::partialFailure, "Truncated chunk did not make the take partial");
        verifyFinalizedAssets(f, 97);
    });
    suite.test("Final camera availability requires completed media, not queued frames", []
    {
        for (bool finalized : {false, true})
        {
            Fixture f; f.video->muxReport = jsonObject();
            jsonSet(f.video->muxReport, "finalized", finalized); jsonSet(f.video->muxReport, "completedFragments", 1);
            jsonSet(f.video->muxReport, "videoPackets", 1);
            const auto start = f.begin(); ok(f.controller.stop(start + 401));
            while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
            require(f.controller.state() == TakeController::State::partialFailure, "Unconfirmed camera tail remained available");
            verifyFinalizedAssets(f, 401, finalized ? rescaleRound(1, 8000, 60) : 0);
        }
    });
    suite.test("Missing WAV chunk becomes a full gap and an indexable silent source", []
    {
        Fixture f; bool removed = false;
        f.video->afterFinish = [&](const juce::File& camera)
        { removed = camera.getParentDirectory().getChildFile("audio/mic06/000001.wav").deleteFile(); };
        const auto start = f.begin(); ok(f.controller.stop(start + 401));
        while (f.audio.stopSample() < 0) { f.feed(); f.controller.tick(); } f.complete();
        require(removed && f.controller.state() == TakeController::State::partialFailure, "Missing chunk was still published as available");
        verifyFinalizedAssets(f, 0);
    });
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
    suite.test("Caller requests independent audio active end; markers excluded; zero microphones allowed", []
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
        f.config.placementSample = f.document.getProject().activeTimelineEnd();
        const auto n0 = f.begin(); require(f.controller.placementSample() == 4000, "Requested audio end, not marker, is reserved");
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
            f.config.placementSample = f.document.getProject().activeTimelineEnd();
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
