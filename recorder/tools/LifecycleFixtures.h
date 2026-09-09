#pragma once
// Explicit synthetic adapters. WAV/journal/take/recovery/render implementations
// are production code; CPU H264 replaces the physical camera/NVENC only here.
#include "CrashFixtures.h"
#include "app/RecorderLifecycle.h"
#include "record/TakeController.h"
#include "capture/PreviewRecovery.h"
#include "storage/IoHealth.h"
#include "storage/EditJournal.h"
#include "app/RecorderSession.h"
#include <thread>

namespace gocue::recorder::lifecycleFixture
{
using recovery::check;
using recovery::require;
inline const juce::StringArray cases{"disk-full", "writer-stall", "audio-queue-overflow", "asio-reset", "asio-rate-change",
    "camera-disconnect", "project-close", "app-exit", "resume", "gpu-device-removed", "finalize-failure", "dubbing-underrun"};
template<class F> void until(F f, int timeoutMs = 12000)
{
    const auto deadline = IoHealth::now() + timeoutMs;
    while (!f()) { require(IoHealth::now() < deadline, "Lifecycle worker deadline exceeded"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
struct Faults : FileIoFaultAdapter
{
    std::atomic<bool> armed{false}, fired{false}, released{false};
    juce::String selected;
    ~Faults() override { released = true; }
    juce::Result beforeIo(FileIoOperation op, const juce::File& path, std::uint64_t offset, std::size_t) override
    {
        if (!armed.load()) return juce::Result::ok();
        if (path.hasFileExtension("wav") && op == FileIoOperation::append && offset >= 44)
        {
            if (selected == "disk-full") { fired = true; return juce::Result::fail("Injected ERROR_DISK_FULL (112)"); }
            if (selected == "writer-stall" || selected == "audio-queue-overflow")
            {
                fired = true;
                until([&] { return released.load(); });
            }
        }
        if (selected == "finalize-failure" && path.hasFileExtension("mp4") && op == FileIoOperation::flushData)
        { fired = true; return juce::Result::fail("Injected MP4 final flush failure"); }
        if (selected == "finalize-hold" && path.hasFileExtension("mp4") && op == FileIoOperation::flushData)
        { fired = true; until([&] { return released.load(); }); }
        return juce::Result::ok();
    }
};
class Camera final : public ITakeVideoStream
{
public:
    explicit Camera(Faults& f) : faults(f) {}
    void prepare(const juce::File& file, NvencProfile, Rational, const AVCodecContext&) override
    { writer = std::make_unique<crashFixture::Camera>(file, &faults); }
    void startAt(ClockMapping, std::int64_t, unsigned, std::function<std::int64_t()>) override {}
    void offer(const VideoSurface&) noexcept override { ++offers; }
    void audioPacket(const AVPacket&) override {} // fixture video owns a separately labelled synthetic reference
    bool ready() const noexcept override { return true; }
    void sourceFailed(std::int64_t at) noexcept override { failedFlag = true; available = at; }
    void endAt(std::int64_t at) noexcept override { if (!failedFlag) available = at; }
    void audioDone() noexcept override {}
    void finish() override
    {
        if (!writer) return;
        try { while (writer->samples() < available.load()) writer->frame(); writer->finish(); }
        catch (...) { writer.reset(); failedFlag = true; throw; }
        writer.reset();
    }
    bool failed() const noexcept override { return failedFlag.load(); }
    std::int64_t availableSamples() const noexcept override { return available.load(); }
    bool thumbnailReady() const noexcept override { return false; }
    juce::var report() const override
    { auto r = jsonObject(); jsonSet(r, "source", "synthetic CPU libopenh264 + fixture AAC; no NVENC/camera hardware"); jsonSet(r, "offers", int(offers.load())); return r; }
private:
    Faults& faults;
    std::unique_ptr<crashFixture::Camera> writer;
    std::atomic<bool> failedFlag{false};
    std::atomic<std::int64_t> available{0};
    std::atomic<unsigned> offers{0};
};
struct Recording
{
    Faults faults;
    RecorderDocument document;
    RecorderAudioEngine audio;
    TakeController take{document, audio, [this] { return std::make_unique<Camera>(faults); }};
    TakeController::Config config;
    std::int64_t position = 0, baseQpc = qpcNow();
    std::uint64_t sequence = 0;
    static constexpr unsigned rate = 48000, block = 480;
    Recording(const juce::File& root, const juce::String& fault, bool baseline = true)
    {
        faults.selected = fault;
        if (baseline)
        {
            crashFixture::baseline(root); RecoveryReport recovered; check(RecoveryScanner().run(root, recovered));
            check(document.adopt(recovered.project, root.getChildFile("project.recorder"), recovered.checkpointInfo));
        }
        else { document.newProject("Lifecycle", rate, {30, 1}); check(document.saveCheckpoint(root.getChildFile("project.recorder"))); }
        std::array<int, 8> map{0, -1, -1, -1, -1, -1, -1, -1};
        check(audio.setInputMap(map)); check(audio.openSynthetic(rate, block, 1, 2)); check(audio.arm(0, true));
        config.projectDirectory = root; config.synthetic = true; config.externalCapture = true; config.projectFps = 30;
        config.cameraMode.width = 1920; config.cameraMode.height = 1080; config.cameraMode.fps = {30, 1}; config.cameraGeneration = 71;
        config.camera2.enabled = true; config.camera2.synthetic = true; config.camera2.mode = config.cameraMode; config.camera2.generation = 72;
        config.faults = &faults;
        for (int i = 0; i < 4; ++i) feed(); until([&] { return audio.clockReady(); });
    }
    ~Recording() { faults.released = true; }
    void feed(unsigned fs = rate, unsigned reset = 0)
    {
        std::array<std::uint8_t, block * 3> raw{};
        std::array<float, block> input{}, left{}, right{};
        for (unsigned i = 0; i < block; ++i) WavTrackWriter::packPcm24(crashFixture::pcm(position + i, 0), raw.data() + i * 3);
        NativeInputView view{raw.data(), 0, 0, nativeFormatForAsio(17)};
        const float* inputs[]{input.data()}; float* outputs[]{left.data(), right.data()};
        BlockStamp stamp{}; stamp.flags = samplePositionValid; stamp.sequence = sequence++; stamp.samplePosition = position;
        stamp.sampleRate = fs; stamp.numSamples = block; stamp.callbackQpc = baseQpc + position * qpcFrequency() / rate; stamp.resets = reset;
        audio.processBlock(stamp, &view, 1, inputs, outputs, 2); position += block;
    }
    void begin()
    {
        check(take.prepare(config)); until([&] { take.tick(); return take.state() == TakeController::State::armed; });
        check(take.start(position)); until([&] { return audio.startCommitted(); });
        feed(); take.tick(); require(take.state() == TakeController::State::recording, "Native boundary must adopt recording");
        for (int i = 0; i < 5; ++i) { feed(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    void finish()
    {
        if (take.state() == TakeController::State::recording && audio.error() == RecorderAudioEngine::Error::none)
        { check(take.stop(position + block)); feed(); }
        until([&] { take.tick(); return take.shutdownComplete(); });
    }
};
inline std::map<juce::String, juce::String> mediaHashes(const juce::File& root)
{
    std::map<juce::String, juce::String> result;
    for (auto file : root.getChildFile("media").findChildFiles(juce::File::findFiles, true))
        result[file.getRelativePathFrom(root)] = recovery::sha256(file);
    return result;
}
inline juce::var cycle(const juce::File& root, const juce::String& selected)
{
    auto row = jsonObject(); jsonSet(row, "case", selected); jsonSet(row, "project", root.getFullPathName());
    const auto started = IoHealth::now();
    Recording f(root, selected);
    const auto completedTake = f.document.getProject().media->takes.front().takeId;
    const auto completedClip = f.document.getProject().tracks.front().clips.items().front().clipId;
    const auto savedMarkers = f.document.getProject().markers;
    const auto original = mediaHashes(root);
    f.begin();
    RecorderLifecycle lifecycle; require(lifecycle.begin(RecorderLifecycle::recording), "Record gate");
    require(!lifecycle.canShutdown() && !lifecycle.begin(RecorderLifecycle::exporting), "Update/export denied during recording");
    f.faults.armed = true;
    const auto before = f.audio.acceptedEnd(), epoch = std::int64_t(f.audio.deviceGeneration());
    double delayMs = 0;
    if (selected == "disk-full")
    { f.feed(); until([&] { return f.audio.error() != RecorderAudioEngine::Error::none; }); require(f.audio.error() == RecorderAudioEngine::Error::writeFailed, "Disk full must stop original writer"); }
    else if (selected == "writer-stall")
    {
        f.feed(); until([&] { return f.faults.fired.load(); }); const auto stall = IoHealth::now();
        for (int i = 0; i < 180; ++i) { f.feed(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        until([&] { return f.audio.processingDelayed(); }); delayMs = double(IoHealth::now() - stall);
        require(delayMs <= 1000, "Processing delay must be observable within one second");
        until([&] { return IoHealth::now() - stall >= 2000; }); f.faults.released = true;
        require(f.audio.error() == RecorderAudioEngine::Error::none, "Two-second stall must retain original audio");
    }
    else if (selected == "audio-queue-overflow")
    {
        f.feed(); until([&] { return f.faults.fired.load(); });
        for (int i = 0; i < 1000 && f.audio.error() == RecorderAudioEngine::Error::none; ++i) { f.feed(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(f.audio.error() == RecorderAudioEngine::Error::rawOverflow || f.audio.error() == RecorderAudioEngine::Error::pcmOverflow, "Overflow is fatal, never padded silence");
        f.faults.released = true;
        // Keep this CPU fixture short; both cameras explicitly record a gap.
        f.take.cameraFailed(0, 71); f.take.cameraFailed(1, 72);
    }
    else if (selected == "asio-reset") f.feed(Recording::rate, 1);
    else if (selected == "asio-rate-change") f.feed(44100);
    else if (selected == "resume") f.audio.deviceDiscontinuity();
    else if (selected == "camera-disconnect")
    {
        f.take.cameraFailed(1, 999); require(!f.take.cameraDisconnected(1), "Late old-device failure discarded");
        f.take.cameraFailed(1, 72); f.feed();
        require(f.take.cameraDisconnected(1) && !f.take.cameraDisconnected(0) && f.audio.error() == RecorderAudioEngine::Error::none, "Cam2 disconnect isolates original audio and cam1");
    }
    else if (selected == "project-close" || selected == "app-exit")
    {
        f.take.requestShutdown(); require(f.take.start().failed() && f.take.prepare(f.config).failed(), "Shutdown blocks commands");
        until([&] { f.take.tick(); return f.audio.stopSample() >= 0; });
        const auto end = f.audio.acceptedEnd(); f.feed(); require(f.audio.acceptedEnd() == end, "Late callback cannot append after detach");
    }
    else if (selected == "gpu-device-removed")
    {
        PreviewRecovery recovery; std::vector<int> order;
        recovery.lost("DXGI_ERROR_DEVICE_REMOVED (0x887a0005)", 0, [&] { order.push_back(1); });
        require(!recovery.retry(499, [] { return true; }), "GPU retry backoff");
        require(recovery.retry(500, [&] { order.push_back(2); return true; }) && order == std::vector<int>{1,2}, "Old presenter joins before device/texture recreation");
        jsonSet(row, "gpuScope", "Production recovery policy with injected destroy/create; physical D3D loss UNAVAILABLE");
    }
    else if (selected == "dubbing-underrun")
    {
        PlaybackBlockQueue queue(Recording::block, 4); std::array<float, Recording::block> left{}, right{};
        require(!queue.consume(0, 1, left.data(), right.data(), Recording::block), "Missing playback block is not accepted");
        f.audio.abort(RecorderAudioEngine::Error::cancelled);
        jsonSet(row, "banner", recorderFaultText(RecorderFault::dubbingUnderrun));
    }
    if (selected == "asio-reset" || selected == "asio-rate-change" || selected == "resume")
    {
        require(std::int64_t(f.audio.deviceGeneration()) > epoch && !f.audio.clockReady(), "ASIO discontinuity creates a new epoch and requires reopen");
        require(f.audio.acceptedEnd() == before, "Changed clock block never enters original PCM");
    }
    f.finish(); f.faults.released = true;
    jsonSet(row, "take", f.take.report()); jsonSet(row, "delayBannerMs", delayMs);
    if (selected == "disk-full" || selected == "writer-stall" || selected == "audio-queue-overflow" || selected == "finalize-failure")
        require(f.faults.fired.load(), "Requested I/O fault was actually reached");
    // Read the real written native PCM prefix and compare against the source oracle.
    const auto* current = f.document.getProject().media->findTake(f.config.takeId.toString());
    require(current && !current->microphoneAssetIds.empty(), "Stopped take is placed");
    const auto* mic = f.document.getProject().media->findAsset(current->microphoneAssetIds.front());
    for (const auto& chunk : mic->chunks)
    {
        juce::MemoryBlock bytes; require(root.getChildFile(chunk.relativePath).loadFileAsData(bytes), "Read original WAV");
        require(bytes.getSize() >= 44 + std::size_t(chunk.sourceRange.length) * 3, "Available range must fit actual bytes");
        const auto* data = static_cast<const std::uint8_t*>(bytes.getData()) + 44;
        for (Sample i = 0; i < chunk.sourceRange.length; ++i)
        {
            const auto word = std::uint32_t(data[i*3]) | std::uint32_t(data[i*3+1]) << 8 | std::uint32_t(data[i*3+2]) << 16;
            require(word == (std::uint32_t(crashFixture::pcm(current->N0 + chunk.sourceRange.start + i, 0)) & 0xffffffu), "No substituted/silent original samples");
        }
    }
    lifecycle.end(RecorderLifecycle::recording); lifecycle.set(RecorderLifecycle::unsaved, true);
    require(!lifecycle.canShutdown(), "Unacknowledged edit prevents updater shutdown");
    check(f.document.performEdit("lifecycle saved edit", [](EditState& edit) { Marker marker; marker.name = "r28-kept"; marker.sample = 0; edit.markers.push_back(marker); }));
    check(f.document.saveCheckpoint(root.getChildFile("project.recorder")));
    lifecycle.end(RecorderLifecycle::unsaved);
    require(lifecycle.begin(RecorderLifecycle::exporting) && !lifecycle.begin(RecorderLifecycle::recording) && !lifecycle.canShutdown(), "Export holds exclusive gate until checkpoint/join");
    lifecycle.end(RecorderLifecycle::exporting); require(lifecycle.canShutdown(), "Idle saved project permits updater");
    jsonSet(row, "export", "UNAVAILABLE: export module absent; production exclusivity gate exercised only");
    const auto allMedia = mediaHashes(root);
    RecoveryReport first, second; check(RecoveryScanner().run(root, first));
    const auto recoveredFiles = crashFixture::hashes(root);
    check(RecoveryScanner().run(root, second));
    require(recoveredFiles == crashFixture::hashes(root), "Second recovery must not change files");
    require(RecorderSerializer::toJson(first.project) == RecorderSerializer::toJson(second.project), "Recovery project idempotence");
    require(first.project.media->findTake(completedTake) != nullptr && first.project.media->findTake(completedTake)->state == TakeState::complete, "Finished take survives");
    bool hasClip = false; for (const auto& track : first.project.tracks) for (const auto& clip : track.clips.items()) hasClip |= clip.clipId == completedClip;
    require(hasClip && first.project.markers.size() >= savedMarkers.size() + 1, "Finished clips and edits survive");
    require(allMedia == mediaHashes(root), "Recovery cannot rewrite originals");
    // A failed write may leave an old RIFF header. Playback opens the scanner's
    // repaired copy, never labels the unfinalized original as a valid WAV.
    const auto* playable = first.project.media->findAsset(mic->assetId);
    if (playable && !playable->chunks.empty())
    {
        TimelineAudioRenderer renderer(Recording::rate, Recording::block);
        auto source = RecorderSession::indexRecordedAudio(*playable, root, Recording::rate, newId());
        PlaybackAudioTrack track; track.trackId = source->trackId; PlaybackAudioClip clip; clip.source = source;
        clip.mapping.lengthSamples = playable->logicalLength; clip.mapping.mediaGeneration = playable->mediaGeneration; track.clips.push_back(clip);
        renderer.setPlan({track}, playable->logicalLength); std::array<float, Recording::block> left{}, right{};
        renderer.renderAudio(0, Recording::block, left.data(), right.data()); renderer.stopWorker();
        jsonSet(row, "playback", "PASS: production TimelineAudioRenderer / validated PCM24 WAV after recovery");
    }
    else jsonSet(row, "playback", "UNAVAILABLE: failed take has no recoverable WAV samples");
    const auto after = mediaHashes(root); for (const auto& hash : original) require(after.at(hash.first) == hash.second, "Completed original hash unchanged");
    jsonSet(row, "originalHashesUnchanged", true); jsonSet(row, "completedTakeAndEditsPreserved", true); jsonSet(row, "recoveryIdempotent", true);
    jsonSet(row, "elapsedMs", double(IoHealth::now() - started)); jsonSet(row, "status", "PASS");
    check(f.audio.closeDevice()); return row;
}
}
