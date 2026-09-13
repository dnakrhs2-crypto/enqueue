#pragma once
// Included by TakeControllerTests after its synthetic-device/video fixture.
#include "AudioRenderFixtures.h"
#include "record/DubbingController.h"
#include "storage/RecoveryScanner.h"

namespace
{
struct ListeningLane { Id track, asset; Sample start = 0, length = 0; int seed = 0; bool stereo = false; };
std::int32_t listeningPcm(int seed, Sample at) { return 100000 + seed * 17000 + int(at % 127) * 64; }
ListeningLane listeningLane(Fixture& f, bool imported, int seed, Sample start = 0, Sample length = 4000)
{
    const auto path = imported ? "listening-" + newId() + ".wav" : "media/takes/listening-" + newId() + "/audio/mic06/000001.wav";
    const auto file = f.config.projectDirectory.getChildFile(path);
    std::vector<std::int32_t> pcm(static_cast<size_t>(length));
    for (Sample i = 0; i < length; ++i) pcm[size_t(i)] = listeningPcm(seed, i);
    recorder_audio_fixture::writePcm24(file, f.rate, pcm, 0, length, imported ? 2 : 1);
    ListeningLane lane; lane.start = start; lane.length = length; lane.seed = seed; lane.stereo = imported;
    if (imported)
    {
        AudioImportControl control; std::unique_ptr<PreparedAudioImport> prepared;
        ok(AudioImport::prepare({file, f.config.projectDirectory, f.document.getProject().projectId, f.rate, start}, control, prepared));
        lane.track = prepared->track().trackId; lane.asset = prepared->asset().assetId;
        ok(commitImportedAudio(f.document, *prepared, control));
    }
    else
    {
        MediaAsset asset; asset.kind = AssetKind::mic; asset.contentIdentity = asset.assetId; asset.mediaGeneration = 1;
        asset.originalFormat.codec = "pcm_s24le"; asset.originalFormat.sampleRate = f.rate;
        asset.originalFormat.channels = 1; asset.originalFormat.bitsPerSample = 24;
        asset.logicalLength = length; asset.availableRanges = {{0, length}}; asset.chunks = {{path, {0, length}}};
        Track track; track.kind = TrackKind::mic; track.microphoneIndex = 5;
        Clip clip; clip.trackId = track.trackId; clip.assetId = asset.assetId; clip.timelineStartSample = start; clip.lengthSamples = length;
        track.clips.edit().push_back(clip); lane.track = track.trackId; lane.asset = asset.assetId;
        Take take; take.number = 1; take.createdAt = "2026-09-12"; take.logicalLength = length; take.placementSample = start;
        take.state = TakeState::partial; take.microphoneAssetIds = {asset.assetId}; take.capture.physicalInputs = {2};
        ok(f.document.registerMedia({asset}, {take})); ok(f.document.performEdit("Existing microphone", [&](EditState& e) { e.tracks.push_back(track); }));
    }
    return lane;
}
void listenAt(Fixture& f, Sample at)
{
    f.session.enterTimeline(true); f.session.scrub(at, true);
    RecordingPlacementTestAccess::configure(f.session, f.config);
    f.saveOutput = true;
}
void finishListening(Fixture& f, Sample stop)
{
    ok(f.controller.stop(stop));
    until([&] { f.feed(); f.controller.tick(); return f.audio.stopSample() >= 0; }); f.complete();
    require(f.controller.state() == TakeController::State::done, f.controller.error().toRawUTF8());
}
CalibrationProfile listeningCalibration(const Fixture& f, Sample in, Sample out, Sample camera = 0)
{
    CalibrationProfile p;
    p.key = calibrationKey("synthetic-camera", f.config.cameraMode, f.config.exposure[0],
        f.audio.deviceInfo().name.toStdString(), f.rate, f.block, f.config.outputMapping, f.audio.calibrationInputMapping());
    p.inputResidualLatencySamples = in; p.outputResidualLatencySamples = out; p.cameraResidualLatency100ns = camera;
    return p;
}
void verifyListeningWav(Fixture& f, Sample origin, Sample inputCorrection, Sample length)
{
    const auto& take = f.document.getProject().media->takes.back();
    require(take.mode == TakeMode::normal && take.N0 == origin && take.logicalLength == length,
        "Listening recording remains a normal take with the corrected clock origin");
    for (const auto& id : take.microphoneAssetIds)
    {
        const auto& asset = *f.document.getProject().media->findAsset(id);
        const auto wav = MediaIndex::recordedAudio(asset, f.config.projectDirectory, f.rate, newId());
        auto source = wavAudioSource(wav); std::vector<float> l(size_t(length), 0), r(size_t(length), 0);
        source->read(0, unsigned(length), l.data(), r.data());
        for (Sample i = 0; i < length; ++i)
        {
            const auto native = origin + inputCorrection + i;
            const bool second = f.microphones > 1 && id != take.microphoneAssetIds.front();
            require(l[size_t(i)] == (second ? float(-345678 - native) : float(123456 + native)) / 8388608.0f, "WAV input coordinate must subtract input latency exactly once");
            const auto expectedRight = f.stereo ? float(-345678 - native) / 8388608.0f : l[size_t(i)];
            require(r[size_t(i)] == expectedRight, "Sparse stereo WAV channels preserve the same corrected sample range");
        }
    }
    for (const auto& track : f.document.getProject().tracks) for (const auto& clip : track.clips.items())
        if (clip.assetId == take.cam1AssetId || clip.assetId == take.microphoneAssetIds.front())
            require(clip.timelineStartSample == f.config.placementSample && clip.sourceIn == 0 && clip.lengthSamples == length,
                "Latency correction does not move the requested timeline placement or create a source offset");
    require(f.document.getProject().takeStacks.empty(), "Normal listening recording must not create a dubbing stack");
}
struct ListeningStall { std::atomic<bool> hold{false}, entered{false}; };
class StalledListeningAudio final : public IPlaybackBlockProvider
{
public:
    StalledListeningAudio(std::unique_ptr<IPlaybackBlockProvider> p, std::shared_ptr<ListeningStall> s)
        : source(std::move(p)), stall(std::move(s)) {}
    void render(float* p, unsigned n, Sample first, unsigned Fs) override
    {
        if (stall->hold.load())
        { stall->entered = true; while (stall->hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        source->render(p, n, first, Fs);
    }
private:
    std::unique_ptr<IPlaybackBlockProvider> source;
    std::shared_ptr<ListeningStall> stall;
};
struct ReleaseListeningStall
{
    std::shared_ptr<ListeningStall> stall;
    ~ReleaseListeningStall() { stall->hold = false; }
};
void addTimelineRecordingTests(recorder_test::Suite& suite)
{
    for (int selection = 0; selection < 3; ++selection)
        suite.test(selection == 0 ? "Timeline recording outputs all imported and existing mic lanes from P"
            : selection == 1 ? "Timeline recording listening mix honors mute"
            : "Timeline recording listening mix honors solo and input monitoring", [selection]
        {
            Fixture f; const auto a = listeningLane(f, true, 1, 37), b = listeningLane(f, true, 2), mic = listeningLane(f, false, 3);
            ok(f.document.performEdit("Listening selection", [&](EditState& e)
            { for (auto& track : e.tracks) { if (selection == 1 && track.trackId == b.track) track.mute = true; if (selection == 2 && track.trackId == a.track) track.solo = true; } }));
            if (selection == 2) { f.monitorInput = .125f; f.audio.setInputMonitoring(true, 1u << 5); }
            constexpr Sample P = 137, length = 401; const auto outputBase = f.position;
            listenAt(f, P); require(bool(f.config.listeningAudio), "Audible future audio selects listening capture");
            const auto before = RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(f.document.getProject()));
            const auto depth = f.document.getHistory().undoDepth(); const auto origin = f.begin();
            require(f.document.isRecordingStructureLocked() && before == RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(f.document.getProject())), "Recording keeps the source clips intact until Stop");
            finishListening(f, origin + length); f.feed();
            for (Sample i = 0; i < length; ++i)
            {
                const auto x = P + i; float l = float(listeningPcm(a.seed, x - a.start)) / 8388608.f, r = -l / 2;
                unsigned count = 1;
                if (selection == 0) { const float v = float(listeningPcm(b.seed, x)) / 8388608.f; l += v; r -= v / 2; ++count; }
                if (selection != 2) { const float v = float(listeningPcm(mic.seed, x)) / 8388608.f; l += v; r += v; ++count; }
                l = l / count + f.monitorInput; r = r / count + f.monitorInput;
                const auto index = size_t(origin - outputBase + i);
                require(std::abs(f.outputL[index] - l) < 1e-7f && std::abs(f.outputR[index] - r) < 1e-7f,
                    "ASIO listening output differs from independent known-source sample oracle");
            }
            for (size_t i = size_t(origin - outputBase + length); i < f.outputL.size(); ++i)
                require(std::abs(f.outputL[i] - f.monitorInput) < 1e-7f, "Stop detaches timeline audio while monitoring stays active");
            verifyListeningWav(f, origin, 0, length);
            require(f.document.getHistory().undoDepth() == depth + 1, "Listening overwrite is one normal placement transaction");
            ok(f.document.undo()); require(before == RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(f.document.getProject())), "Undo restores overwritten microphone without changing imported tracks");
        });
    for (unsigned rate : {8000u, 48000u})
        suite.test(rate == 8000 ? "Listening input/output latency and residuals align every PCM sample at 8 kHz"
            : "Listening input/output latency and camera origin align at 48 kHz", [rate]
        {
            constexpr Sample Lin = 173, Lout = 287, Rin = -11, Rout = 23, P = 137, length = 1601;
            Fixture f(1, true, int(Lin), int(Lout), rate, 128); listeningLane(f, true, 1, 0, rate);
            f.config.cameraSymbolicLink = "synthetic-camera";
            f.config.calibration[0] = listeningCalibration(f, Rin, Rout, 10000); // 1 ms camera residual
            while (f.position < rate / 3) f.feed();
            until([&] { const auto clock = f.audio.masterClock().snapshot(); return clock && clock->valid; });
            const auto outputBase = f.position; listenAt(f, P); const auto origin = f.begin();
            require(f.video->origin == origin && f.video->cameraLatency == 10000, "Camera receives the audible origin and its own residual");
            finishListening(f, origin + length);
            const auto report = f.controller.report(); const auto submit = Sample(report["outputSubmissionSample"]);
            require(origin == submit + Lout + Rout && Sample(report["inputCorrectionSamples"]) == Lin + Rin, "Independent device and residual terms reach the common origin");
            for (Sample i = 0; i < length; ++i)
                require(f.outputL[size_t(submit - outputBase + i)] == float(listeningPcm(1, P + i)) / 8388608.f,
                    "Output P sample must be submitted at S0, including a partial first/last callback");
            verifyListeningWav(f, origin, Lin + Rin, length);
            const auto& meta = f.controller.placementMetadata();
            require(meta.N0 == origin && meta.Nstop == origin + length && meta.timelineSample == P, "UI placement/elapsed metadata uses corrected capture boundaries");
            // Same production camera mapper used by LiveTakeVideo, with the
            // origin/Lcam actually handed to its injected video stream above.
            const auto master = f.video->master->snapshot(); require(master && master->valid, "Prepared synthetic master clock");
            CameraClockMapper camera(*f.video->master, qpcFrequency(), {60,1}, f.video->cameraLatency);
            FrameStamp frame;
            const Sample imageAt = origin + 777;
            for (unsigned i = 0; i < 6; ++i)
            {
                const auto at = imageAt + rescaleRound(10000, rate, 10000000) - Sample(5 - i) * rate / 60;
                frame.frame = i + 1; frame.generation = 1; frame.callback = f.qpc + rescaleRound(at, qpcFrequency(), rate);
                frame.pts100ns = rescaleRound(at, 10000000, rate); frame.hasDeviceTimestamp = frame.deviceTimestampValid = true;
                frame.deviceTimestamp100ns = std::uint64_t(rescaleRound(frame.callback, 10000000, qpcFrequency())); camera.observe(frame);
            }
            const auto cam = camera.snapshot(); require(cam && cam->valid, "Prepared synthetic camera clock");
            AnchoredCameraTimeMapper mapper(*master, *cam, f.video->origin, rate);
            const auto placedFrame = P + rescaleRound(mapper.map(frame), rate, 10000000);
            require(std::abs(placedFrame - (P + 777)) <= 1, "Camera image and heard timeline sample differ by at most one sample of rational rounding");
            JournalReplay journal; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), journal));
            const auto& started = journal.records.front().payload;
            require(started["placementMode"].toString() == "normal" && Sample(started["Pstart"]) == P && Sample(started["N0"]) == origin,
                "Corrected listening capture retains the normal recovery journal schema");
        });
    suite.test("Timeline with no future audible span keeps the original placement and reporting path", []
    {
        for (int kind = 0; kind < 4; ++kind)
        {
            Fixture f(1, false, 173, 287); Sample P = 137;
            if (kind)
            {
                const auto lane = listeningLane(f, true, 1, 0, 600);
                if (kind == 1) P = 600;
                else if (kind == 2) ok(f.document.performEdit("Mute all", [](EditState& e) { e.tracks[0].mute = true; }));
                else
                {
                    auto asset = *f.document.getProject().media->findAsset(lane.asset); asset.availableRanges = {{0, 100}}; asset.gaps = {{100, 500}}; ++asset.mediaGeneration;
                    ok(f.document.updateMediaAsset(asset));
                }
            }
            listenAt(f, P); require(!f.config.listeningAudio, "No future audible span must leave listening factory empty");
            const auto origin = f.begin(); finishListening(f, origin + 401);
            require(f.controller.placementSample() == P && !f.controller.report().hasProperty("timelineListening")
                && !f.audio.telemetry().hasProperty("alignedToOutput"), "Original normal placement/report path remains unchanged");
            verifyListeningWav(f, origin, 0, 401);
            require(std::all_of(f.outputL.begin(), f.outputL.end(), [](float x) { return x == 0; }), "No reference audio output without future audio");
        }
    });
    suite.test("Listening records every armed sparse microphone and keeps their original channels", []
    {
        Fixture f(2, false, 17, 29); listeningLane(f, true, 1); listenAt(f, 137);
        const auto origin = f.begin(); finishListening(f, origin + 401); verifyListeningWav(f, origin, 17, 401);
        const auto& take = f.document.getProject().media->takes.back();
        require(take.microphoneAssetIds.size() == 2 && take.capture.physicalInputs == std::vector<int>({2, 5}), "Both armed microphone slots are preserved");
    });
    suite.test("Listening fit observation epoch changes retain the same audible and capture origins", []
    {
        Fixture f; listeningLane(f, true, 1, 0, 16000);
        while (f.position < 2800) f.feed();
        until([&] { const auto clock = f.audio.masterClock().snapshot(); return clock && clock->valid; });
        listenAt(f, 137); const auto origin = f.begin();
        const auto epoch = f.audio.masterClock().snapshot()->epoch;
        ++f.sequence; // Same native sample axis/QPC, missing only a fit observation.
        for (unsigned i = 0; i < 35; ++i) { f.feed(); f.controller.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        until([&] { const auto clock = f.audio.masterClock().snapshot(); return clock && clock->epoch > epoch; });
        require(f.controller.state() == TakeController::State::recording && f.audio.startSample() == origin, "Fit epoch churn cannot abort or reanchor a listening take");
        finishListening(f, f.position + 81);
    });
    suite.test("Recording tab with imported audio stays silent and appends at active end", []
    {
        Fixture f(1, false, 173, 287); listeningLane(f, true, 1, 37, 4000);
        f.session.enterTimeline(false); f.session.scrub(137, true); RecordingPlacementTestAccess::configure(f.session, f.config); f.saveOutput = true;
        require(f.config.placementSample == 4037 && !f.config.listeningAudio, "Recording tab retains append policy with imported audio");
        const auto origin = f.begin(); finishListening(f, origin + 401); verifyListeningWav(f, origin, 0, 401);
        require(std::all_of(f.outputL.begin(), f.outputL.end(), [](float x) { return x == 0; }), "Recording tab must not submit reference PCM");
    });
    suite.test("Session Stop after default listening start publishes P plus elapsed before finalizer drain", []
    {
        Fixture f(1, false, 173, 287); listeningLane(f, true, 1, 0, 16000); listenAt(f, 137); f.arm();
        const auto beforeStart = f.position; ok(f.controller.start()); const auto origin = f.controller.scheduledStart();
        const auto lead = f.controller.startLeadSamples(); const auto Fs = std::int64_t(f.audio.deviceInfo().sampleRate);
        require(lead >= Fs / 10 && lead >= std::int64_t(f.block) * 2 && lead <= (std::max)(Fs / 4, std::int64_t(f.block) * 2), "Default start lead must stay between 100 ms (or two blocks) and 250 ms");
        require(origin == beforeStart + lead + 287, "Default start reserves the submission lead plus reported output latency");
        until([&] { return f.audio.startCommitted(); });
        until([&] { f.feed(); f.session.tick(); return f.audio.acceptedEnd() >= origin + 401; });
        require(f.session.elapsed() == f.audio.acceptedEnd() - origin, "Recording UI elapsed is the corrected confirmed capture prefix");
        f.video->release = false; ok(f.session.stopRecording()); const auto stop = f.audio.requestedStopSample();
        require(stop == f.position + f.block + 287, "Session Stop reserves matching future input and output boundaries");
        until([&] { f.feed(); f.session.tick(); return f.controller.placementMetadata().ready; });
        require(f.session.playhead() == 137 + stop - origin && !f.session.playing(), "Placement-ready cursor uses P plus corrected length while files are still draining");
        f.video->release = true; f.complete(); verifyListeningWav(f, origin, 173, stop - origin);
    });
    suite.test("Listening starts in a gap, reaches future audio and continues in silence past its end", []
    {
        Fixture f(0); listeningLane(f, true, 1, 500, 401); const auto outputBase = f.position; listenAt(f, 137);
        require(bool(f.config.listeningAudio), "A future audible clip activates listening even when P is in a gap");
        const auto origin = f.begin();
        while (f.audio.acceptedEnd() < origin + 2400) { f.feed(); f.controller.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(f.controller.state() == TakeController::State::recording && f.audio.stopSample() < 0, "Audio end never auto-stops normal recording");
        const auto stop = f.position + 81; finishListening(f, stop); f.feed();
        for (Sample i = 0; i < 363; ++i) require(f.outputL[size_t(origin - outputBase + i)] == 0, "Leading timeline gap renders silence");
        require(f.outputL[size_t(origin - outputBase + 450)] != 0, "Future reference audio is heard");
        for (size_t i = size_t(origin - outputBase + 764); i < f.outputL.size(); ++i) require(f.outputL[i] == 0, "Audio tail and stopped output are silent");
        require(f.document.getProject().media->takes.back().microphoneAssetIds.empty(), "Camera-only listening creates no microphone files");
        require(Sample(f.controller.report()["timelineListening"]["outputStopSample"]) == stop, "Stop has an exact output boundary");
    });
    for (int failure = 0; failure < 3; ++failure)
        suite.test(failure == 0 ? "Listening ASIO reset preserves the same normal take prefix and recovery placement"
            : failure == 1 ? "Listening prefetch underrun stops output and preserves raw media"
            : "Listening latency change stops at the confirmed input prefix", [failure]
        {
            Fixture f(1, false, 17, 29); listeningLane(f, true, 1, 0, 16000); listenAt(f, 137);
            auto stall = std::make_shared<ListeningStall>(); ReleaseListeningStall release{stall};
            const auto factory = f.config.listeningAudio;
            f.config.listeningAudio = [factory, stall] { return std::make_unique<StalledListeningAudio>(factory(), stall); };
            const auto origin = f.begin(); f.feed();
            if (failure == 0) f.audio.deviceDiscontinuity(RecorderAudioEngine::Error::asioReset);
            else if (failure == 2) { ++f.latencyChanges; f.feed(); }
            else
            {
                stall->hold = true;
                until([&] { f.feed(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); return f.audio.error() != RecorderAudioEngine::Error::none; });
                require(stall->entered.load(), "Underrun injection actually suspended the render worker"); stall->hold = false;
            }
            const auto length = f.audio.acceptedEnd() - origin; f.complete();
            require(length > 0 && f.controller.state() == TakeController::State::partialFailure, "Failure must preserve a nonempty partial normal take");
            verifyListeningWav(f, origin, 17, length); verifyFinalizedAssets(f, length);
            require(!f.audio.shutdownBlocker()->load() && !f.document.isRecordingStructureLocked(), "Failure drains and releases both audio and edit locks");
            f.outputL.clear(); f.outputR.clear(); f.feed(); require(std::all_of(f.outputL.begin(), f.outputL.end(), [](float x) { return x == 0; }), "Failure detaches reference output");
            RecoveryReport recovery; ok(RecoveryScanner().run(f.config.projectDirectory, recovery));
            const auto* take = recovery.project.media->findTake(f.config.takeId.toString());
            require(take && take->mode == TakeMode::normal && take->placementSample == 137 && take->logicalLength == length,
                "Recovery preserves corrected length and P without making a dubbing stack");
        });
    suite.test("Listening preparation failure releases ownership before a clean normal take retry", []
    {
        Fixture f; listeningLane(f, true, 1); listenAt(f, 137);
        f.config.listeningAudio = []() -> std::unique_ptr<IPlaybackBlockProvider> { throw std::runtime_error("injected listening source failure"); };
        ok(f.controller.prepare(f.config)); f.complete();
        require(f.controller.state() == TakeController::State::partialFailure && !f.audio.shutdownBlocker()->load()
            && !f.document.isRecordingStructureLocked() && f.document.getProject().media->takes.empty(), "Failed preparation creates no placed take or retained lock");
        f.config.takeId = juce::Uuid(); f.config.listeningAudio = {}; const auto origin = f.begin(); finishListening(f, origin + 401);
    });
    suite.test("Shutdown of an armed listening take detaches output without another ASIO callback", []
    {
        Fixture f; listeningLane(f, true, 1); listenAt(f, 137); f.arm(); ok(f.controller.start());
        until([&] { return f.audio.startCommitted(); }); f.controller.requestShutdown(); f.complete();
        require(f.controller.shutdownComplete() && !f.audio.shutdownBlocker()->load() && !f.document.isRecordingStructureLocked(), "Armed shutdown releases output and journal even when ASIO never calls again");
        f.feed(); require(std::all_of(f.outputL.begin(), f.outputL.end(), [](float x) { return x == 0; }), "No reference escapes after armed shutdown");
    });
}
}
