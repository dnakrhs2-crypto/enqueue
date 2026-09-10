#include "TestSupport.h"
#include "../tools/LifecycleFixtures.h"
#include "ui/UiState.h"

using namespace gocue::recorder;
using recorder_test::require;
int runLifecycleTests()
{
    recorder_test::Suite suite;
    suite.test("Updater rejects every live/unsaved activity and both directions of record/export", []
    {
        RecorderLifecycle state;
        for (auto activity : {RecorderLifecycle::recording, RecorderLifecycle::dubbing, RecorderLifecycle::exporting,
            RecorderLifecycle::recovering, RecorderLifecycle::finalizing, RecorderLifecycle::unsaved, RecorderLifecycle::fileWork, RecorderLifecycle::configuring})
        { state.set(activity, true); require(!state.canShutdown(), "Active blocker permits updater"); state.end(activity); require(state.canShutdown(), "Idle after release"); }
        require(state.begin(RecorderLifecycle::exporting), "Export begins");
        require(!state.begin(RecorderLifecycle::recording) && !state.begin(RecorderLifecycle::dubbing), "Export excludes capture");
        state.end(RecorderLifecycle::exporting); require(state.begin(RecorderLifecycle::recording), "Checkpoint releases export gate");
        require(!state.begin(RecorderLifecycle::exporting), "Capture excludes export");
        state.end(RecorderLifecycle::recording);
        const auto generation = state.generation(); state.invalidate(); require(!state.accepts(generation), "Late seek discarded");
        state.blockCommands(); require(!state.canShutdown() && !state.begin(RecorderLifecycle::recording), "Closing is irreversible for this lifecycle");
    });
    suite.test("Concurrent updater reads only atomic snapshots", []
    {
        RecorderLifecycle state; state.set(RecorderLifecycle::unsaved, true); std::atomic<bool> failed{false};
        std::thread reader([&] { for (int i = 0; i < 10000; ++i) if (state.canShutdown()) failed = true; });
        for (int i = 0; i < 10000; ++i) { state.set(RecorderLifecycle::recording, true); state.end(RecorderLifecycle::recording); }
        reader.join(); require(!failed.load(), "Unsaved revision blocker cannot be lost");
    });
    suite.test("Dubbing ASIO output owns an independent updater/export blocker until callback detach", []
    {
        RecorderAudioEngine audio; RecorderLifecycle state; state.bindCaptureBlocker(audio.shutdownBlocker());
        struct Client final : IAudioOutputClient { void processOutput(const BlockStamp&, float*, float*) noexcept override {} } client;
        audio.setDubbingOutputClient(&client);
        require(!state.canShutdown() && !state.begin(RecorderLifecycle::exporting), "Native dubbing ownership blocks updater/export");
        audio.setDubbingOutputClient(nullptr); require(state.canShutdown(), "Detach barrier releases native ownership");
    });
    suite.test("Application shutdown waits for export acknowledgement and owner commit before releasing devices", []
    {
        RecorderDocument document; RecorderSession session(document); bool checkpointRequested = false;
        lifecycleFixture::check(session.audioEngine().openSynthetic(48000, 480));
        lifecycleFixture::check(session.beginExclusive(RecorderLifecycle::exporting, [&] { checkpointRequested = true; }));
        session.requestShutdown(); session.tick();
        require(checkpointRequested && !session.readyForShutdownCommit(), "Export checkpoint/join acknowledgement is required");
        session.endExclusive(RecorderLifecycle::exporting); session.tick();
        require(session.readyForShutdownCommit() && !session.shutdownComplete(), "Media quiescence precedes owner commit");
        require(session.audioEngine().deviceInfo().sampleRate == 48000, "Device retained until commit authorization");
        session.releaseForShutdown(); lifecycleFixture::until([&] { session.tick(); return session.shutdownComplete(); });
        require(session.audioEngine().deviceInfo().sampleRate == 0, "Device released after commit and worker join");
    });
    suite.test("Audio device failure travels with the completion and never blocks camera configuration", []
    {
        // Apartment-model ASIO drivers can only be created on the (STA) message thread, so the device is
        // negotiated on the caller; the failure is delivered once through onConfigured with the camera work.
        RecorderDocument document; RecorderSession session(document);
        int callbacks = 0; juce::Result callbackResult = juce::Result::ok();
        session.onConfigured = [&](const juce::Result& r, const UserSettings&) { ++callbacks; callbackResult = r; };
        UserSettings s; s.asioDeviceId = "recorder-test-missing-asio-device"; s.cameraEnabled = {false, false};
        require(session.configure(s).wasOk(), "configure accepts the request; the audio failure is reported with the completion");
        lifecycleFixture::until([&] { session.tick(); return !session.configuring() && callbacks == 1; });
        require(callbackResult.failed() && callbackResult.getErrorMessage().contains(juce::String::fromUTF8("장치 연결을 확인하세요")), "Audio failure keeps the user-facing prefix");
        require(session.error.contains(juce::String::fromUTF8("장치 연결을 확인하세요")), "Banner carries the audio failure");
        require((session.lifecycleState()->snapshot() & RecorderLifecycle::configuring) == 0 && !session.busy(), "Configuring ends with the completion");
        require(session.audioEngine().deviceInfo().sampleRate == 0, "Failed negotiation leaves no device open");
        session.tick(); require(callbacks == 1, "A later tick does not re-emit the completion");
    });
    suite.test("Unset ASIO device selects the first driver on this PC with first-run defaults", []
    {
        RecorderDocument document; RecorderSession session(document);
        UserSettings applied; int callbacks = 0; juce::Result callbackResult = juce::Result::ok();
        session.onConfigured = [&](const juce::Result& r, const UserSettings& s) { ++callbacks; applied = s; callbackResult = r; };
        UserSettings s; s.cameraEnabled = {false, false};
        require(session.configure(s).wasOk(), "configure accepts an unset device");
        lifecycleFixture::until([&] { session.tick(); return callbacks == 1; });
        const auto names = RecorderAudioEngine::deviceNames();
        if (names.isEmpty()) { require(applied.asioDeviceId.isEmpty() && session.audioEngine().deviceInfo().sampleRate == 0, "No ASIO driver: nothing selected"); return; }
        require(applied.asioDeviceId == names[0], "The first registry driver is chosen");
        if (callbackResult.wasOk())
        {
            const auto info = session.audioEngine().deviceInfo();
            require(info.sampleRate != 0, "The chosen driver is open");
            require(info.physicalInputs == 0 || (applied.physicalInputs.size() == 1 && applied.physicalInputs[0] == 0), "Microphone 1 defaults to input 1");
            require(info.physicalOutputs < 2 || (applied.output.left == 0 && applied.output.right == 1), "Playback defaults to outputs 1/2");
        }
    });
    suite.test("First-run audio defaults apply once per device choice, mono on a single output, explicit none kept", []
    {
        RecorderAudioEngine::DeviceInfo info; info.sampleRate = 48000; info.physicalInputs = 2; info.physicalOutputs = 2;
        UserSettings s; require(applyAudioDefaults(s, info) && s.audioDefaultsApplied, "Defaults apply on the first open");
        require(s.physicalInputs == std::vector<int>{0} && s.output.left == 0 && s.output.right == 1 && !s.output.mono, "Microphone 1 -> input 1, playback -> outputs 1/2");
        s.output = {}; require(!applyAudioDefaults(s, info) && s.output.left == -1 && s.output.right == -1, "An explicit none is kept once defaults were applied");
        UserSettings m; info.physicalOutputs = 1;
        require(applyAudioDefaults(m, info) && m.output.mono && m.output.monoChannel == 0 && m.output.left == -1 && m.output.right == -1 && m.output.validate().wasOk(), "Single output -> valid mono mapping");
        UserSettings n; info.physicalInputs = 0; info.physicalOutputs = 0;
        require(!applyAudioDefaults(n, info) && n.audioDefaultsApplied && n.physicalInputs.empty() && n.output.left == -1, "No channels -> nothing mapped, marker still set");
        UserSettings closed; RecorderAudioEngine::DeviceInfo none;
        require(!applyAudioDefaults(closed, none) && !closed.audioDefaultsApplied, "No open device -> untouched");
    });
    suite.test("A project without media follows the device sample rate; a fixed project names both rates", []
    {
        RecorderDocument document; require(document.getProject().Fs == 48000, "Provisional default");
        require(!adoptDeviceSampleRate(document, 0), "No open device -> untouched");
        require(adoptDeviceSampleRate(document, 44100) && document.getProject().Fs == 44100, "Provisional project adopts the device rate");
        require(!adoptDeviceSampleRate(document, 44100), "Equal rates -> nothing to do");
        UserSettings s; s.asioDeviceId = "X"; s.physicalInputs = {0}; s.output.left = 0; s.output.right = 1;
        RecorderAudioEngine::DeviceInfo info; info.name = "X"; info.sampleRate = 44100; info.physicalInputs = 2; info.physicalOutputs = 2;
        RecorderProject fixed; auto media = std::make_shared<MediaRegistry>(); media->assets.push_back(MediaAsset{}); fixed.media = media;
        const auto r = validateAudioSettings(s, info, fixed);
        require(r.failed() && r.getErrorMessage().contains("48000") && r.getErrorMessage().contains("44100"), "Fixed-project mismatch names both rates");
        require(validateAudioSettings(s, info, RecorderProject{}).wasOk(), "Provisional project accepts any device rate");
    });
    suite.test("Camera input modes get a sensible default per project fps and friendly labels", []
    {
        const CameraMode nv5 = CameraMode::parse("NV12 1920x1080 5/1"), mj60 = CameraMode::parse("MJPEG 1920x1080 60/1"), nv60 = CameraMode::parse("NV12 1920x1080 60/1"),
            yuy30 = CameraMode::parse("YUY2 1920x1080 30/1"), mj5994 = CameraMode::parse("MJPEG 1920x1080 60000/1001"), hd720 = CameraMode::parse("MJPEG 1280x720 60/1");
        const std::vector<CameraMode> modes{nv5, hd720, yuy30, nv60, mj60};
        require(preferred1080pMode(modes, 60) == 4, "60 fps project: MJPEG 60 before NV12 60");
        require(preferred1080pMode(modes, 30) == 2, "30 fps project: the exact 30 fps mode wins over faster ones");
        require(preferred1080pMode({nv5, hd720}, 30) == 0, "Only a slow 1080p mode: still the best available");
        require(preferred1080pMode({hd720}, 30) == -1, "No 1080p mode");
        require(preferred1080pMode({nv5, mj5994}, 60) == 1 && reachesProjectFps(mj5994, 60) && !reachesProjectFps(nv5, 60), "59.94 counts as reaching 60");
        require(friendlyModeText(mj60) == juce::String::fromUTF8("1080p 60fps \xc2\xb7 MJPEG") && friendlyModeText(mj5994) == juce::String::fromUTF8("1080p 59.94fps \xc2\xb7 MJPEG"), "Friendly label");
        require(mj60.text() == "MJPEG 1920x1080 60/1", "Stored form unchanged");
    });
    suite.test("All Korean failure banners have the specified meaning", []
    {
        require(recorderFaultText(RecorderFault::camera2Disconnected) == juce::String::fromUTF8("캠2 연결이 끊겼습니다. 캠1과 원본 녹음은 계속됩니다."), "Camera message");
        require(recorderFaultText(RecorderFault::storageWrite) == juce::String::fromUTF8("저장 장치에 쓸 수 없어 녹화를 멈췄습니다."), "Storage message");
        require(recorderFaultText(RecorderFault::dubbingUnderrun) == juce::String::fromUTF8("오디오 재생이 끊겨 더빙을 중단했습니다."), "Dubbing message");
        require(recorderFaultText(RecorderFault::finalize) == juce::String::fromUTF8("일반 MP4 마무리 실패 · 재시도"), "Retry message");
        require(recorderFaultText(RecorderFault::processingDelay) == juce::String::fromUTF8("처리 지연이 발생했습니다"), "Delay message");
        for (auto fault : {RecorderFault::audioOverflow, RecorderFault::audioReset, RecorderFault::audioRateChanged, RecorderFault::save, RecorderFault::recovery, RecorderFault::updateBusy})
            require(recorderFaultText(fault).isNotEmpty(), "No unmapped failure");
    });
    suite.test("GPU device reset joins old presenter, retries on cooldown, preserves capture", []
    {
        PreviewRecovery recovery; unsigned destroyed = 0, created = 0;
        const auto old = recovery.epoch(); recovery.lost("0x887A0005", 100, [&] { ++destroyed; });
        require(recovery.epoch() > old && destroyed == 1, "Lost device invalidates display generation after join");
        require(!recovery.retry(599, [&] { ++created; return true; }), "Cooldown respected");
        require(recovery.retry(600, [&] { require(destroyed == 1, "Destroy before create"); ++created; return true; }), "Presenter recreated");
        require(created == 1 && !recovery.recovering(), "Single recreation");
        recovery.reset(); recovery.lost("0x887a0007", 0, [] {});
        for (int i = 1; i <= 3; ++i) recovery.retry(i * 500, [] { return false; });
        require(recovery.exhausted(), "Persistent failure never busy-loops");
    });
    suite.test("Closing an armed take never waits for a callback that may not arrive", []
    {
        const auto root = crashFixture::directory("r28-armed-close"); lifecycleFixture::Recording f(root, "app-exit", false);
        lifecycleFixture::check(f.take.prepare(f.config)); lifecycleFixture::until([&] { f.take.tick(); return f.take.state() == TakeController::State::armed; });
        f.take.requestShutdown(); lifecycleFixture::until([&] { f.take.tick(); return f.take.shutdownComplete(); });
        require(!f.document.isRecordingStructureLocked(), "Shutdown releases structure lock after empty cancelled preparation");
    });
    suite.test("Shutdown waits for durable media before join/device release; late callbacks are ignored", []
    {
        lifecycleFixture::Recording f(crashFixture::directory("r28-order"), "finalize-hold", false); f.begin(); f.faults.armed = true;
        f.take.requestShutdown(); lifecycleFixture::until([&] { f.take.tick(); return f.faults.fired.load(); });
        require(!f.take.shutdownComplete() && f.audio.closeDevice().failed(), "Device cannot release ahead of media/journal commit");
        const auto accepted = f.audio.acceptedEnd(); f.feed(); require(f.audio.acceptedEnd() == accepted, "Callback detached before finalizer drain");
        f.faults.released = true; f.finish();
        JournalReplay log; lifecycleFixture::check(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), log));
        bool stopped = false, finalized = false;
        for (const auto& record : log.records)
        { if (record.kind == JournalKind::TakeStopped) stopped = true; if (record.kind == JournalKind::TakeFinalized) { require(stopped, "Durable stop precedes final commit"); finalized = true; } }
        require(finalized, "Final commit reached"); lifecycleFixture::check(f.audio.closeDevice());
    });
    suite.test("Late take finalization cannot publish into a replaced project", []
    {
        lifecycleFixture::Recording f(crashFixture::directory("r28-generation"), "finalize-hold", false); f.begin(); f.faults.armed = true;
        f.take.requestShutdown(); lifecycleFixture::until([&] { f.take.tick(); return f.faults.fired.load(); });
        f.document.newProject("replacement", 48000, {30,1}); const auto newProject = f.document.getProject().projectId;
        f.faults.released = true; f.finish();
        require(f.document.getProject().projectId == newProject && f.document.getProject().media->takes.empty(), "Old finalizer never resurrects old clips");
    });
    for (const auto& fault : {juce::String("project-close"), juce::String("asio-rate-change"), juce::String("disk-full"), juce::String("writer-stall")})
        suite.test(("Production lifecycle + recovery: " + fault).toRawUTF8(), [fault]
        { lifecycleFixture::cycle(crashFixture::directory("r28-cycle"), fault); });
    return suite.result("LifecycleTests");
}
