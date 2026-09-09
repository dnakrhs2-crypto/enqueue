#include "TestSupport.h"
#include "record/DubbingController.h"
#include "playback/ImportedAudioCache.h"
#include "playback/TimelineTransport.h"
#include <chrono>
#include <thread>

using namespace gocue::recorder;
using recorder_test::require;
namespace
{
void ok(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
template<class F> void until(F f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!f()) { require(std::chrono::steady_clock::now() < deadline, "Dubbing worker timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
struct VideoState
{
    juce::MemoryBlock aac;
    Sample origin = -1, length = 0, lastMapped100ns = 0;
    std::atomic<bool> failed{false};
    juce::String mappingError;
};
class VideoDouble final : public ITakeVideoStream
{
public:
    VideoDouble(std::shared_ptr<VideoState> s, std::unique_ptr<CameraTimeMapper> m, std::atomic<unsigned>& p)
        : state(std::move(s)), mapper(std::move(m)), prepared(p)
    { state->aac.reset(); state->origin = -1; state->length = state->lastMapped100ns = 0; state->failed = false; state->mappingError.clear(); }
    void prepare(const juce::File& file, NvencProfile, Rational, const AVCodecContext& audio) override
    { require(audio.codec_id == AV_CODEC_ID_AAC, "Production AAC reference context"); output = file; ++prepared; }
    void startAt(ClockMapping, Sample origin, unsigned, std::function<Sample()>) override { state->origin = origin; }
    void offer(const VideoSurface& f) noexcept override
    { if (state->origin >= 0) try { state->lastMapped100ns = mapper->map(f.stamp); } catch (const std::exception& e) { state->failed = true; state->mappingError = e.what(); } }
    void audioPacket(const AVPacket& p) override { state->aac.append(p.data,size_t(p.size)); }
    bool ready() const noexcept override { return true; }
    void sourceFailed(Sample s) noexcept override { state->failed = true; state->length = s; }
    void endAt(Sample s) noexcept override { if (!state->failed) state->length = s; }
    void audioDone() noexcept override {}
    void finish() override { require(output.replaceWithText("Dubbing lifecycle fixture, not encoded video"), "Write explicit video double"); }
    bool failed() const noexcept override { return state->failed; }
    Sample availableSamples() const noexcept override { return state->length; }
    bool thumbnailReady() const noexcept override { return false; }
    juce::var report() const override { auto v = jsonObject(); jsonSet(v,"source","test double; no camera/NVENC opened"); jsonSet(v,"mappingError",state->mappingError); return v; }
private:
    std::shared_ptr<VideoState> state; std::unique_ptr<CameraTimeMapper> mapper; juce::File output; std::atomic<unsigned>& prepared;
};
struct Stall { std::atomic<bool> paused{false}, entered{false}; };
class StallProvider final : public IPlaybackBlockProvider
{
public:
    StallProvider(std::unique_ptr<IPlaybackBlockProvider> p, std::shared_ptr<Stall> s) : source(std::move(p)), stall(std::move(s)) {}
    void render(float* p, unsigned n, Sample first, unsigned Fs) override
    {
        if (stall->paused.load()) { stall->entered = true; while (stall->paused.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        source->render(p,n,first,Fs);
    }
private:
    std::unique_ptr<IPlaybackBlockProvider> source; std::shared_ptr<Stall> stall;
};
struct Fixture
{
    RecorderDocument document; RecorderAudioEngine audio;
    PlaybackPcmQueue idleQueue{8000,80}; TimelineTransport transport{8000,qpcFrequency(),idleQueue,32000};
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("DubbingTests-" + newId());
    DubbingController::Config config;
    std::array<std::shared_ptr<VideoState>,2> videos{std::make_shared<VideoState>(),std::make_shared<VideoState>()};
    std::atomic<unsigned> preparedVideos{0};
    std::shared_ptr<Stall> stall = std::make_shared<Stall>();
    std::unique_ptr<DubbingController> controller;
    Sample position = 0; std::int64_t qpc = qpcNow(); std::uint64_t sequence = 0;
    std::vector<float> outputL, outputR;
    Id importedAsset, importedClip;
    Fixture(bool mic = false, int channels = 2, unsigned cameraCount = 1)
    {
        ok(directory.createDirectory()); const auto source = directory.getChildFile("input.wav");
        juce::WavAudioFormat wav; auto stream = source.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.release(),8000,unsigned(channels),24,{},0)); require(writer != nullptr,"Fixture WAV writer");
        juce::AudioBuffer<float> b(channels,32000);
        for (int i = 0; i < b.getNumSamples(); ++i) { b.setSample(0,i,float(1000 + i % 101) / 8192); if (channels == 2) b.setSample(1,i,float(-2000 - i % 127) / 8192); }
        require(writer->writeFromAudioSampleBuffer(b,0,b.getNumSamples()),"Write known reference samples"); writer.reset();
        document.newProject("Dubbing",8000,{60,1}); AudioImportControl control; std::unique_ptr<PreparedAudioImport> imported;
        ok(AudioImport::prepare({source,directory,document.getProject().projectId,8000,0},control,imported));
        config.audioTrackId = imported->track().trackId; importedAsset = imported->asset().assetId; importedClip = imported->clip().clipId;
        ok(commitImportedAudio(document,*imported,control));
        std::array<int,8> map{-1,-1,-1,-1,-1,-1,-1,-1}; if (mic) map[5] = 2;
        ok(audio.setInputMap(map)); OutputMapping outputs; outputs.left = 0; outputs.right = 1; ok(audio.setOutputMap(outputs));
        ok(audio.openSynthetic(8000,80,8,2)); if (mic) ok(audio.arm(5,true));
        config.projectDirectory = directory; config.Pstart = 137; config.spanSamples = 1600; config.synthetic = true; config.recordMicrophones = mic;
        for (unsigned i = 0; i < cameraCount; ++i)
        {
            DubbingController::Camera c; c.mode.width = 1920; c.mode.height = 1080; c.mode.fps = {30,1};
            c.calibration.outputResidualLatencySamples = 23; c.calibration.inputResidualLatencySamples = 11; config.cameras.push_back(c);
        }
        controller = std::make_unique<DubbingController>(document,audio,&transport,
            [this](unsigned i,std::unique_ptr<CameraTimeMapper> m) { return std::make_unique<VideoDouble>(videos[i],std::move(m),preparedVideos); },
            [this,count = 0](const RecorderProject& p,const juce::File& f,const Id& id) mutable -> std::unique_ptr<IPlaybackBlockProvider>
            {
                auto source = DubbingController::prepareReferenceAudio(p,f,id);
                if ((count++ % 2) == 0) return std::make_unique<StallProvider>(std::move(source),stall);
                return source;
            });
        for (int i = 0; i < 4; ++i) feed(); until([&] { return audio.clockReady() && audio.masterClock().snapshot().has_value(); });
    }
    ~Fixture()
    {
        stall->paused = false;
        if (controller && controller->locked()) { controller->abort(); try { complete(); } catch (...) {} }
        controller.reset();
    }
    void feed(bool reset = false, bool camera = true)
    {
        std::array<std::uint8_t,240> native{}; std::array<float,80> l{},r{},in{};
        for (unsigned i = 0; i < 80; ++i) WavTrackWriter::packPcm24(123456 + int(position + i),native.data() + i * 3);
        NativeInputView view{native.data(),0,2,nativeFormatForAsio(17)};
        const float* inputs[]{in.data()}; float* outputs[]{l.data(),r.data()};
        BlockStamp stamp{}; stamp.sequence = sequence++; stamp.samplePosition = position; stamp.sampleRate = 8000; stamp.numSamples = 80;
        stamp.flags = samplePositionValid | latenciesValid; stamp.callbackQpc = qpc + rescaleRound(position,qpcFrequency(),8000); stamp.resets = reset ? 1 : 0;
        audio.processBlock(stamp,config.recordMicrophones ? &view : nullptr,config.recordMicrophones ? 1 : 0,inputs,outputs,2);
        outputL.insert(outputL.end(),l.begin(),l.end()); outputR.insert(outputR.end(),r.begin(),r.end());
        if (camera && controller)
        {
            VideoSurface f; f.stamp.frame = sequence; f.stamp.generation = 1; f.stamp.callback = stamp.callbackQpc;
            f.stamp.pts100ns = rescaleRound(position,10000000,8000); f.stamp.hasDeviceTimestamp = f.stamp.deviceTimestampValid = true;
            f.stamp.deviceTimestamp100ns = std::uint64_t(rescaleRound(stamp.callbackQpc,10000000,qpcFrequency()));
            for (unsigned i = 0; i < config.cameras.size(); ++i) controller->offer(i,f);
        }
        position += 80;
    }
    void arm()
    {
        ok(controller->prepare(config));
        until([&]
        {
            feed(); controller->tick();
            if (controller->state() == DubbingController::State::partialFailure) throw std::runtime_error(controller->error().toStdString());
            return controller->state() == DubbingController::State::armed;
        });
    }
    void begin()
    {
        arm(); ok(controller->start(position + 161)); until([&] { return audio.startCommitted(); });
        while (audio.startSample() < 0) { feed(); controller->tick(); }
        require(controller->state() == DubbingController::State::recording,"Native callback adopts O0");
    }
    void complete()
    {
        until([&] { controller->tick(); return !controller->locked(); });
    }
    void finishNormally()
    {
        const auto stop = controller->placement().O0 + controller->placement().spanSamples;
        while (position < stop + 160 && controller->state() != DubbingController::State::finalizing)
        { feed(); controller->tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        complete(); if (controller->state() != DubbingController::State::done) throw std::runtime_error(juce::JSON::toString(controller->report()).toStdString());
    }
};
}
int runDubbingPlacementTests()
{
    recorder_test::Suite tests;
    tests.test("Off-grid Pstart, output latency sign and no double input correction", []
    {
        const OutputBufferStamp b{10000,137,256,1};
        require(outputOriginSample(137,480,b,37) == 10517,"Positive output latency moves O0 later");
        require(projectVideoSample(137,11000,10517) == 620,"Pvideo subtracts O0 once");
        require(projectInputSample(137,10517,10517) == 137,"Already corrected input needs no further latency");
        require(outputOriginSample(137,480,b,-37) == 10443,"Negative output residual sign");
    });
    tests.test("Preparation waits for the master clock fit before arming", []
    {
        Fixture f; ok(f.controller->prepare(f.config));
        // Supply enough camera observations but less than the master's minimum
        // 250ms fit window. Let file/AAC preparation settle without advancing ASIO.
        until([&] { return f.preparedVideos.load() == 1; });
        for (int i = 0; i < 16; ++i) f.feed();
        for (int i = 0; i < 40; ++i) { f.controller->tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(f.controller->state() == DubbingController::State::preparing, "Camera/PCM readiness alone must not arm dubbing");
        until([&] { f.feed(); f.controller->tick(); return f.controller->state() == DubbingController::State::armed; });
        ok(f.controller->start(f.position + 161)); until([&] { return f.audio.startCommitted(); });
        f.finishNormally();
    });
    tests.test("Mic-off callback clock, scheduled selected stereo PCM and two-camera anchor", []
    {
        Fixture f(false,2,2); const auto original = f.document.getProject().media->findAsset(f.importedAsset)->contentIdentity;
        f.begin(); const auto placement = f.controller->placement();
        require(placement.Pstart == 137 && placement.O0 == placement.outputSubmissionSample + 23,"O0 residual applied once with fixed Pstart");
        f.finishNormally(); const auto& p = f.document.getProject();
        require(f.audio.armedMicrophones().empty() && f.audio.currentSample() > placement.O0,"Clock continues without inputs");
        require(p.media->takes.back().microphoneAssetIds.empty(),"Mic-off creates no WAV assets");
        require(p.media->findAsset(f.importedAsset)->contentIdentity == original,"Imported original preserved");
        require(p.findClip(f.importedClip)->timelineStartSample == 0 && p.findClip(f.importedClip)->takeStackId.isEmpty(),"Completed audio untouched/outside link");
        const auto at = size_t(placement.outputSubmissionSample);
        for (int i = 0; i < 32; ++i)
        {
            require(std::abs(f.outputL[at + i] - float(1000 + (137 + i) % 101) / 8192) < 0.000001f,"Left begins at Pstart source sample");
            require(std::abs(f.outputR[at + i] - float(-2000 - (137 + i) % 127) / 8192) < 0.000001f,"Stereo right preserved");
        }
        require(f.outputL[at - 1] == 0 && f.videos[0]->origin == placement.O0 && f.videos[1]->origin == placement.O0,"Silent preroll and common file origin");
        require(f.videos[0]->aac.getSize() && f.videos[0]->aac == f.videos[1]->aac,"Identical reference AAC packets to both cameras");
        require(f.controller->placement().recordedSamples == 1600 && p.activeTimelineEnd() == 32000,"Stop inserts at Pstart, not timeline end");
        require(f.controller->calibrationOffsetReport()["status"].toString() == "PASS", "Changed profiles reproject the same captured stamp with exact output shift");
        JournalReplay replay; ok(RecordingJournal::replay(f.directory.getChildFile("journal"),replay)); bool started = false;
        for (const auto& r : replay.records) if (r.kind == JournalKind::TakeStarted)
        { started = true; require(bool(r.payload["usesOutputOrigin"]) && r.payload["placementMode"].toString() == "dub", "Journal carries dubbing origin contract"); }
        require(started,"Durable TakeStarted present");
    });
    tests.test("Optional native WAV uses corrected O0 range, never selected output PCM", []
    {
        Fixture f(true); f.begin(); const auto origin = f.controller->placement().O0; f.finishNormally();
        const auto& take = f.document.getProject().media->takes.back(); require(take.microphoneAssetIds.size() == 1,"One sparse armed mic recorded");
        const auto* a = f.document.getProject().media->findAsset(take.microphoneAssetIds[0]); require(a->logicalLength == 1600,"WAV range length exact");
        auto reader = AudioImport::openReader(f.directory.getChildFile(a->chunks[0].relativePath)); require(reader && reader->lengthInSamples == 1600,"Reopen original WAV");
        std::array<float,1600> samples{}; float* ptr[]{samples.data()}; require(reader->read(ptr,1,0,1600),"Read original WAV samples");
        for (int i = 0; i < 1600; ++i)
            require(std::abs(samples[size_t(i)] - float(123456 + origin + 11 + i) / 8388608) < 0.0000002f,"Input residual corrected exactly once; native source sample oracle");
        const auto report = f.controller->report(); require(Sample(report["audio"]["N0"]) == origin && Sample(report["audio"]["Nstop"]) == origin + 1600,"Reported corrected [O0,Ostop)");
    });
    tests.test("Selected mono reference duplicates L/R, ignores other tracks and mic arm", []
    {
        Fixture f(true,1);
        ok(f.document.performEdit("Other imported audio lane",[&](EditState& e)
        {
            Track t; t.kind = TrackKind::importAudio; t.name = "Other audio"; auto c = *f.document.getProject().findClip(f.importedClip);
            c.clipId = newId(); c.trackId = t.trackId; c.sourceIn = 31; c.lengthSamples -= 31; t.clips.edit().push_back(c); e.tracks.push_back(t);
        }));
        auto source = DubbingController::prepareReferenceAudio(f.document.getProject(),f.directory,f.config.audioTrackId);
        std::array<float,64> data{}; source->render(data.data(),32,137,8000);
        for (int i = 0; i < 32; ++i) require(data[i * 2] == data[i * 2 + 1] && std::abs(data[i * 2] - float(1000 + (137 + i) % 101) / 8192) < 0.000001f,"Mono selected audio, no mic mix");
        recorder_test::rejects([&] { DubbingController::prepareReferenceAudio(f.document.getProject(),f.directory,newId()); });
    });
    tests.test("Selected reference follows imported clip cuts and timeline gaps", []
    {
        Fixture f;
        ok(f.document.performEdit("Cut reference",{},[&](const RecorderProject& p) { return ClipEdits::remove(p,{f.importedClip},{801,99}); }));
        auto source = DubbingController::prepareReferenceAudio(f.document.getProject(),f.directory,f.config.audioTrackId);
        std::array<float,1024> data{}; source->render(data.data(),512,700,8000);
        for (int at = 801; at < 900; ++at) require(data[(at - 700) * 2] == 0 && data[(at - 700) * 2 + 1] == 0,"Cut gap is silent");
        require(std::abs(data[600] - float(1000 + 1000 % 101) / 8192) < 0.000001f,"Post-cut source resumes at absolute original sample");
    });
    tests.test("Reference AAC bytes are identical with microphone recording off/on", []
    {
        Fixture off; off.begin(); off.finishNormally();
        Fixture on(true); on.begin(); on.finishNormally();
        require(off.videos[0]->aac.getSize() && off.videos[0]->aac == on.videos[0]->aac, "Mic input never sums into the completed-audio reference encoder");
    });
    tests.test("Controller retake preserves first range, early stop tail and automatic stop", []
    {
        Fixture f; f.begin(); f.finishNormally(); const auto stack = f.controller->placement().stackId;
        ok(f.controller->retake()); until([&] { f.feed(); f.controller->tick(); return f.controller->state() == DubbingController::State::armed; });
        ok(f.controller->start(f.position + 161)); until([&] { return f.audio.startCommitted(); });
        const auto origin = f.controller->placement().O0; ok(f.controller->stop(origin + 803));
        while (f.position < origin + 963 && f.controller->state() != DubbingController::State::finalizing) { f.feed(); f.controller->tick(); }
        f.complete(); require(f.controller->state() == DubbingController::State::done,"Early retake completes");
        require(f.controller->placement().stackId == stack && f.controller->placement().spanSamples == 1600 && f.controller->placement().recordedSamples == 803,"Early stop retains first range");
        const auto* s = TakeStackEdits::find(f.document.getProject(),stack); require(s->versions.size() == 2,"New version on top");
    });
    return tests.result("dubbing-clock-placement");
}

// Shared lifecycle fixture for the separate failure suite; no additional test main.
void runDubbingFailureScenario(int scenario)
{
    Fixture f(false,2,scenario == 5 ? 2 : 1);
    if (scenario == 0)
    {
        f.config.spanSamples = 24000; f.begin(); f.stall->paused = true;
        // Drain enough of the bounded producer queue for the provider to enter
        // the injected disk stall, then exhaust the remaining real PCM blocks.
        for (int i = 0; i < 100 && !f.stall->entered; ++i) { f.feed(); f.controller->tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        for (int i = 0; i < 100 && f.controller->failure() == DubbingController::Failure::none; ++i) { f.feed(); f.controller->tick(); }
        require(f.controller->failure() == DubbingController::Failure::playbackUnderrun,"Real queue underrun is terminal during dubbing");
        f.stall->paused = false; f.complete();
        require(f.controller->state() == DubbingController::State::partialFailure,"Underrun not disguised as normal completion");
        require(f.controller->placement().recordedSamples > 0 && f.document.getProject().media->takes.back().state == TakeState::partial,"Partial capture prefix preserved");
        require(Sample(f.controller->report()["playbackUnderruns"]) == 1,"Underrun reported once");
    }
    else if (scenario == 1)
    {
        f.begin(); for (int i = 0; i < 3; ++i) f.feed(); const auto accepted = f.audio.acceptedEnd();
        f.feed(true,false); f.controller->tick(); f.complete();
        require(f.controller->failure() == DubbingController::Failure::asioReset && f.controller->placement().Ostop == accepted,"Reset preserves last callback-confirmed end");
        require(f.document.getProject().media->takes.back().state == TakeState::partial,"Reset take is partial");
        RecorderDocument opened; ok(opened.openCheckpoint(f.directory.getChildFile("project.recorder")));
        require(opened.getProject().media->takes.back().state == TakeState::partial,"Partial state persists to disk");
    }
    else if (scenario == 2)
    {
        f.begin(); const auto revision = f.document.getProject().editRevision; bool ran = false;
        require(f.document.performEdit("locked", [&](EditState&) { ran = true; }).failed(),"Legacy edit locked");
        require(f.document.performEdit("locked",{},[&](const RecorderProject& p) { ran = true; return ClipEditResult(p); }).failed(),"Pure edit overload locked");
        require(!ran && f.document.getProject().editRevision == revision && f.document.undo().failed(),"No edit callback or undo during dubbing");
        recorder_test::rejects([&] { f.transport.seek(900); }); recorder_test::rejects([&] { f.transport.scrub(900,true,qpcNow()); });
        recorder_test::rejects([&] { f.transport.stop(); }); recorder_test::rejects([&] { f.transport.play(); });
        OutputMapping outputs; outputs.left = 1; require(f.audio.setOutputMap(outputs).failed() && f.audio.closeDevice().failed() && f.audio.arm(0,false).failed(),"Device, output, arm locked");
        f.finishNormally(); require(!f.transport.isDubbingLocked() && !f.document.isRecordingStructureLocked(),"Locks released after finalization");
    }
    else if (scenario == 3)
    {
        f.config.audioTrackId = newId(); require(f.controller->prepare(f.config).failed(),"Missing reference is rejected before locks");
        require(!f.document.isRecordingStructureLocked() && !f.controller->locked(),"Preparation rejection is atomic");
    }
    else if (scenario == 4)
    {
        ok(f.controller->prepare(f.config)); f.controller->abort();
        f.complete(); require(f.controller->state() == DubbingController::State::partialFailure, "Cancellation during preparation terminates");
        require(f.document.getProject().media->takes.empty(), "Cancelled empty take is not placed");
        require(f.audio.closeDevice().wasOk(), "Cancelled preparation drains and releases audio session");
    }
    else if (scenario == 5)
    {
        f.begin(); f.controller->cameraFailed(0);
        const auto stop = f.controller->placement().O0 + f.controller->placement().spanSamples;
        while (f.position < stop + 160 && f.controller->state() != DubbingController::State::finalizing)
        { f.feed(); f.controller->tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        f.complete(); const auto& p = f.document.getProject(); const auto& take = p.media->takes.back();
        require(f.controller->state() == DubbingController::State::partialFailure && f.controller->placement().recordedSamples == 1600, "One failed camera does not stop the common recording range");
        require(!p.media->findAsset(take.cam1AssetId)->gaps.empty() && p.media->findAsset(take.cam2AssetId)->gaps.empty(), "Failed camera tail is a gap; other camera remains complete");
    }
}
