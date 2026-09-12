#include "CrashFixtures.h"
#include "storage/EditJournal.h"
#include <iostream>

using namespace gocue::recorder;
using namespace gocue::recorder::recovery;
namespace
{
juce::String editHash(const RecorderProject& p) { return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)); }
struct Fixture
{
    juce::File root = crashFixture::directory("r19-recovery"); RecoveryReport base; RecorderDocument doc; crashFixture::TicketSink sink;
    Fixture()
    {
        crashFixture::baseline(root); check(RecoveryScanner().run(root, base));
        check(doc.adopt(base.project, root.getChildFile("project.recorder"), base.checkpointInfo)); doc.setJournalSink(&sink);
    }
    ~Fixture() { doc.setJournalSink(nullptr); }
};
void twice(Fixture& f, const RecorderProject& expected)
{
    const auto before = crashFixture::hashes(f.root, true); RecoveryReport first, second; RecorderDocument adopted;
    check(RecoveryScanner().run(f.root, first, &adopted)); const auto all = crashFixture::hashes(f.root);
    check(RecoveryScanner().run(f.root, second));
    require(editHash(first.project) == editHash(expected) && editHash(adopted.getProject()) == editHash(expected), "Active clips/mute/solo/markers/version not restored");
    require(adopted.getHistory().undoDepth() == 0, "Recovery restored an undo command stack");
    require(RecorderSerializer::toJson(first.project) == RecorderSerializer::toJson(second.project)
        && second.changedTakes == 0 && second.addedClips == 0 && all == crashFixture::hashes(f.root), "Second recovery changed files/model/clips");
    require(before == crashFixture::hashes(f.root, true), "Original bytes changed during recovery");
}
void addVersions(RecorderDocument& doc)
{
    check(doc.performEdit("fixture versions", [](EditState& e)
    {
        e.linkGroups.clear(); TakeStack stack; stack.spanSamples = 48000; TakeVersion a, b;
        auto& clips = e.tracks.front().clips.edit(); auto first = clips.front(), second = first;
        first.linkGroupId.clear(); second.linkGroupId.clear(); first.takeStackId = second.takeStackId = stack.stackId;
        first.versionId = a.versionId; second.versionId = b.versionId; second.clipId = newId(); second.lengthSamples = 24000;
        a.clipIds = {first.clipId}; b.clipIds = {second.clipId}; clips = {first, second};
        stack.versions = {a, b}; stack.activeVersionId = a.versionId; e.takeStacks = {stack};
    }));
}
}
int runRetakeRecoveryTests()
{
    av_log_set_level(AV_LOG_ERROR); unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& run)
    { try { run(); ++passed; std::cout << "PASS " << name << '\n'; } catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; } };
    for (const auto* action : {"edit", "undo", "retake"}) for (bool committed : {false, true})
    {
        const auto label = juce::String(action) + (committed ? " after commit" : " during append");
        test(label.toRawUTF8(), [action, committed]
        {
            Fixture f; EditJournal preparation; check(preparation.open(f.root, f.base.project, f.base.checkpointInfo));
            addVersions(f.doc); check(preparation.append(f.sink.ticket, f.doc.getProject()));
            check(f.doc.performEdit("mute solo markers", [](EditState& e)
            { e.tracks[0].mute = true; e.tracks[0].solo = true; e.markers[0].sample = 19001; e.markers[0].name = "durable marker"; }));
            check(preparation.append(f.sink.ticket, f.doc.getProject())); check(preparation.checkpoint());
            const auto cursor = preparation.checkpointInfo(); check(preparation.close()); const auto before = f.doc.getProject();
            const juce::String kind(action);
            if (kind == "undo") check(f.doc.undo());
            else if (kind == "retake") check(f.doc.performEdit("reserved version switch", [](EditState& e) { e.takeStacks[0].activeVersionId = e.takeStacks[0].versions[1].versionId; }));
            else check(f.doc.performEdit("edit", [](EditState& e) { e.tracks[0].mute = false; e.markers[0].name = "new"; }));
            bool hitStage = false;
            EditJournal log({nullptr, [&](const char* s)
            { if (juce::String(s) == (committed ? "journal-after-commit" : "journal-before-commit")) { hitStage = true; throw std::runtime_error("Injected crash boundary"); } }});
            check(log.open(f.root, before, cursor));
            require(log.append(f.sink.ticket, f.doc.getProject(), kind == "retake" ? JournalKind::RetakeVersionSwitch : JournalKind::EditTransaction).failed(), "Crash seam did not stop append");
            log.close(); require(hitStage, "Wrong crash stage"); twice(f, committed ? f.doc.getProject() : before);
        });
    }
    test("Late TakeFinalized restores registry without resurrecting a deleted placement", []
    {
        Fixture f; const auto take = f.doc.getProject().media->takes[0];
        EditJournal log; check(log.open(f.root, f.base.project, f.base.checkpointInfo));
        check(f.doc.updateTakeState(take.takeId, TakeState::finalising)); check(log.appendRegistry(f.doc.getProject()));
        check(f.doc.performEdit("delete placed take", [](EditState& e) { for (auto& t : e.tracks) t.clips.edit().clear(); e.linkGroups.clear(); }));
        check(log.append(f.sink.ticket, f.doc.getProject())); check(log.close());
        auto finalized = f.doc.getProject(); auto registry = std::make_shared<MediaRegistry>(*finalized.media); registry->takes[0].state = TakeState::complete; finalized.media = registry;
        // Manifest is a test fixture of the completed media. The normal writer's
        // finalization event is separate from the deleted placement transaction.
        const auto manifest = f.root.getChildFile("media/takes/" + juce::Uuid(take.takeId).toDashedString() + "/take.json");
        require(manifest.deleteFile(), "Remove fixture manifest before recreating it"); RecoveryScanner::writeTakeManifest(f.root, finalized, take.takeId);
        const auto& asset = *finalized.media->findAsset(take.cam1AssetId); JournalTakeStarted start; start.takeId = juce::Uuid(take.takeId);
        start.files.push_back({juce::Uuid(asset.assetId).toDashedString(), asset.relativePath, asset.relativePath});
        RecordingJournal takes; check(takes.open(f.root.getChildFile("journal"))); check(takes.append(start));
        check(takes.append(JournalTakeStopped{start.takeId, 48000, juce::Uuid()})); check(takes.append(JournalTakeFinalized{start.takeId})); check(takes.close());
        twice(f, f.doc.getProject()); RecoveryReport r; check(RecoveryScanner().run(f.root, r));
        require(r.project.media->findTake(take.takeId)->state == TakeState::complete && r.project.tracks.front().clips.items().empty(), "Finalize resurrected clip or lost complete state");
    });
    for (const auto* stage : {"recovery-before-commit", "recovery-after-commit"}) test(stage, [stage]
    {
        Fixture f; EditJournal log; check(log.open(f.root, f.base.project, f.base.checkpointInfo));
        check(f.doc.performEdit("saved edit", [](EditState& e) { e.markers[0].name = "saved after replay"; }));
        check(log.append(f.sink.ticket, f.doc.getProject())); check(log.close());
        RecoveryReport interrupted; RecorderDocument untouched;
        RecoveryScanner scanner({nullptr, [stage](const char* s) { if (juce::String(s) == stage) throw std::runtime_error("Re-crash"); }});
        require(scanner.run(f.root, interrupted, &untouched).failed(), "Recovery hook not reached");
        require(untouched.getProject().projectId != f.doc.getProject().projectId, "Adopted before commit completion"); twice(f, f.doc.getProject());
    });
    test("Recovered journal continuation accepts a new edit and another restart", []
    {
        Fixture f; EditJournal log; check(log.open(f.root, f.base.project, f.base.checkpointInfo));
        check(f.doc.performEdit("one", [](EditState& e) { e.name = "one"; })); check(log.append(f.sink.ticket, f.doc.getProject())); check(log.close());
        RecoveryReport recovered; check(RecoveryScanner().run(f.root, recovered));
        check(f.doc.adopt(recovered.project, f.root.getChildFile("project.recorder"), recovered.checkpointInfo));
        EditJournal resumed; check(resumed.open(f.root, recovered.project, recovered.checkpointInfo));
        check(f.doc.performEdit("two", [](EditState& e) { e.name = "two"; })); check(resumed.append(f.sink.ticket, f.doc.getProject())); check(resumed.close()); twice(f, f.doc.getProject());
    });
    test("Corrupt primary remains intact while resumed edits checkpoint into recovery", []
    {
        Fixture f; const auto primary = f.root.getChildFile("project.recorder"); require(primary.replaceWithText("{torn"), "Damage fixture primary");
        const auto original = sha256(primary); RecoveryReport recovered; check(RecoveryScanner().run(f.root, recovered));
        check(f.doc.adopt(recovered.project, primary, recovered.checkpointInfo));
        EditJournal log; check(log.open(f.root, recovered.project, recovered.checkpointInfo));
        check(f.doc.performEdit("after damaged checkpoint", [](EditState& e) { e.name = "continued safely"; }));
        check(log.append(f.sink.ticket, f.doc.getProject())); check(log.checkpoint()); check(log.close());
        twice(f, f.doc.getProject()); require(sha256(primary) == original, "Corrupt primary overwritten during checkpoint");
    });
    test("Invalid placement delta does not suppress a valid stopped take", []
    {
        const auto root = crashFixture::directory("r19-invalid-placement"); const auto saved = crashFixture::baseline(root);
        const auto take = saved.media->takes[0]; const auto asset = *saved.media->findAsset(take.cam1AssetId);
        RecorderProject empty; empty.projectId = saved.projectId;
        require(root.getChildFile("project.recorder").replaceWithText(RecorderSerializer::toJson(empty)), "Reset fixture checkpoint");
        require(root.getChildFile("project.recorder.bak").replaceWithText(RecorderSerializer::toJson(empty)), "Reset fixture backup");
        require(root.getChildFile("journal/edits-000001.log").replaceWithData(nullptr, 0), "Reset fixture journal");
        RecorderDocument doc; crashFixture::TicketSink sink; check(doc.adopt(empty, root.getChildFile("project.recorder"), {})); doc.setJournalSink(&sink);
        check(doc.placeTake(take, {asset})); auto payload = EditJournal::payload(empty, sink.ticket, doc.getProject()); set(payload, "validationHash", "wrong");
        RecordingJournal edits; check(edits.openEdits(root.getChildFile("journal"))); check(edits.appendEditRecord(JournalKind::TakePlacement, payload, juce::Uuid(sink.ticket.transactionId))); check(edits.close());
        RecordingJournal journal; check(journal.open(root.getChildFile("journal"))); JournalTakeStarted start; start.takeId = juce::Uuid(take.takeId);
        start.files.push_back({juce::Uuid(asset.assetId).toDashedString(), asset.relativePath, asset.relativePath}); check(journal.append(start));
        check(journal.append(JournalTakeStopped{start.takeId, 48000, juce::Uuid(sink.ticket.transactionId)})); check(journal.append(JournalTakeFinalized{start.takeId})); check(journal.close());
        RecoveryReport first, second; check(RecoveryScanner().run(root, first)); check(RecoveryScanner().run(root, second));
        require(first.ignoredEditTail && first.addedClips == 1 && second.addedClips == 0 && second.changedTakes == 0, "Invalid edit transaction hid stopped placement");
        doc.setJournalSink(nullptr);
    });
    test("Round-10 completed manifest reuses normal MP4 without remux", []
    {
        Fixture f; const auto take = f.doc.getProject().media->takes[0]; const auto asset = *f.doc.getProject().media->findAsset(take.cam1AssetId);
        auto v = object(); set(v, "schemaVersion", 1); set(v, "takeId", juce::Uuid(take.takeId).toDashedString()); set(v, "state", "done");
        set(v, "cameraAssetId", asset.assetId); set(v, "Fs", 48000); set(v, "fpsNumerator", 30); set(v, "fpsDenominator", 1);
        set(v, "N0", integer(take.N0)); set(v, "Nstop", integer(take.N0 + take.logicalLength)); set(v, "logicalLength", integer(take.logicalLength));
        set(v, "placementSample", integer(take.placementSample)); set(v, "microphones", juce::Array<juce::var>{});
        auto video = object(), mux = object(); const auto media = f.root.getChildFile(asset.relativePath);
        set(mux, "finalized", true); set(mux, "fileSizeBytes", integer(media.getSize())); set(video, "mux", mux); set(video, "inspection", Mp4TakeWriter::inspect(media)); set(v, "video", video);
        const auto file = f.root.getChildFile("media/takes/" + juce::Uuid(take.takeId).toDashedString() + "/take.json");
        require(file.replaceWithText(juce::JSON::toString(v)), "Write native product fixture manifest"); const auto all = crashFixture::hashes(f.root);
        RecoveryReport r; check(RecoveryScanner().run(f.root, r));
        require(r.changedTakes == 0 && all == crashFixture::hashes(f.root) && r.project.media->findAsset(asset.assetId)->relativePath == asset.relativePath, "Completed product MP4 was unnecessarily recovered");
    });
    std::cout << "RetakeRecoveryTests: " << passed << " passed, " << failed << " failed (retake controller binding deferred to 27/28)\n"; return failed ? 1 : 0;
}
