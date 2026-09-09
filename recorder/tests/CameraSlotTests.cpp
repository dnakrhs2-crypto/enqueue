#include "TestSupport.h"
#include "app/RecorderSettings.h"
#include "capture/CameraCatalog.h"
#include "record/TakeController.h"
#include "sync/CameraClockMapper.h"
#include "FramePatternSource.h"
#include <chrono>
#include <thread>

using namespace gocue::recorder;
using recorder_test::require;
namespace
{
void ok(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
template<class F> void until(F f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!f()) { require(std::chrono::steady_clock::now() < deadline, "Camera slot worker timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
CameraMode mode(unsigned fps = 30) { return CameraMode::parse("NV12 1920x1080 " + std::to_string(fps) + "/1"); }
CameraDevice device(const char* link, unsigned index = 0)
{ CameraDevice d; d.symbolicLink = link; d.friendlyName = "Identical friendly name"; d.modes.push_back(mode()); d.modes[0].nativeIndex = index; return d; }
UserSettings selections()
{
    UserSettings s; s.cameraEnabled = {true, true}; s.cameraDeviceIds = {"uvc:A", "uvc:B"}; s.cameraModes = {mode().text(), mode().text()}; return s;
}
struct Lane
{
    std::atomic<unsigned> prepares{0}, packets{0}, offers{0};
    std::atomic<bool> failed{false}, throwAudio{false}, finishing{false}, release{true};
    std::atomic<std::int64_t> start{-1}, end{-1}, available{0};
};
class VideoDouble final : public ITakeVideoStream
{
public:
    explicit VideoDouble(std::shared_ptr<Lane> s) : lane(std::move(s)) {}
    void prepare(const juce::File& path, NvencProfile, Rational, const AVCodecContext&) override
    {
        ++lane->prepares; output = path;
        require(output.withFileExtension("recording.mp4").replaceWithText("camera-slot lifecycle double; not media"), "Create explicit test double");
    }
    void startAt(ClockMapping clock, std::int64_t n0, unsigned, std::function<std::int64_t()>) override
    { require(clock.valid, "Common ASIO mapping is valid"); lane->start = n0; }
    void offer(const VideoSurface&) noexcept override { ++lane->offers; }
    void audioPacket(const AVPacket&) override
    { if (lane->throwAudio) throw std::runtime_error("Injected single-camera mux failure"); ++lane->packets; }
    bool ready() const noexcept override { return true; }
    void sourceFailed(std::int64_t sample) noexcept override { if (!lane->failed.exchange(true)) lane->available = sample; }
    void endAt(std::int64_t length) noexcept override { lane->end = length; if (!lane->failed) lane->available = length; }
    void audioDone() noexcept override {}
    void finish() override
    {
        lane->finishing = true; until([&] { return lane->release.load(); });
        if (!output.existsAsFile()) require(output.withFileExtension("recording.mp4").moveFileTo(output), "Finalize test double");
    }
    bool failed() const noexcept override { return lane->failed.load(); }
    std::int64_t availableSamples() const noexcept override { return lane->available.load(); }
    bool thumbnailReady() const noexcept override { return false; }
    juce::var report() const override { auto v = jsonObject(); jsonSet(v, "source", "camera-slot lifecycle double; not encoded media"); return v; }
private:
    std::shared_ptr<Lane> lane;
    juce::File output;
};
struct Fixture
{
    RecorderDocument document;
    RecorderAudioEngine audio;
    std::array<std::shared_ptr<Lane>, 2> lanes{std::make_shared<Lane>(), std::make_shared<Lane>()};
    unsigned factories = 0;
    TakeController take{document, audio, [this] { return std::make_unique<VideoDouble>(lanes.at(factories++ % 2)); }};
    TakeController::Config config;
    std::int64_t position = 0, baseQpc = qpcNow();
    std::uint64_t sequence = 0;
    Fixture()
    {
        std::array<int, 8> inputs; inputs.fill(-1); inputs[0] = 0;
        ok(audio.setInputMap(inputs)); ok(audio.openSynthetic(8000, 80, 1, 2)); ok(audio.arm(0, true));
        document.newProject("Camera slots", 8000, {60,1});
        config.projectDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("CameraSlotTests-" + juce::Uuid().toString());
        config.synthetic = true; config.cameraMode = mode(); config.cameraGeneration = 11;
        config.camera2.enabled = true; config.camera2.synthetic = true; config.camera2.mode = mode(60); config.camera2.generation = 22;
        for (int i = 0; i < 4; ++i) feed(); until([&] { return audio.clockReady(); });
    }
    ~Fixture() { for (auto& lane : lanes) lane->release = true; }
    void feed()
    {
        std::array<std::uint8_t, 240> pcm{}; std::array<float, 80> left{}, right{}, input{};
        for (unsigned i = 0; i < 80; ++i) WavTrackWriter::packPcm24(12345 + int(position + i), pcm.data() + i * 3);
        NativeInputView view{pcm.data(), 0, 0, nativeFormatForAsio(17)};
        const float* inputs[]{input.data()}; float* outputs[]{left.data(), right.data()};
        BlockStamp stamp{}; stamp.flags = samplePositionValid; stamp.sequence = sequence++; stamp.samplePosition = position;
        stamp.sampleRate = 8000; stamp.numSamples = 80; stamp.callbackQpc = baseQpc + position * qpcFrequency() / 8000;
        audio.processBlock(stamp, &view, 1, inputs, outputs, 2); position += 80;
    }
    std::int64_t begin()
    {
        ok(take.prepare(config)); until([&] { take.tick(); return take.state() == TakeController::State::armed; });
        const auto n0 = position + 81; ok(take.start(n0)); until([&] { return audio.startCommitted(); });
        while (audio.startSample() < 0) { feed(); take.tick(); } return n0;
    }
    void stopAt(std::int64_t end)
    { ok(take.stop(end)); while (audio.stopSample() < 0) { feed(); take.tick(); } take.tick(); }
    void finish()
    { until([&] { take.tick(); return take.state() == TakeController::State::done || take.state() == TakeController::State::partialFailure; }); }
};
}
int runCameraSlotTests()
{
    recorder_test::Suite suite;
    suite.test("Stable symbolic links survive enumeration reorder and native type index changes", []
    {
        CameraCatalog c; auto s = selections(); ok(c.configure(s)); c.refresh({device("uvc:A", 1), device("uvc:B", 9)});
        const auto generation = c.slot(0).generation; c.refresh({device("UVC:b", 2), device("UVC:a", 23)});
        require(c.slot(0).mode->nativeIndex == 23 && c.slot(1).mode->nativeIndex == 2, "Native mode follows symbolic link, not list position");
        require(c.slot(0).generation == generation, "Reordered native index does not reset a device");
        auto wrong = device("uvc:A"); wrong.modes[0] = mode(60); c.refresh({wrong, device("uvc:B")});
        require(c.slot(0).status == CameraSlotStatus::modeUnavailable && !c.slot(0).mode, "Never substitute another native rate");
    });
    suite.test("Duplicate enabled camera is rejected transactionally including symbolic link case", []
    {
        CameraCatalog c; auto s = selections(); ok(c.configure(s)); s.cameraDeviceIds[1] = "UVC:a";
        require(c.configure(s).failed() && c.slot(1).symbolicLink == "uvc:B", "Reject duplicate without changing either slot");
    });
    suite.test("Disconnect/reconnect changes only that slot generation and never auto-substitutes", []
    {
        CameraCatalog c; ok(c.configure(selections())); c.refresh({device("uvc:A"), device("uvc:B")});
        const auto a = c.slot(0).generation, b = c.slot(1).generation;
        c.refresh({device("uvc:A"), device("uvc:C")});
        require(c.slot(1).status == CameraSlotStatus::disconnected && c.slot(1).generation > b, "Missing cam2 invalidates generation");
        const auto missing = c.slot(1).generation; c.refresh({device("uvc:A")}); require(c.slot(1).generation == missing, "Repeated absence is idempotent");
        c.refresh({device("uvc:B"), device("uvc:A")});
        require(c.slot(1).ready() && c.slot(1).generation > missing && c.slot(0).generation == a, "Reconnect restores saved slot only");
        c.disconnected(1); require(!c.slot(1).ready(), "Explicit capture failure invalidates catalog readiness");
    });
    suite.test("RecorderSettings persists enabled flags, stable links and native modes", []
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("CameraSettingsTests-" + juce::Uuid().toString());
        const auto s = selections(); { RecorderSettings store(root); ok(store.set(s)); ok(store.save().get()); }
        RecorderSettings restored(root); ok(restored.load()); require(restored.get().cameraDeviceIds == s.cameraDeviceIds && restored.get().cameraModes == s.cameraModes, "Native selections roundtrip");
        auto off = restored.get(); off.cameraEnabled[1] = false; ok(restored.set(off)); ok(restored.save().get());
        RecorderSettings again(root); ok(again.load()); require(!again.get().cameraEnabled[1] && again.get().cameraModes[1] == s.cameraModes[1], "OFF preserves saved mode for the next take");
    });
    for (const bool missing : {false, true}) suite.test(missing ? "Missing cam2 creates no source/encoder/file/asset" : "OFF cam2 creates no source/encoder/file/asset", [missing]
    {
        Fixture f; f.config.camera2.enabled = missing; f.config.camera2.synthetic = false; f.config.camera2.symbolicLink.clear();
        const auto n0 = f.begin(); require(!f.take.cameraActive(1) && !f.take.previewPool(1) && f.factories == 1, "No second pipeline created");
        f.stopAt(n0 + 1601); f.finish();
        const auto& t = f.document.getProject().media->takes.front(); require(t.cam2AssetId.isEmpty() && t.state == TakeState::complete, "Normal one-camera model");
        const auto folder = f.config.projectDirectory.getChildFile("media/takes/" + f.config.takeId.toDashedString());
        require(!folder.getChildFile("cam2.mp4").exists() && !folder.getChildFile("cam2.recording.mp4").exists(), "No fake cam2 media");
        JournalReplay journal; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), journal));
        for (const auto& entry : journal.records) require(!juce::JSON::toString(entry.payload).contains("cam2.mp4"), "Journal has no planned cam2 file");
    });
    suite.test("Two lanes share N0/Nstop; stopping reaches both before a slow finalizer", []
    {
        Fixture f; const auto n0 = f.begin(); f.lanes[0]->release = false; f.stopAt(n0 + 1601);
        until([&] { return f.lanes[0]->finishing.load(); });
        require(f.lanes[0]->start == n0 && f.lanes[1]->start == n0, "Same N0 for both MP4 streams");
        require(f.lanes[0]->end == 1601 && f.lanes[1]->end == 1601, "Both exact exclusive ends delivered before join");
        require(f.document.getProject().media->takes.size() == 1, "Immediate placement while one finalizer waits");
        f.lanes[0]->release = true; f.finish(); require(f.take.state() == TakeController::State::done, "Two-camera normal completion");
        const auto report = f.take.report(); require(report["cameras"].size() == 2, "Two independent lane reports");
    });
    suite.test("Cam2 disconnect leaves its gap and preserves cam1/raw WAV through Nstop", []
    {
        Fixture f; const auto n0 = f.begin(); for (int i = 0; i < 10; ++i) f.feed();
        f.take.cameraFailed(1, 22); f.take.tick(); const auto available = f.lanes[1]->available.load();
        require(f.take.warning().contains(juce::String::fromUTF8("캠2 연결이 끊겼습니다. 캠1과 원본 녹음은 계속됩니다.")), "Exact partial-failure banner");
        require(f.take.state() == TakeController::State::recording && f.audio.stopSample() < 0, "Cam2 failure never stops healthy collection");
        f.stopAt(n0 + 8001); f.finish(); const auto& p = f.document.getProject(); const auto& t = p.media->takes.front();
        const auto* a = p.media->findAsset(t.cam1AssetId); const auto* b = p.media->findAsset(t.cam2AssetId); const auto* wav = p.media->findAsset(t.microphoneAssetIds.front());
        require(t.logicalLength == 8001 && a->gaps.empty() && a->availableRanges.front().length == 8001, "Healthy camera remains full length");
        require(b->gaps.size() == 1 && b->gaps.front().start == available && b->gaps.front().length == 8001 - available, "Only failed camera tail is a gap");
        require(wav->gaps.empty() && wav->availableRanges.front().length == 8001, "Original WAV remains full length");
        require(f.take.report()["cameras"][1]["gaps"].size() == 1, "Gap is persisted in the take manifest");
    });
    for (unsigned failed : {0u, 1u}) suite.test(failed ? "Cam2 AAC mux rejection cannot stop cam1/raw" : "Cam1 AAC mux rejection cannot stop cam2/raw", [failed]
    {
        Fixture f; const auto n0 = f.begin(); f.lanes[failed]->throwAudio = true; f.stopAt(n0 + 8001); f.finish();
        require(f.lanes[failed]->failed && f.lanes[1 - failed]->packets > 0 && !f.lanes[1 - failed]->failed, "AAC fanout isolates exceptions per lane");
        require(!f.audio.referenceFailed() && std::int64_t(f.take.report()["audio"]["wav"]["writtenSamplesPerMic"]) == 8001, "Reference dispatcher and raw writer stay alive");
    });
    suite.test("Stale frame and disconnect generations are ignored independently", []
    {
        Fixture f; const auto n0 = f.begin(); VideoSurface frame; frame.prepare(2, 2); frame.stamp.generation = 21;
        f.take.offer(1, frame); f.take.cameraFailed(1, 21); require(f.lanes[1]->offers == 0 && !f.lanes[1]->failed, "Reject previous cam2 incarnation");
        frame.stamp.generation = 22; f.take.offer(1, frame); frame.stamp.generation = 11; f.take.offer(frame);
        require(f.lanes[0]->offers == 1 && f.lanes[1]->offers == 1, "Correct generation routed to each camera");
        f.stopAt(n0 + 1601); f.finish(); require(std::int64_t(f.take.report()["cameras"][1]["staleOffers"]) == 1, "Discard is counted");
    });
    suite.test("New cam2 generation stays a partial failure even after reanchor notifications", []
    {
        Fixture f; const auto n0 = f.begin(); for (int i = 0; i < 10; ++i) f.feed();
        VideoSurface frame; frame.stamp.generation = 23;
        f.take.cameraDiscontinuity(1, 22); f.take.offer(1, frame);
        const auto available = f.lanes[1]->available.load();
        require(f.lanes[1]->failed && f.take.cameraDisconnected(1), "A new incarnation must not resume this take lane");
        f.take.cameraDiscontinuity(1, 22); frame.stamp.generation = 22; f.take.offer(1, frame);
        f.stopAt(n0 + 8001); f.finish();
        const auto& p = f.document.getProject(); const auto& take = p.media->takes.front();
        const auto* cam2 = p.media->findAsset(take.cam2AssetId);
        require(f.take.state() == TakeController::State::partialFailure && cam2->gaps.size() == 1
                && cam2->gaps.front().start == available, "Reconnect retains the failed tail boundary");
        require(p.media->findAsset(take.cam1AssetId)->gaps.empty()
                && p.media->findAsset(take.microphoneAssetIds.front())->gaps.empty(), "Cam1 and original WAV stay complete");
    });
    suite.test("Slow encoder cannot borrow the other camera's surfaces or either preview mailbox", []
    {
        VideoSurfacePool a(320, 192), b(320, 192); NvencFramePool encA(4, 320, 192), encB(4, 320, 192);
        VideoSurface input; input.prepare(320, 192); probe::paintPattern(input, {1,1});
        std::array<int, 4> held{}; for (auto& h : held) { require(encA.copy(input) && encA.pop(h), "Pin cam1 encoder surface"); }
        for (unsigned frame = 1; frame <= 60; ++frame)
        {
            require(!encA.copy(input), "Cam1 pool stays bounded when encoder retains every surface");
            const auto first = a.acquireWrite(); require(first != VideoSurfacePool::none, "Cam1 preview remains independent of its encoder"); a.publish(first);
            const auto next = b.acquireWrite(); require(next != VideoSurfacePool::none, "Cam2 has its own reserved preview surfaces");
            auto& surface = b.surface(next); probe::paintPattern(surface, {frame,2}); surface.stamp.frame = frame;
            require(encB.copy(surface), "Cam2 encoder progresses despite pinned cam1 pool"); b.publish(next);
            b.uploadLatest([&](const VideoSurface& p) { const auto id = probe::readPattern(p.nv12.data(), 320, 320, 192); require(id && id->camera == 2 && id->frame == frame, "Cam2 latest pixels remain independent"); });
            int slot = -1; require(encB.pop(slot), "Cam2 dequeue"); encB.release(slot);
        }
        require(encA.occupied() == 4 && encB.highWater() == 1 && encB.occupied() == 0, "Independent encode pool counters");
        require(a.snapshot().overwritten == 59 && b.snapshot().consumed == 60 && !b.snapshot().exhausted, "Independent mailbox counters");
        for (auto h : held) encA.release(h);
    });
    suite.test("Each CameraClockMapper owns its generation and native cadence", []
    {
        ClockMapper master(qpcFrequency()); CameraClockMapper a(master, qpcFrequency(), {30,1}), b(master, qpcFrequency(), {60,1});
        for (unsigned i = 1; i <= 60; ++i)
        {
            FrameStamp s; s.frame = i; s.generation = 11; s.pts100ns = std::int64_t(i) * 10000000 / 30; s.callback = qpcFrequency() + std::int64_t(i) * qpcFrequency() / 30; a.observe(s);
            s.generation = 22; s.pts100ns = std::int64_t(i) * 10000000 / 60; s.callback = qpcFrequency() + std::int64_t(i) * qpcFrequency() / 60; b.observe(s);
        }
        const auto epoch = a.snapshot()->epoch; FrameStamp reconnect; reconnect.frame = 1; reconnect.generation = 23; reconnect.callback = qpcFrequency() * 3; b.observe(reconnect);
        require(a.snapshot()->epoch == epoch && a.snapshot()->valid && b.snapshot()->generation == 23, "Cam2 epoch cannot reset cam1 clock");
    });
    return suite.result("camera-slots");
}
