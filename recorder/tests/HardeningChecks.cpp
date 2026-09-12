#include "HardeningChecks.h"
#include "AudioRenderFixtures.h"
#include "TestSupport.h"
#include "../tools/CrashFixtures.h"
#include "app/RecorderDocument.h"
#include "export/ExportController.h"
#include "export/WavExportWriter.h"
#include "playback/VideoPlaybackEngine.h"
#include "storage/EditJournal.h"
#include "ui/TimelineView.scale.h"
#include "support/Platform.h"
#include <TlHelp32.h>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace gocue::recorder::hardening
{
namespace
{
using recorder_test::require;
template<class F> void eventually(F f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!f()) { require(std::chrono::steady_clock::now() < deadline, "Hardening worker timeout"); Sleep(1); }
}
struct Resources { DWORD handles = 0, threads = 0; };
Resources resources()
{
    Resources r;
    require(GetProcessHandleCount(GetCurrentProcess(), &r.handles) != 0, "Read process handles");
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    require(snapshot != INVALID_HANDLE_VALUE, "Read process threads");
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) do { if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++r.threads; } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot); require(r.threads > 0, "Empty thread snapshot"); return r;
}
juce::var resourceReport(Resources before, Resources after)
{
    require(after.handles <= before.handles && after.threads <= before.threads, "Retained process handles or threads after worker join");
    auto r = jsonObject(); jsonSet(r,"handlesBefore",int(before.handles)); jsonSet(r,"handlesAfter",int(after.handles));
    jsonSet(r,"threadsBefore",int(before.threads)); jsonSet(r,"threadsAfter",int(after.threads));
    jsonSet(r,"retainedHandles",0); jsonSet(r,"retainedThreads",0);
    jsonSet(r,"gpuTextures","UNAVAILABLE: no GPU device/texture allocated; Claude GPU repeat required"); return r;
}
std::shared_ptr<VideoIndex> index(unsigned frames, std::int64_t offset = 100)
{
    auto s = std::make_shared<VideoIndex>(); s->epoch = std::make_shared<MediaEpoch>(); s->width = 1920; s->height = 1080;
    s->packets.reserve(frames);
    for (unsigned i = 0; i < frames; ++i) s->packets.push_back({i,i,offset + std::int64_t(i) * 65536,1,65536,i % 60 == 0,i % 60 == 0});
    s->validateAndBuild(); return s;
}
struct DecodeGate
{
    std::atomic<bool> block{true}; std::atomic<unsigned> owners{0}; PlaybackWakeEvent entered, release;
};
struct Decoder final : IVideoFrameDecoder
{
    std::shared_ptr<DecodeGate> gate;
    explicit Decoder(std::shared_ptr<DecodeGate> g) : gate(std::move(g)) { ++gate->owners; }
    ~Decoder() override { --gate->owners; }
    std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t, const std::function<bool()>&) override
    {
        if (gate->block.exchange(false)) { gate->entered.signal(); WaitForSingleObject(gate->release.nativeHandle(),10000); }
        return {}; // Deliberately returns even when cancelled; production generation fencing must reject it.
    }
};
void seekCycle(unsigned attempts)
{
    std::weak_ptr<const VideoIndex> retiredSource; std::weak_ptr<const PlaybackVideoFrame> retiredFrame;
    auto gate = std::make_shared<DecodeGate>();
    {
        auto s = index(600); retiredSource = s;
        VideoPlaybackEngine engine([gate](auto) { return std::make_unique<Decoder>(gate); });
        PlaybackVideoClip a; a.source = s; a.mapping.clipId = newId(); a.mapping.mediaGeneration = 1; a.mapping.lengthSamples = s->length;
        auto b = a; b.camera = 1; b.mapping.clipId = newId(); engine.prepare({a,b}); engine.seek(0,1);
        require(WaitForSingleObject(gate->entered.nativeHandle(),10000) == WAIT_OBJECT_0,"Blocked seek entered decoder");
        for (unsigned i = 2; i <= attempts; ++i) engine.seek(80000 + i,i);
        gate->release.signal(); const Sample target = 80000 + attempts;
        eventually([&] { return engine.ready(target,attempts); });
        for (unsigned lane = 0; lane < 2; ++lane)
        {
            const auto selection = engine.displaySelection(lane);
            require(selection.frame && selection.frame->generation == attempts
                && selection.frame->pts == target / 800,"Cancelled decode published stale source/frame");
            retiredFrame = selection.frame;
            bool submitted = false;
            require(!engine.submitIfCurrent(lane,1,[&] { submitted = true; }) && !submitted,"Cancelled seek reached presentation");
        }
        engine.stop(); require(gate->owners == 0,"Decoder owners retained after stop"); exportCheck(engine.status());
    }
    require(retiredSource.expired() && retiredFrame.expired(),"Source/frame wrappers retained after engine destruction");
}
struct WriteGate final : FileIoFaultAdapter
{
    std::mutex mutex; std::condition_variable cv; bool entered = false, release = false;
    juce::Result beforeIo(FileIoOperation op, const juce::File& file, std::uint64_t, std::size_t) override
    {
        if (op == FileIoOperation::append && file.getFileName().endsWith(".wav.partial"))
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!entered) { entered = true; cv.notify_all();
                if (!cv.wait_for(lock,std::chrono::seconds(10),[&] { return release; })) return juce::Result::fail("Hardening export release timeout"); }
        }
        return juce::Result::ok();
    }
    void unblock() { { std::lock_guard<std::mutex> lock(mutex); release = true; } cv.notify_all(); }
};
void exportCycle(const RecorderProject& p, const juce::File& source, const juce::File& destination)
{
    WriteGate gate; ExportController controller; ExportController::Request request; request.destination = destination; request.range = SampleRange{0,48000};
    exportCheck(controller.start(p,source,request,false,&gate));
    bool entered;
    { std::unique_lock<std::mutex> lock(gate.mutex); entered = gate.cv.wait_for(lock,std::chrono::seconds(10),[&] { return gate.entered; }); }
    const bool blocked = !controller.beforeRecording(); controller.cancel(); gate.unblock(); controller.wait();
    require(entered,"Real WAV export did not reach write checkpoint"); require(blocked,"Recording raced an active export writer");
    const auto state = controller.status(); require(state.state == ExportController::State::cancelled,"Export did not unwind cancellation");
    require(!state.outputDirectory.exists() && !destination.exists(),"Cancelled export was published");
    require(controller.beforeRecording(),"Export gate retained after join"); controller.endRecording();
    require(controller.activityState() == ExportActivity::State::idle,"Recording reservation retained");
}
}
juce::var scale(const juce::File& directory, std::size_t clipCount)
{
    require(clipCount >= 2 && clipCount <= 100000,"Clip count must be 2..100000"); exportCheck(directory.createDirectory());
    const auto started = juce::Time::getMillisecondCounterHiRes();
    auto stageStart = started; auto stages = jsonObject();
    const auto stage = [&](const char* name) { const auto end = juce::Time::getMillisecondCounterHiRes(); jsonSet(stages,name,end-stageStart); stageStart = end; };
    RecorderProject p; p.name = "Synthetic hardening metadata"; const Sample span = 3 * 3600 * Sample(p.Fs);
    auto media = std::make_shared<MediaRegistry>(); MediaAsset asset; asset.kind = AssetKind::importAudio;
    asset.logicalLength = span; asset.availableRanges = {{0,span}}; asset.relativePath = "media/imports/metadata-only.wav";
    asset.contentIdentity = "synthetic:metadata-only; no audio payload"; asset.originalFormat.codec = "pcm_s24le";
    asset.originalFormat.sampleRate = p.Fs; asset.originalFormat.channels = 2; asset.originalFormat.bitsPerSample = 24;
    media->assets.push_back(asset); p.media = media; Track track; track.kind = TrackKind::importAudio;
    auto& clips = track.clips.edit(); clips.reserve(clipCount);
    for (std::size_t i = 0; i < clipCount; ++i)
    { Clip c; c.trackId = track.trackId; c.assetId = asset.assetId; c.timelineStartSample = Sample(i) * span; c.lengthSamples = span; clips.push_back(c); }
    p.tracks.push_back(track); exportCheck(p.validate()); const auto text = RecorderSerializer::toJson(p);
    RecorderProject parsed; exportCheck(RecorderSerializer::fromJson(text,parsed)); require(RecorderSerializer::toJson(parsed) == text,"10k clip canonical round trip changed");
    stage("constructValidateRoundTripMs");
    const auto file = directory.getChildFile("project.recorder"); RecorderDocument document; exportCheck(document.adopt(parsed,file,{})); exportCheck(document.saveCheckpoint(file));
    const auto plan = document.renderPlanSnapshot(); require(plan->activeClips.size() == clipCount,"Long-clip render plan lost clips");
    for (std::size_t i = 0; i < clipCount; ++i)
    {
        // Every adjacent clip restarts the same source: internal boundaries get
        // the specified 3ms/144-sample fades, and the two outer edges stay exact.
        const auto& c = plan->activeClips[i];
        require(c.microfadeInSamples == (i ? 144 : 0) && c.microfadeOutSamples == (i + 1 < clipCount ? 144 : 0),"Large-plan fade attached to the wrong clip/edge");
    }
    stage("adoptAndCheckpointMs");
    TimelineVisibleIndex visible; visible.rebuild(parsed,parsed.tracks); std::size_t comparisons = 0;
    for (unsigned i = 0; i < 1000; ++i)
    {
        const auto at = Sample((std::uint64_t(i) * 909) % clipCount) * span;
        const auto found = visible.visible(0,at,at + 48000); require(found.size() == 1 && (*found.begin())->timelineStartSample == at,"Visible long clip containment"); comparisons += found.comparisons;
    }
    stage("visibleIndexAnd1000QueriesMs");
    const auto before = RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(document.getProject()));
    {
        EditJournalWorker journal(directory,document.snapshot()); journal.attach(document);
        exportCheck(document.performEdit("Large project reorder",{},[&](const RecorderProject& q)
            { return ClipEdits::reorder(q,{clips.back().clipId},ClipEdits::Placement::before,{clips.front().clipId}); }));
        exportCheck(document.undo()); require(RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(document.getProject())) == before,"Large reorder undo hash");
        exportCheck(document.redo()); exportCheck(journal.shutdown()); journal.detach();
    }
    stage("durableReorderUndoRedoMs");
    RecorderProject recovered; exportCheck(RecorderSerializer::readCheckpoint(file,recovered));
    require(RecorderSerializer::toJson(recovered) == RecorderSerializer::toJson(document.getProject()),"Large durable edit checkpoint changed");
    RecorderProject twice; exportCheck(RecorderSerializer::readCheckpoint(file,twice)); require(RecorderSerializer::toJson(twice) == RecorderSerializer::toJson(recovered),"Large checkpoint reopen not idempotent");
    stage("checkpointReopenTwiceMs");
    auto r = jsonObject(); jsonSet(r,"status","PASS"); jsonSet(r,"clipCount",Sample(clipCount)); jsonSet(r,"sourceLengthSamples",span);
    jsonSet(r,"timelineLengthSamples",p.activeTimelineEnd()); jsonSet(r,"jsonBytes",Sample(text.getNumBytesAsUTF8()));
    jsonSet(r,"visibleQueries",1000); jsonSet(r,"queryComparisons",Sample(comparisons)); jsonSet(r,"elapsedMs",juce::Time::getMillisecondCounterHiRes()-started);
    jsonSet(r,"stages",stages);
    jsonSet(r,"fadeFieldsChecked",Sample(clipCount));
    jsonSet(r,"durableReorderUndoRedo",true); jsonSet(r,"sourceKind","3-hour source metadata reused by every clip; no 3-hour media, GPU or physical timing claim"); return r;
}
juce::var largeFiles(const juce::File& directory)
{
    auto r = crashFixture::largeFaultChecks(directory); const std::uint64_t samples = (1ull << 32) + 137;
    const auto header = WavExportWriter::makeHeader(48000,samples); const auto parsed = WavExportWriter::readHeader(header.bytes.data(),header.bytes.size());
    require(header.rf64 && parsed.sampleCount == samples && parsed.dataBytes == samples * 3,"RF64 virtual count truncated");
    DurableFile file; exportCheck(file.open(directory.getChildFile("virtual-rf64-header.bin"),DurableFile::OpenMode::createNew));
    exportCheck(file.write(header.bytes.data(),header.bytes.size())); exportCheck(file.flushData()); exportCheck(file.close());
    auto s = index(3 * 3600 * 60,(1ll << 32) + 4096); const auto last = s->frameAt(s->length-1);
    require(last == s->packets.size()-1 && s->packets[last].offset > (1ll << 32) && s->previousIdr(s->length-1) <= last,"Long video index lost 64-bit offset/seek");
    jsonSet(r,"rf64SampleCount",Sample(parsed.sampleCount)); jsonSet(r,"rf64DataBytes",Sample(parsed.dataBytes)); jsonSet(r,"actualHeaderBytes",Sample(header.bytes.size()));
    jsonSet(r,"longVideoFrames",Sample(s->packets.size())); jsonSet(r,"longVideoSeconds",10800); jsonSet(r,"lastVirtualPacketOffset",s->packets.back().offset);
    jsonSet(r,"actualLargeFileWritten",false); return r;
}
juce::var cancelledSeeks(unsigned iterations)
{
    require(iterations >= 2 && iterations <= 1000000,"Seek iterations must be 2..1000000"); seekCycle(2); const auto before = resources();
    constexpr unsigned cycles = 20; for (unsigned i = 0; i < cycles; ++i) seekCycle(iterations);
    auto r = jsonObject(); jsonSet(r,"status","PASS"); jsonSet(r,"cycles",cycles); jsonSet(r,"seeksPerCycle",iterations);
    jsonSet(r,"resources",resourceReport(before,resources())); jsonSet(r,"retainedDecoderOwners",0); jsonSet(r,"retainedSourceAndFrameOwners",0);
    jsonSet(r,"sourceKind","production dual-lane playback workers; blocked decoder double, no GPU textures"); return r;
}
juce::var cancelledExports(const juce::File& directory, unsigned iterations)
{
    require(iterations > 0 && iterations <= 1000,"Export cancellation iterations must be 1..1000"); exportCheck(directory.createDirectory());
    recorder_audio_fixture::Fixture fixture; const auto originalHashes = fixture.hashes(); auto p = fixture.project;
    p.tracks.erase(p.tracks.begin(),p.tracks.begin()+2); auto media = std::make_shared<MediaRegistry>(*p.media);
    media->assets.erase(media->assets.begin(),media->assets.begin()+2); media->takes[0].cam1AssetId.clear(); media->takes[0].cam2AssetId.clear(); media->takes[0].state = TakeState::partial; p.media = media;
    exportCycle(p,fixture.root,directory.getChildFile("warmup")); const auto before = resources();
    for (unsigned i = 0; i < iterations; ++i) exportCycle(p,fixture.root,directory.getChildFile("cancel-"+juce::String(i)));
    const auto after = resources(); require(fixture.hashes() == originalHashes,"Export cancellation modified original PCM");
    require(directory.findChildFiles(juce::File::findFilesAndDirectories,true,"*.partial").isEmpty(),"Cancelled export partial retained");
    auto r = jsonObject(); jsonSet(r,"status","PASS"); jsonSet(r,"iterations",iterations); jsonSet(r,"resources",resourceReport(before,after));
    jsonSet(r,"retainedPartials",0); jsonSet(r,"originalHashesUnchanged",true);
    jsonSet(r,"sourceKind","production export controller/material WAV writes; synthetic PCM fixture; video/GPU cancellation requires Claude"); return r;
}
}
