#include "export/ExportController.h"
#include "export/WavExportWriter.h"
#include "app/RecorderDocument.h"
#include "AudioRenderFixtures.h"
#include "TestSupport.h"
#include "support/Platform.h"
#include <condition_variable>
#include <chrono>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
struct Fault final : FileIoFaultAdapter
{
    FileIoOperation operation = FileIoOperation::append;
    bool manifestOnly = false;
    juce::Result beforeIo(FileIoOperation op, const juce::File& file, std::uint64_t, std::size_t) override
    { return op == operation && (!manifestOnly || file.getFileName().startsWith("export-manifest"))
        ? juce::Result::fail("Injected disk write/flush failure") : juce::Result::ok(); }
};
RecorderProject audioProject(const recorder_audio_fixture::Fixture& f)
{
    auto p = f.project; p.tracks.erase(p.tracks.begin(), p.tracks.begin() + 2);
    auto registry = std::make_shared<MediaRegistry>(*p.media);
    registry->assets.erase(registry->assets.begin(), registry->assets.begin() + 2);
    registry->takes[0].cam1AssetId.clear(); registry->takes[0].cam2AssetId.clear();
    registry->takes[0].state = TakeState::partial; // model's recovered microphone-only take contract
    p.media = registry; return p;
}
ExportController::Request request(const juce::File& root)
{ ExportController::Request r; r.destination = root.getChildFile("delivery"); r.range = SampleRange{0, 4800}; return r; }
bool hasPartial(const juce::File& root)
{ return !root.findChildFiles(juce::File::findFilesAndDirectories, true, "*.partial").isEmpty(); }
}
int runExportLifecycleTests()
{
    Suite s;
    s.test("Collision suffix handles files/directories and retry never overwrites completed outputs", []
    {
        recorder_audio_fixture::Fixture f; const auto p = audioProject(f); auto r = request(f.root);
        exportCheck(r.destination.createDirectory()); const auto original = r.destination.getChildFile("keep.txt"); require(original.replaceWithText("completed-original"), "Seed completed file");
        require(r.destination.getSiblingFile("delivery (2)").replaceWithText("also-complete"), "Seed file collision");
        ExportController c; exportCheck(c.start(p, f.root, r)); c.wait(); const auto done = c.status();
        require(done.state == ExportController::State::completed && done.outputDirectory.getFileName() == "delivery (3)", "Readable suffix collision resolution");
        require(original.loadFileAsString() == "completed-original" && r.destination.getSiblingFile("delivery (2)").loadFileAsString() == "also-complete", "Existing outputs untouched");
        auto manifest = juce::JSON::parse(done.outputDirectory.getChildFile("export-manifest.json"));
        require(manifest["files"].size() == 2 && Sample(manifest["range"]["sampleCount"]) == 4800 && manifest["editRevision"] == juce::var(p.editRevision), "Committed revision/common range");
        for (const auto& file : *manifest["files"].getArray())
        {
            const auto header = WavExportWriter::inspect(done.outputDirectory.getChildFile(file["name"].toString()));
            require(header.sampleCount == 4800 && file["verified"] == juce::var(true) && file["assetIds"].size() == 1, "Manifest matches real verified WAV");
        }
        c.cancel(); require(done.outputDirectory.exists() && !hasPartial(f.root), "Late cancel preserves completed publication");
        require(c.retry().failed(), "Cannot retry completed job");
    });
    s.test("Cancellation waits for a quiescent worker before reserving recording", []
    {
        recorder_audio_fixture::Fixture f; const auto p = audioProject(f);
        std::mutex mutex; std::condition_variable cv; bool entered = false, release = false; juce::File partial;
        ExportController c([&](const ExportJob& j, const ExportController::Request&, ExportControl& control, FileIoFaultAdapter*)
        {
            ExportActivity::Lease lease(control.activity); ExportPublication publication(j); partial = j.partialDirectory;
            { std::lock_guard<std::mutex> lock(mutex); entered = true; } cv.notify_all();
            { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return release; }); }
            control.checkpoint(); return juce::var();
        });
        exportCheck(c.start(p, f.root, request(f.root)));
        { std::unique_lock<std::mutex> lock(mutex); require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }), "Worker checkpoint reached"); }
        const bool blocked = !c.beforeRecording() && c.activityState() == ExportActivity::State::exporting && partial.isDirectory();
        const bool secondRejected = c.start(p, f.root, request(f.root)).failed();
        { std::lock_guard<std::mutex> lock(mutex); release = true; } cv.notify_all(); c.wait();
        require(blocked, "Recording cannot pass a busy checkpoint"); require(secondRejected, "Second export cannot race pending cancel");
        require(c.status().state == ExportController::State::cancelled && !partial.exists(), "Only owned partial removed after unwind");
        require(c.beforeRecording() && c.activityState() == ExportActivity::State::recording, "Recording reserves shared gate after join");
        require(c.start(p, f.root, request(f.root)).failed(), "Export rejected during recording reservation"); c.endRecording();
        require(c.activityState() == ExportActivity::State::idle, "Failed/completed recording releases gate");
    });
    s.test("Write and manifest flush failures clean partials; retry uses immutable revision", []
    {
        for (bool manifest : {false, true})
        {
            recorder_audio_fixture::Fixture f; auto p = audioProject(f); auto r = request(f.root); const auto hashes = f.hashes();
            Fault fault; fault.manifestOnly = manifest; fault.operation = manifest ? FileIoOperation::flushData : FileIoOperation::append;
            ExportController c; exportCheck(c.start(p, f.root, r, false, &fault)); c.wait();
            require(c.status().state == ExportController::State::failed && !r.destination.exists() && !hasPartial(f.root), "Failed job never publishes");
            ++p.editRevision; p.tracks[0].clips.edit().clear();
            exportCheck(c.retry()); c.wait(); const auto done = c.status();
            require(done.state == ExportController::State::completed && Sample(done.manifest["editRevision"]) == 14, "Retry retains captured revision");
            require(f.hashes() == hashes, "Original PCM bytes unchanged by failure/retry");
        }
    });
    s.test("Document recording lock rejects before job creation", []
    {
        recorder_audio_fixture::Fixture f; RecorderDocument d; exportCheck(d.saveCheckpoint(f.root.getChildFile("empty.recorder")));
        d.setRecordingStructureLock(true); ExportController c; require(c.start(d, request(f.root)).failed(), "Locked document blocks export");
        require(c.activityState() == ExportActivity::State::idle && !request(f.root).destination.exists(), "Rejected start owns no activity/output"); d.setRecordingStructureLock(false);
    });
    s.test("Publication collision after snapshot and forged verification never replace targets", []
    {
        recorder_audio_fixture::Fixture f; auto p = audioProject(f); ExportActivity gate; ExportControl control(gate);
        ExportJob j(p, f.root, f.root.getChildFile("collision"), SampleRange{0, 1600});
        exportCheck(j.outputDirectory.createDirectory()); rejects([&] { ExportPublication publication(j); });
        require(j.outputDirectory.isDirectory() && !j.partialDirectory.exists(), "Racing completed directory preserved");
        ExportJob missing(p, f.root, f.root.getChildFile("missing"), SampleRange{0, 1600});
        { ExportPublication publication(missing); auto row = jsonObject(); jsonSet(row, "name", "mic01.wav"); jsonSet(row, "verified", true); juce::Array<juce::var> files{row}; rejects([&] { publication.commit(files, control); }); }
        require(!missing.outputDirectory.exists() && !missing.partialDirectory.exists(), "Missing verified payload cannot commit");
    });
    s.test("Parent path failure reports and changed-folder retry succeeds", []
    {
        recorder_audio_fixture::Fixture f; const auto p = audioProject(f); auto r = request(f.root); const auto blocker = f.root.getChildFile("not-a-directory");
        require(blocker.replaceWithText("preserve"), "Seed parent path failure"); r.destination = blocker.getChildFile("export");
        ExportController c; exportCheck(c.start(p, f.root, r)); c.wait(); require(c.status().state == ExportController::State::failed, "Native create failure surfaced");
        exportCheck(c.retry(f.root.getChildFile("retry-folder"))); c.wait(); require(c.status().state == ExportController::State::completed && blocker.loadFileAsString() == "preserve", "Changed folder retry preserves blocker");
    });
    return s.result("ExportLifecycleTests");
}
