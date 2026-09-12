#include "storage/EditJournal.h"
#include "storage/RecoveryScanner.h"
#include "storage/StorageEncoding.h"
#include <iostream>

using namespace gocue::recorder;
using namespace gocue::recorder::recovery;
namespace
{
struct Sink : IEditJournalSink
{
    EditDelta ticket;
    juce::Result enqueue(const EditDelta& d) override { ticket = d; return juce::Result::ok(); }
};
struct Fixture
{
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderR19-edit-" + newId());
    RecorderProject initial; CheckpointInfo cursor; RecorderDocument document; Sink sink;
    Fixture()
    {
        check(RecorderSerializer::writeCheckpoint(root.getChildFile("project.recorder"), initial, cursor));
        check(document.adopt(initial, root.getChildFile("project.recorder"), cursor)); document.setJournalSink(&sink);
    }
    ~Fixture() { document.setJournalSink(nullptr); }
    EditDelta rename(const char* name)
    { check(document.performEdit(name, [name](EditState& e) { e.name = name; })); return sink.ticket; }
    void appendRename(EditJournal& log, const char* name)
    { const auto ticket = rename(name); check(log.append(ticket, document.getProject())); }
};
juce::MemoryBlock bytes(const juce::File& f) { juce::MemoryBlock b; require(f.loadFileAsData(b), "Read fixture"); return b; }
void replace(const juce::File& f, const juce::MemoryBlock& b) { require(f.replaceWithData(b.getData(), b.getSize()), "Mutate isolated fixture"); }
struct FlushGate : FileIoFaultAdapter
{
    std::mutex mutex; std::condition_variable event; bool entered = false, release = false, fail = false;
    juce::Result beforeIo(FileIoOperation op, const juce::File& f, std::uint64_t, std::size_t) override
    {
        if (op == FileIoOperation::flushData && f.getFileName().startsWith("edits-"))
        {
            std::unique_lock<std::mutex> guard(mutex); entered = true; event.notify_all();
            if (!event.wait_for(guard, std::chrono::seconds(5), [&] { return release; })) return juce::Result::fail("Test flush gate timeout");
            if (fail) return juce::Result::fail("Injected journal FlushFileBuffers failure");
        }
        return juce::Result::ok();
    }
    void unblock() { const std::lock_guard<std::mutex> guard(mutex); release = true; event.notify_all(); }
    bool wait() { std::unique_lock<std::mutex> guard(mutex); return event.wait_for(guard, std::chrono::seconds(5), [&] { return entered; }); }
};
}
int runEditJournalTests()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& run)
    { try { run(); ++passed; std::cout << "PASS " << name << '\n'; } catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; } };
    test("Native edit delta roundtrip retains int64 markers, order and no snapshot commands", []
    {
        Fixture f; EditJournal log; check(log.open(f.root, f.initial, f.cursor));
        check(f.document.performEdit("markers", [](EditState& e)
        { Marker a, b; a.sample = 9007199254740993LL; a.name = "first"; b.name = "second"; e.markers = {a, b}; }));
        check(log.append(f.sink.ticket, f.document.getProject()));
        check(f.document.performEdit("order", [](EditState& e) { std::reverse(e.markers.begin(), e.markers.end()); }));
        check(log.append(f.sink.ticket, f.document.getProject())); check(log.close());
        auto result = f.initial; EditJournalReplay replay; check(EditJournal::replay(f.root, result, f.cursor, replay));
        require(!replay.framing.ignoredTail && replay.appliedEdits == 2 && RecorderSerializer::toJson(result) == RecorderSerializer::toJson(f.document.getProject()), "Delta replay differs");
        require(!replay.framing.records[0].payload.hasProperty("result"), "Full project snapshot used as edit command");
    });
    test("Undo and redo each persist a new revision despite history coalescing", []
    {
        Fixture f; EditJournal log; check(log.open(f.root, f.initial, f.cursor));
        f.appendRename(log, "one");
        f.appendRename(log, "two");
        check(f.document.undo()); check(log.append(f.sink.ticket, f.document.getProject()));
        require(f.document.getProject().editRevision == 3 && f.document.getProject().name == "one", "Undo revision not advanced");
        check(f.document.redo()); check(log.append(f.sink.ticket, f.document.getProject())); check(log.close());
        auto p = f.initial; EditJournalReplay r; check(EditJournal::replay(f.root, p, f.cursor, r));
        require(r.appliedEdits == 4 && p.editRevision == 4 && p.name == "two", "Redo lost");
    });
    for (const auto* corruption : {"tail", "crc", "sequence", "schema", "commit"}) test(corruption, [corruption]
    {
        Fixture f; EditJournal log({nullptr, {}, 128}); check(log.open(f.root, f.initial, f.cursor));
        f.appendRename(log, "saved"); f.appendRename(log, "bad");
        f.appendRename(log, "later"); check(log.close());
        const auto file = RecordingJournal::editLogFile(f.root.getChildFile("journal"), 2); auto b = bytes(file);
        auto* p = static_cast<std::uint8_t*>(b.getData()); const juce::String kind(corruption);
        if (kind == "tail") b.setSize(b.getSize() - 3);
        else if (kind == "crc") p[49] ^= 1;
        else if (kind == "commit") p[b.getSize() - 1] ^= 1;
        else
        {
            if (kind == "sequence") storageEncoding::put(p + 12, std::uint64_t(99));
            else storageEncoding::put(p + 8, std::uint16_t(99));
            storageEncoding::put(p + 44, storageEncoding::crc32(p, 44));
        }
        replace(file, b); const auto original = sha256(file); auto result = f.initial; EditJournalReplay replay;
        check(EditJournal::replay(f.root, result, f.cursor, replay));
        require(replay.framing.ignoredTail && result.name == "saved" && sha256(file) == original, "Bad tail or later segment applied");
        EditJournal reopen; require(reopen.open(f.root, result, f.cursor).failed(), "Damaged tail reopened for append");
    });
    test("Duplicate transactions span rotated segments without duplicate edits", []
    {
        Fixture f; const auto d = f.rename("once"); const auto payload = EditJournal::payload(f.initial, d, f.document.getProject());
        RecordingJournal log; check(log.openEdits(f.root.getChildFile("journal"), 1, 0, 128));
        check(log.appendEditRecord(JournalKind::EditTransaction, payload, juce::Uuid(d.transactionId)));
        check(log.appendEditRecord(JournalKind::EditTransaction, payload, juce::Uuid(d.transactionId))); check(log.close());
        auto p = f.initial; EditJournalReplay r; check(EditJournal::replay(f.root, p, f.cursor, r));
        require(r.framing.duplicateTransactions == 1 && r.framing.lastSequence == 2 && r.appliedEdits == 1, "Transaction applied twice");
    });
    test("Semantic delta checksum failure stops a structurally committed record", []
    {
        Fixture f; const auto d = f.rename("wrong"); auto payload = EditJournal::payload(f.initial, d, f.document.getProject());
        set(payload, "validationHash", "0000000000000000"); RecordingJournal log; check(log.openEdits(f.root.getChildFile("journal")));
        check(log.appendEditRecord(JournalKind::EditTransaction, payload)); check(log.close());
        auto p = f.initial; EditJournalReplay r; check(EditJournal::replay(f.root, p, f.cursor, r));
        require(r.framing.ignoredTail && p.editRevision == 0, "Invalid entity result published");
    });
    test("Two committed checkpoint generations retain backup replay before collecting logs", []
    {
        Fixture f; EditJournal log; check(log.open(f.root, f.initial, f.cursor));
        f.appendRename(log, "first"); check(log.checkpoint());
        const auto one = log.checkpointInfo(); require(log.collectedSegments() == 0, "Collected before two commits");
        f.appendRename(log, "second"); check(log.checkpoint());
        require(log.checkpointInfo().generation > one.generation && log.collectedSegments() == 1, "Generation/collection boundary");
        check(log.close()); const auto primary = f.root.getChildFile("project.recorder");
        require(primary.replaceWithText("{torn"), "Corrupt test primary"); RecorderProject p; CheckpointInfo info;
        check(RecorderSerializer::readCheckpoint(primary, p, &info)); require(info.usedBackup && p.name == "first", "Backup not retained");
        EditJournalReplay r; check(EditJournal::replay(f.root, p, info, r));
        require(!r.framing.ignoredTail && p.name == "second" && p.editRevision == 2, "Backup cannot replay after collection");
    });
    for (bool fail : {false, true}) test(fail ? "Failed journal flush stays unsaved" : "Completed gesture stays saving until owner receives flush", [fail]
    {
        Fixture f; FlushGate gate; gate.fail = fail; EditJournalWorker::Options options; options.journal.faults = &gate;
        EditJournalWorker worker(f.root, f.document.snapshot(), f.cursor, options); worker.attach(f.document); check(worker.waitUntilIdle());
        check(f.document.performEdit("pending", [](EditState& e) { e.name = "pending"; })); f.document.endGesture();
        const bool waiting = gate.wait(); const bool saving = f.document.isDirty() && f.document.durableRevision() == 0;
        gate.unblock(); const auto done = worker.waitUntilIdle(); worker.drain();
        require(waiting && saving, "Gesture hidden before durable revision");
        require(fail ? done.failed() && f.document.isDirty() && f.document.getError().isNotEmpty()
                     : done.wasOk() && !f.document.isDirty() && f.document.durableRevision() == 1, "Flush acknowledgement state wrong");
        const auto closed = worker.shutdown(); worker.detach(); require(closed.failed() == fail, "Shutdown lost storage error");
    });
    test("Automatic checkpoint worker and normal shutdown use latest durable snapshot", []
    {
        Fixture f; std::mutex mutex; std::condition_variable event; bool checkpointed = false;
        EditJournalWorker::Options options; require(options.checkpointInterval.count() == 10000, "Production interval changed");
        options.checkpointInterval = std::chrono::milliseconds(30);
        options.journal.hook = [&](const char* stage) { if (juce::String(stage) == "checkpoint-after-commit") { const std::lock_guard<std::mutex> guard(mutex); checkpointed = true; event.notify_all(); } };
        EditJournalWorker worker(f.root, f.document.snapshot(), f.cursor, options); worker.attach(f.document); check(worker.waitUntilIdle());
        check(f.document.performEdit("auto", [](EditState& e) { e.name = "auto"; }));
        bool reached; { std::unique_lock<std::mutex> guard(mutex); reached = event.wait_for(guard, std::chrono::seconds(5), [&] { return checkpointed; }); }
        check(worker.waitUntilIdle()); worker.drain(); check(worker.shutdown()); worker.detach();
        RecorderProject saved; check(RecorderSerializer::readCheckpoint(f.root.getChildFile("project.recorder"), saved));
        require(reached && saved.name == "auto" && !f.document.isDirty(), "Automatic durable checkpoint missing");
    });
    test("Project writer lock refuses a concurrent journal worker", []
    {
        Fixture f; EditJournal a, b; check(a.open(f.root, f.initial, f.cursor)); require(b.open(f.root, f.initial, f.cursor).failed(), "Second writer acquired project"); check(a.close());
    });
    for (const auto* stage : {"checkpoint-backup-flushed", "checkpoint-verified-before-replace", "checkpoint-after-replace", "checkpoint-before-commit", "checkpoint-after-commit", "checkpoint-before-collect"})
        test(stage, [stage]
    {
        Fixture f; bool armed = false, reached = false;
        EditJournal log({nullptr, [&](const char* s) { if (armed && juce::String(stage) == s) { reached = true; throw std::runtime_error("Checkpoint interruption"); } }});
        check(log.open(f.root, f.initial, f.cursor)); f.appendRename(log, "first"); check(log.checkpoint());
        f.appendRename(log, "durable second"); armed = true; require(log.checkpoint().failed(), "Checkpoint hook did not interrupt"); log.close();
        RecoveryReport first, second; check(RecoveryScanner().run(f.root, first)); check(RecoveryScanner().run(f.root, second));
        require(reached && first.project.name == "durable second" && RecorderSerializer::toJson(first.project) == RecorderSerializer::toJson(second.project)
            && second.replayedEdits == 0 && second.changedTakes == 0, "Checkpoint crash lost durable edit or repeated recovery");
    });
    test("Provisional rate adoption rescales markers in the same registry record and replays atomically", []
    {
        Fixture f; EditJournalWorker worker(f.root, f.document.snapshot(), f.cursor); worker.attach(f.document); check(worker.waitUntilIdle());
        Marker m; m.sample = 4800; check(f.document.addMarker(m)); check(worker.waitUntilIdle());
        check(f.document.adoptProvisionalTimebase(44100)); check(worker.waitUntilIdle());
        require(f.document.getProject().Fs == 44100 && f.document.getProject().markers[0].sample == 4410 && f.document.isDirty(), "Adopted rate with the marker kept at 0.1 s");
        check(f.document.performEdit("after", [](EditState& e) { e.name = "after"; })); check(worker.waitUntilIdle());
        check(worker.shutdown()); worker.detach();
        RecorderProject p = f.initial; EditJournalReplay r; check(EditJournal::replay(f.root, p, f.cursor, r));
        require(!r.framing.ignoredTail && p.Fs == 44100 && p.markers.size() == 1 && p.markers[0].sample == 4410 && p.name == "after", "Replay applies time base + markers in one registry record, then the later edit");
    });
    test("Explicit document Save queues a checkpoint behind edits and timebase registry", []
    {
        Fixture f; EditJournalWorker worker(f.root, f.document.snapshot(), f.cursor); worker.attach(f.document); check(worker.waitUntilIdle());
        check(f.document.setTimebase(44100, {30, 1}));
        check(f.document.performEdit("save", [](EditState& e) { e.name = "explicit"; }));
        check(f.document.saveCheckpoint(f.root.getChildFile("project.recorder"))); require(!f.document.isDirty(), "Save acknowledged before queued edits");
        check(worker.shutdown()); worker.detach(); RecorderProject p; check(RecorderSerializer::readCheckpoint(f.root.getChildFile("project.recorder"), p));
        require(p.name == "explicit" && p.Fs == 44100 && p.editRevision == 1, "Save did not persist current durable state");
    });
    std::cout << "EditJournalTests: " << passed << " passed, " << failed << " failed\n"; return failed ? 1 : 0;
}
