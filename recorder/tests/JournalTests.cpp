#include "storage/RecordingJournal.h"
#include "storage/StorageEncoding.h"
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace gocue::recorder;
namespace
{
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void success(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
juce::File fixture()
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderR06-journal-" + juce::Uuid().toString());
    success(dir.createDirectory()); return dir; // Retain isolated fixtures for failure inspection.
}
JournalTakeStarted startRecord()
{
    JournalTakeStarted s;
    s.takeId = juce::Uuid("11111111-1111-4111-8111-111111111111");
    s.n0 = 9007199254740993LL; s.o0 = s.n0 + 240; s.pstart = -101; s.usesOutputOrigin = true;
    s.files = {{"22222222-2222-4222-8222-222222222222", "media/takes/t/audio/mic01/000001.wav", "media/takes/t/audio/mic01/{chunk}.wav"}};
    s.devices = {{"fixture-device", juce::String::fromUTF8("마이크 1"), 1, 0, 7}};
    return s;
}
JournalCheckpoint checkpointRecord()
{
    const auto s = startRecord();
    return {s.takeId, {{s.files[0].path, 144044, 48000, 0, 48000, 1, 48000, 44, 3}}};
}
juce::MemoryBlock bytes(const juce::File& f)
{
    juce::MemoryBlock data; require(f.loadFileAsData(data), "Read fixture bytes"); return data;
}
void replace(const juce::File& f, const void* p, std::size_t n) { require(f.replaceWithData(p, n), "Write fixture bytes"); }
struct FlushFault : FileIoFaultAdapter
{
    bool failFlush = false;
    juce::Result beforeIo(FileIoOperation op, const juce::File&, std::uint64_t, std::size_t) override
    { return failFlush && op == FileIoOperation::flushData ? juce::Result::fail("Injected FlushFileBuffers failure") : juce::Result::ok(); }
};
}
int runJournalTests()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& body)
    {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("DurableFile append/patch/app flush/OS watermark and reopen", []
    {
        const auto path = fixture().getChildFile("append.bin"); FlushFault fault; DurableFile file(&fault);
        success(file.open(path, DurableFile::OpenMode::createNew)); success(file.write("abc", 3));
        success(file.flushApplicationBuffers()); require(file.writtenBytes() == 3 && file.durableBytes() == 0, "App flush must not advance durable watermark");
        success(file.flushData()); require(file.durableBytes() == 3, "Successful OS flush advances watermark");
        success(file.write("def", 3)); success(file.writeAt(0, "A", 1));
        require(file.writtenBytes() == 6 && file.durableBytes() == 3, "Patch does not move append position/watermark");
        fault.failFlush = true;
        require(file.flushData().failed() && file.durableBytes() == 3, "Failed OS flush preserves last watermark");
        require(file.write("g", 1).failed() && file.close().failed(), "Failure is latched, explicit close propagates it");
        require(path.loadFileAsString() == "Abcdef", "No truncation, append follows header patch");
        fault.failFlush = false; success(file.open(path));
        require(file.writtenBytes() == 6 && file.durableBytes() == 0, "Reopen never assumes prior data is durable");
        success(file.flushData()); require(file.durableBytes() == 6, "Reopen flush captures actual length"); success(file.close());
        require(file.open(path, DurableFile::OpenMode::createNew).failed(), "CREATE_NEW preserves existing original");
        require(path.loadFileAsString() == "Abcdef", "Existing bytes unchanged");
    });
    test("DurableFile real Win32 open error and patch bounds", []
    {
        DurableFile f;
        require(f.open(fixture().getChildFile("missing/no.bin")).failed(), "Win32 missing-parent error propagates");
        success(f.open(fixture().getChildFile("bounds.bin"))); success(f.write("123", 3)); success(f.flushData());
        require(f.writeAt(3, "4", 1).failed() && f.durableBytes() == 3, "Header patch cannot extend or claim durability");
    });
    test("IEEE CRC32 known answer and all record kinds roundtrip", []
    {
        require(storageEncoding::crc32("123456789", 9) == 0xcbf43926u, "IEEE CRC32 golden value");
        const auto dir = fixture(); RecordingJournal journal; const auto s = startRecord(); const juce::Uuid edit;
        success(journal.open(dir)); success(journal.append(s)); success(journal.append(checkpointRecord()));
        success(journal.append(JournalTakeStopped{s.takeId, s.n0 + 48000, edit})); success(journal.append(JournalTakeFinalized{s.takeId}));
        require(journal.durableSequence() == 4, "Every acknowledged record has journal OS flush"); success(journal.close());
        JournalReplay read; success(RecordingJournal::replay(dir, read));
        require(!read.ignoredTail && read.records.size() == 4 && read.lastSequence == 4, "Four valid commits");
        require(static_cast<juce::int64>(read.records[0].payload["N0"]) == s.n0, "int64 sample origin above 2^53 preserved");
        require(static_cast<juce::int64>(read.records[0].payload["Pstart"]) == -101, "Signed placement preserved");
        require(read.records[0].payload["files"][0]["plannedPath"].toString() == s.files[0].plannedPath, "File list and planned chunk path");
        require(read.records[0].payload["devices"][0]["name"].toString() == s.devices[0].name, "UTF-8 device mapping");
        require(static_cast<int>(read.records[0].payload["pcm"]["bitsPerSample"]) == 24, "PCM format saved");
        require(static_cast<juce::int64>(read.records[1].payload["files"][0]["validBytes"]) == 144044, "Durable position saved");
        require(read.records[2].payload["placementEditId"].toString() == edit.toDashedString(), "Stop/edit linkage retained");
        require(read.records[3].kind == JournalKind::TakeFinalized, "Finalization is separate");
    });
    test("Every truncated byte of last record replays only the prior commit", []
    {
        const auto dir = fixture(); RecordingJournal journal; success(journal.open(dir)); success(journal.append(startRecord()));
        success(journal.append(checkpointRecord())); success(journal.close());
        const auto file = RecordingJournal::logFile(dir, 1); const auto all = bytes(file);
        const auto* data = static_cast<const std::uint8_t*>(all.getData()); const auto first = storageEncoding::get<std::uint32_t>(data + 4);
        for (std::size_t cut = first; cut < all.getSize(); ++cut)
        {
            replace(file, data, cut); JournalReplay read; success(RecordingJournal::replay(dir, read));
            require(read.records.size() == 1 && read.lastSequence == 1, "Truncated tail must never apply incomplete record");
            require(read.ignoredTail == (cut != first), "Exact prior commit boundary is a clean end");
        }
    });
    test("CRC corruption stops subsequent valid records; original is preserved on reopen", []
    {
        const auto dir = fixture(); RecordingJournal journal; success(journal.open(dir)); success(journal.append(startRecord()));
        success(journal.append(checkpointRecord())); success(journal.append(JournalTakeFinalized{startRecord().takeId})); success(journal.close());
        const auto file = RecordingJournal::logFile(dir, 1); auto all = bytes(file); auto* data = static_cast<std::uint8_t*>(all.getData());
        const auto first = storageEncoding::get<std::uint32_t>(data + 4); data[first + RecordingJournal::headerBytes + 3] ^= 1;
        replace(file, data, all.getSize()); JournalReplay read; success(RecordingJournal::replay(dir, read));
        require(read.ignoredTail && read.records.size() == 1 && read.tailReason.contains("CRC"), "CRC corruption cuts off all later records");
        require(journal.open(dir).failed(), "Writer refuses to append behind corrupt tail");
        require(bytes(file) == all, "Recovery never truncates/rewrites originals");
    });
    test("Header CRC, sequence, future schema and commit marker are enforced", []
    {
        const auto dir = fixture(); RecordingJournal j; success(j.open(dir)); success(j.append(startRecord())); success(j.close());
        const auto file = RecordingJournal::logFile(dir, 1); const auto all = bytes(file);
        for (unsigned variant = 0; variant < 4; ++variant)
        {
            auto corrupt = all; auto* data = static_cast<std::uint8_t*>(corrupt.getData());
            if (variant == 0) data[20] ^= 1; // UUID corruption is covered by header CRC.
            if (variant == 1) storageEncoding::put(data + 12, std::uint64_t{2});
            if (variant == 2) storageEncoding::put(data + 8, std::uint16_t{2});
            if (variant == 1 || variant == 2) storageEncoding::put(data + 44, storageEncoding::crc32(data, 44));
            if (variant == 3) data[corrupt.getSize() - 1] ^= 1;
            replace(file, data, corrupt.getSize()); JournalReplay read; success(RecordingJournal::replay(dir, read));
            require(read.ignoredTail && read.records.empty(), "Corrupt metadata/commit is not applied");
        }
    });
    test("Duplicate transactions across rotations are ignored while physical sequence advances", []
    {
        const auto dir = fixture(); RecordingJournal j; const juce::Uuid txn;
        success(j.open(dir, 56)); success(j.append(startRecord(), txn)); success(j.append(startRecord(), txn)); success(j.append(checkpointRecord()));
        success(j.close()); JournalReplay read; success(RecordingJournal::replay(dir, read));
        require(read.fileCount == 3 && read.lastSequence == 3 && read.records.size() == 2 && read.duplicateTransactions == 1, "Dedupe spans log files");
        require(RecordingJournal::logFile(dir, 1).getFileName() == "takes-000001.log", "Six digit numbering starts at one");
        success(j.open(dir, 56)); success(j.append(JournalTakeFinalized{startRecord().takeId})); success(j.close());
        success(RecordingJournal::replay(dir, read));
        require(read.fileCount == 4 && read.lastSequence == 4 && read.records.back().sequence == 4, "Reopen continues sequence and rotation");
    });
    test("A damaged earlier rotation stops all later segments and a missing segment is detected", []
    {
        const auto dir = fixture(); RecordingJournal j; success(j.open(dir, 56)); success(j.append(startRecord()));
        success(j.append(checkpointRecord())); success(j.append(JournalTakeFinalized{startRecord().takeId})); success(j.close());
        const auto second = RecordingJournal::logFile(dir, 2); auto data = bytes(second);
        replace(second, data.getData(), data.getSize() - 1); JournalReplay read; success(RecordingJournal::replay(dir, read));
        require(read.ignoredTail && read.records.size() == 1, "Tail stops replay across rotation");
        require(second.moveFileTo(dir.getChildFile("preserved-second.bad")), "Preserve fixture while simulating missing segment");
        success(RecordingJournal::replay(dir, read)); require(read.ignoredTail && read.tailReason.contains("gap"), "Missing journal segment detected");
    });
    test("Journal flush failure never acknowledges sequence and directory has a single writer", []
    {
        const auto dir = fixture(); FlushFault fault; RecordingJournal j(&fault), other;
        success(j.open(dir)); require(other.open(dir).failed(), "Second writer lock rejected");
        fault.failFlush = true; const auto flushed = j.append(startRecord());
        require(flushed.failed() && flushed.getErrorMessage().contains("Injected") && j.durableSequence() == 0, "No durable acknowledgement on injected flush failure");
        require(j.append(checkpointRecord()).failed() && j.close().failed(), "Journal write error remains visible");
    });
    test("Unsafe paths and missing PCM/device fields are rejected before append", []
    {
        const auto dir = fixture(); RecordingJournal j; success(j.open(dir)); auto s = startRecord(); s.files[0].path = "../outside.wav";
        require(j.append(s).failed() && j.durableSequence() == 0, "Relative path traversal rejected");
        j.close(); success(j.open(dir)); s = startRecord(); s.pcm.bitsPerSample = 32;
        require(j.append(s).failed(), "Wrong PCM format rejected");
    });
    std::cout << "Journal/durable tests: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}

int runWavChunkTests();
int recorderRound01TestMain(int, char**);
// CMake renames only the existing TestMain.cpp entry point. No shared runner edit.
int main(int argc, char** argv)
{
    try
    {
        if (argc == 1)
        {
            const auto journal = runJournalTests(); const auto wav = runWavChunkTests();
            const auto capture = recorderRound01TestMain(argc, argv); return journal || wav || capture ? 1 : 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--suite")
        {
            const std::string suite(argv[2]);
            if (suite == "journal-durable") { const auto journal = runJournalTests(); const auto wav = runWavChunkTests(); return journal || wav ? 1 : 0; }
            if (suite == "wav-chunks") return runWavChunkTests();
            // Preserve suites added to the original runner by parallel rounds.
            return recorderRound01TestMain(argc, argv);
        }
        std::cerr << "RecorderTests [--suite capture-contract|journal-durable|wav-chunks]\n"; return 2;
    }
    catch (const std::exception& e) { std::cerr << "Unhandled storage test exception: " << e.what() << '\n'; return 1; }
}
