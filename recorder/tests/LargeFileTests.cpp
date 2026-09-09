#include "CrashFixtures.h"
#include <iostream>

using namespace gocue::recorder;
using namespace gocue::recorder::recovery;
namespace
{
struct DiskFull : FileIoFaultAdapter
{
    FileIoOperation operation = FileIoOperation::append; std::atomic<bool> enabled{false}; juce::String extension;
    juce::Result beforeIo(FileIoOperation op, const juce::File& path, std::uint64_t, std::size_t) override
    { return enabled && op == operation && (extension.isEmpty() || path.hasFileExtension(extension)) ? juce::Result::fail("Injected ERROR_DISK_FULL (Win32 112)") : juce::Result::ok(); }
};
}
int runLargeFileTests()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& run)
    { try { run(); ++passed; std::cout << "PASS " << name << '\n'; } catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; } };
    test("Virtual 4GiB boundary and Win32 disk-full without large allocation", []
    { const auto result = crashFixture::largeFaultChecks(crashFixture::directory("large")); require(result["status"].toString() == "PASS", "Virtual I/O adapter failed"); });
    test("Take journal preserves >4GiB offsets and >2^53 sample coordinates", []
    {
        const auto root = crashFixture::directory("large-journal"); RecordingJournal journal; check(journal.open(root)); JournalCheckpoint checkpoint;
        checkpoint.files.push_back({"media/takes/large.wav", (1ull << 32) + 99, 1234567890, 9007199254740993ull, 9007199254740994LL, 1, 48000, 44, 3});
        check(journal.append(checkpoint)); check(journal.close()); JournalReplay replay; check(RecordingJournal::replay(root, replay));
        const auto f = (*replay.records[0].payload["files"].getArray())[0]; require(number(f["validBytes"]) == (1ll << 32) + 99 && number(f["firstSample"]) == 9007199254740993LL, "64-bit journal coordinates rounded/narrowed");
        require(RecordingJournal::logFile(root, 1).getSize() < 4096, "Test created large physical file");
    });
    for (const auto operation : {FileIoOperation::append, FileIoOperation::patch, FileIoOperation::flushData}) test("WAV worker propagates disk-full and keeps last durable samples", [operation]
    {
        const auto root = crashFixture::directory("wav-full"); auto c = crashFixture::wavConfig(root, 1); DiskFull fault; fault.extension = "wav"; fault.operation = operation; c.faults = &fault;
        WavTrackWriter writer(c); check(writer.start()); fault.enabled = true;
        crashFixture::push(writer, 0, 1600, 1); const auto r = writer.stop(1600, juce::Uuid());
        require(r.failed() && r.getErrorMessage().contains("112") && writer.error() == WavTrackWriter::Error::io && writer.journalDurableSamples() == 0, "Disk full was reported as durable success");
        JournalReplay replay; check(RecordingJournal::replay(root.getChildFile("journal"), replay)); for (const auto& record : replay.records) require(record.kind != JournalKind::TakeFinalized, "Failed media was finalized");
    });
    test("Recovery copy disk-full never commits/adopts or modifies original", []
    {
        const auto root = crashFixture::directory("recovery-full"); RecorderProject initial; RecoveryScanner::writeCheckpoint(root.getChildFile("project.recorder"), initial);
        auto c = crashFixture::wavConfig(root, 1); WavTrackWriter writer(c); check(writer.start()); for (unsigned i = 0; i < 30; ++i) crashFixture::push(writer, std::uint64_t(i) * 1600, 1600, 1); check(writer.stop(48000, juce::Uuid()));
        const auto before = crashFixture::hashes(root, true); DiskFull fault; fault.enabled = true; fault.extension = "wav";
        RecorderDocument document; check(document.adopt(initial, root.getChildFile("project.recorder"), {})); RecoveryReport report;
        require(RecoveryScanner({&fault, {}}).run(root, report, &document).failed() && report.warnings.joinIntoString(" ").contains("112"), "Recovery disk-full not propagated");
        require(document.getProject().media->takes.empty() && before == crashFixture::hashes(root, true), "Failed recovery adopted or altered source");
        require(!report.attempt.getChildFile("commit.log").existsAsFile(), "Disk-full recovery committed");
        RecoveryReport retry; check(RecoveryScanner().run(root, retry)); require(retry.changedTakes == 1, "Recovery retry failed");
    });
    test("Checkpoint flush failure keeps old primary and backup", []
    {
        const auto root = crashFixture::directory("checkpoint-full"); RecorderProject old; const auto file = root.getChildFile("project.recorder"); RecoveryScanner::writeCheckpoint(file, old);
        const auto hash = sha256(file); auto next = old; next.editRevision = 1; DiskFull fault; fault.operation = FileIoOperation::flushData; fault.enabled = true;
        bool rejected = false; try { RecoveryScanner::writeCheckpoint(file, next, &fault); } catch (const std::exception& e) { rejected = std::string(e.what()).find("112") != std::string::npos; }
        require(rejected && sha256(file) == hash, "Failed checkpoint flush replaced primary");
    });
    test("Disk-full edit journal has no visible committed revision", []
    {
        const auto root = crashFixture::directory("edit-full"); RecorderProject initial; RecoveryScanner::writeCheckpoint(root.getChildFile("project.recorder"), initial);
        RecorderDocument doc; check(doc.adopt(initial, root.getChildFile("project.recorder"), {})); crashFixture::TicketSink sink; doc.setJournalSink(&sink);
        check(doc.performEdit("unsaved", [](EditState& e) { e.name = "unsaved"; })); DiskFull fault; fault.enabled = true; bool rejected = false;
        try { RecoveryScanner::appendEdit(root.getChildFile("journal/edits-000001.log"), sink.ticket, doc.getProject(), &fault); } catch (...) { rejected = true; }
        RecoveryReport report; check(RecoveryScanner().run(root, report)); require(rejected && report.project.editRevision == 0 && report.project.name == initial.name, "Failed edit became saved");
    });
    test("MP4 recovery output disk-full is an I/O error, not a camera gap", []
    {
        av_log_set_level(AV_LOG_ERROR); const auto root = crashFixture::directory("mp4-full"); const auto source = root.getChildFile("cam1.mp4");
        { crashFixture::Camera camera(source); for (int i = 0; i < 31; ++i) camera.frame(); camera.finish(); }
        const auto hash = sha256(source); DiskFull fault; fault.enabled = true; fault.operation = FileIoOperation::flushData; bool rejected = false;
        try { Mp4RecoveryIndex::recover(source, Mp4RecoveryIndex::pathFor(source), root.getChildFile("output.mp4"), 48000, &fault); }
        catch (const OutputError& e) { rejected = std::string(e.what()).find("112") != std::string::npos; }
        require(rejected && sha256(source) == hash, "Recovery MP4 output error was swallowed");
    });
    std::cout << "LargeFileTests: " << passed << " passed, " << failed << " failed\n"; return failed ? 1 : 0;
}
