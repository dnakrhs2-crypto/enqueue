#include "TestSupport.h"
#include "../tools/LifecycleFixtures.h"

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
    suite.test("Audio device failure surfaces synchronously from configure on the caller thread", []
    {
        // Apartment-model ASIO drivers can only be created on the (STA) message thread, so the
        // device negotiation must run on the caller and report there, not on the camera worker.
        RecorderDocument document; RecorderSession session(document);
        int callbacks = 0; std::thread::id callbackThread; juce::Result callbackResult = juce::Result::ok();
        session.onConfigured = [&](const juce::Result& r, const UserSettings&) { ++callbacks; callbackThread = std::this_thread::get_id(); callbackResult = r; };
        UserSettings s; s.asioDeviceId = "recorder-test-missing-asio-device"; s.cameraEnabled = {false, false};
        const auto result = session.configure(s);
        require(result.failed(), "Missing ASIO device must fail configure synchronously");
        require(result.getErrorMessage().contains(juce::String::fromUTF8("장치 연결을 확인하세요")), "Synchronous audio failure keeps the user-facing prefix");
        require(callbacks == 1 && callbackThread == std::this_thread::get_id() && callbackResult.failed(), "onConfigured runs exactly once, synchronously on the caller, with the failure");
        require(!session.configuring() && !session.busy(), "No asynchronous device work remains and the session is not busy");
        require((session.lifecycleState()->snapshot() & RecorderLifecycle::configuring) == 0, "Lifecycle configuring bit is released before configure returns");
        session.tick();
        require(callbacks == 1, "A later tick does not re-emit the completion");
        require(session.audioEngine().deviceInfo().sampleRate == 0, "Failed negotiation leaves no device open");
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
