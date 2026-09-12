#include "CrashFixtures.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace gocue::recorder;
using namespace gocue::recorder::recovery;
namespace
{
struct Fixture
{
    juce::File root = crashFixture::directory("recovery"); RecorderProject initial;
    Fixture() { RecoveryScanner::writeCheckpoint(root.getChildFile("project.recorder"), initial); }
    juce::File editLog() const { return root.getChildFile("journal/edits-000001.log"); }
};
void raw(const juce::File& file, const juce::MemoryBlock& bytes) { require(file.replaceWithData(bytes.getData(), bytes.getSize()), "Mutate isolated test fixture"); }
juce::MemoryBlock bytes(const juce::File& f) { juce::MemoryBlock b; require(f.loadFileAsData(b), "Read test fixture"); return b; }
juce::Uuid audio(Fixture& f, unsigned seconds = 2, Sample placement = 0)
{
    auto config = crashFixture::wavConfig(f.root, 2); config.pstart = placement; WavTrackWriter writer(config); check(writer.start());
    for (unsigned i = 0; i < seconds * 30; ++i)
    { crashFixture::push(writer, std::uint64_t(i) * 1600, 1600, 2); while (writer.queueFrames() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    check(writer.stop(Sample(seconds) * 48000, juce::Uuid())); return config.takeId;
}
EditDelta edit(Fixture& f, RecorderDocument& doc, const char* name)
{
    crashFixture::TicketSink sink; doc.setJournalSink(&sink); check(doc.performEdit(name, [&](EditState& state) { state.name = name; }));
    doc.setJournalSink(nullptr); RecoveryScanner::appendEdit(f.editLog(), sink.ticket, doc.getProject()); return sink.ticket;
}
RecorderDocument& adopt(Fixture& f, RecorderDocument& doc) { check(doc.adopt(f.initial, f.root.getChildFile("project.recorder"), {})); return doc; }
}
int runRecoveryTests()
{
    av_log_set_level(AV_LOG_ERROR); unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& run)
    { try { run(); ++passed; std::cout << "PASS " << name << '\n'; } catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; } };
    test("Recovery replays 5-second then 15-second placement with an empty interval and is idempotent", []
    {
        Fixture f; audio(f, 5); const auto later = audio(f, 2, 15 * 48000);
        RecoveryReport a, b; check(RecoveryScanner().run(f.root, a));
        require(a.project.media->findTake(later.toString())->placementSample == 15 * 48000, "Journal Pstart retained");
        for (const auto& lane : a.project.tracks)
            require(lane.clips.items().size() == 2 && lane.clips.items()[0].timelineEnd() == 5 * 48000
                && lane.clips.items()[1].timelineStartSample == 15 * 48000, "Recovery leaves the requested timeline gap");
        const auto state = RecorderSerializer::toJson(a.project); const auto files = crashFixture::hashes(f.root);
        check(RecoveryScanner().run(f.root, b));
        require(b.changedTakes == 0 && b.addedClips == 0 && RecorderSerializer::toJson(b.project) == state
            && crashFixture::hashes(f.root) == files, "Second recovery neither shifts placement nor rewrites media");
    });
    test("Unfinished take without TakeStopped recovers durable prefix at the requested playhead", []
    {
        Fixture f; auto config = crashFixture::wavConfig(f.root, 1); config.pstart = 15 * 48000 + 37;
        {
            WavTrackWriter writer(config); check(writer.start());
            for (unsigned i = 0; i < 3; ++i) crashFixture::push(writer, i * 1600, 1600, 1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (writer.writtenSamples() < 4800)
            { require(std::chrono::steady_clock::now() < deadline, "Unfinished take writer timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            // The writer's failure/destruction path checkpoints its accepted
            // prefix without inventing a Stop or placement transaction.
        }
        JournalReplay journal; check(RecordingJournal::replay(f.root.getChildFile("journal"), journal));
        for (const auto& record : journal.records) require(record.kind != JournalKind::TakeStopped && record.kind != JournalKind::TakeFinalized,
            "Unfinished fixture must have no normal stop/finalize record");
        RecoveryReport report; check(RecoveryScanner().run(f.root, report));
        const auto* take = report.project.media->findTake(config.takeId.toString());
        require(take && take->state == TakeState::partial && take->placementSample == config.pstart && take->logicalLength == 4800,
            "Crash prefix retains absolute placement independently of capture-clock length");
        require(report.project.tracks[0].clips.items()[0].timelineStartSample == config.pstart, "Recovered clip begins after the leading empty interval");
    });
    for (const Sample placement : {Sample{0}, Sample{24037}})
    {
        const auto name = "Recovery overwrites sparse microphone at exact Pstart " + std::to_string(placement);
        test(name.c_str(), [placement]
        {
            Fixture f; audio(f); RecoveryReport before; check(RecoveryScanner().run(f.root, before));
            const auto untouched = before.project.tracks[0].clips.items()[0];
            auto config = crashFixture::wavConfig(f.root, 1); config.logicalMicrophones = {2}; config.devices[0].mic = 2; config.pstart = placement;
            WavTrackWriter writer(config); check(writer.start()); crashFixture::push(writer, 0, 1600, 1); check(writer.stop(1600, juce::Uuid()));
            const auto originals = crashFixture::hashes(f.root, true); RecoveryReport after; check(RecoveryScanner().run(f.root, after));
            const auto* take = after.project.media->findTake(config.takeId.toString());
            require(take && take->placementSample == placement, "Zero is a real overwrite position, never an append sentinel");
            const auto* kept = after.project.findClip(untouched.clipId);
            require(kept && kept->timelineStartSample == untouched.timelineStartSample && kept->lengthSamples == untouched.lengthSamples,
                "Recovery does not carve the linked unrecorded microphone");
            const auto& lane = after.project.tracks[1];
            require(lane.microphoneIndex == 1 && lane.clips.items().back().timelineStartSample == placement
                && lane.clips.items().back().lengthSamples == 1600, "Sparse logical microphone and placement retained");
            require(lane.clips.items().size() == (placement == 0 ? 2u : 3u), "Recovery uses trim or split on the recorded lane");
            require(after.addedClips == 1 && originals == crashFixture::hashes(f.root, true), "Count newly placed clips without changing originals");
            RecoveryReport again; check(RecoveryScanner().run(f.root, again));
            require(again.changedTakes == 0 && RecorderSerializer::toJson(again.project) == RecorderSerializer::toJson(after.project), "Overwrite recovery is idempotent");
        });
    }
    test("Torn stereo right sample is excluded as a whole frame during recovery", []
    {
        Fixture f; auto c = crashFixture::wavConfig(f.root, 1); c.slotChannels = {2};
        c.devices[0].rightPhysicalIndex = c.devices[0].physicalIndex + 1; c.devices[0].rightActiveIndex = 1;
        WavTrackWriter writer(c); check(writer.start()); crashFixture::push(writer,0,1600,2); check(writer.stop(1600,juce::Uuid()));
        const auto file = f.root.getChildFile(WavTrackWriter::chunkPath(c.takeId,1,1)); auto torn = bytes(file); torn.setSize(torn.getSize()-1); raw(file,torn);
        const auto original = sha256(file); RecoveryReport report; check(RecoveryScanner().run(f.root,report));
        const auto* asset = report.project.media->findAsset(report.project.media->takes[0].microphoneAssetIds[0]);
        require(asset && asset->originalFormat.channels == 2 && asset->logicalLength == 1600 && asset->chunks[0].sourceRange.length == 1599
            && asset->gaps.size() == 1 && asset->gaps[0].length == 1, "Incomplete L/R frame becomes one tail gap");
        require(f.root.getChildFile(asset->chunks[0].relativePath).getSize() == 44+1599*6 && sha256(file) == original, "Complete stereo prefix copied; original untouched");
    });
    test("Mixed stereo/mono recovery preserves headers, samples and input pairs idempotently", []
    {
        Fixture f; auto c = crashFixture::wavConfig(f.root, 2); c.slotChannels = {2,1};
        c.devices[0].rightPhysicalIndex = c.devices[0].physicalIndex + 1; c.devices[0].rightActiveIndex = 1;
        c.devices[1].physicalIndex = c.devices[0].physicalIndex + 2; c.devices[1].activeIndex = 2;
        WavTrackWriter writer(c); check(writer.start()); crashFixture::push(writer, 0, 1600, 3); check(writer.stop(1600, juce::Uuid()));
        const auto originals = crashFixture::hashes(f.root, true); RecoveryReport report; check(RecoveryScanner().run(f.root, report));
        require(report.project.media->takes.size() == 1, "Recovered mixed take");
        const auto& take = report.project.media->takes[0];
        require(take.capture.physicalInputsRight == std::vector<int>({c.devices[0].rightPhysicalIndex,-1}), "Recovered physical input pair");
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto* asset = report.project.media->findAsset(take.microphoneAssetIds[i]);
            require(asset && asset->originalFormat.channels == (i ? 1 : 2) && asset->logicalLength == 1600, "Recovered format/frame count");
            const auto original = bytes(f.root.getChildFile(WavTrackWriter::chunkPath(c.takeId, i + 1, 1)));
            const auto recovered = bytes(f.root.getChildFile(asset->chunks[0].relativePath));
            require(original == recovered, "Recovered interleaved PCM is byte-identical");
        }
        const auto project = RecorderSerializer::toJson(report.project); RecoveryReport second; check(RecoveryScanner().run(f.root, second));
        require(RecorderSerializer::toJson(second.project) == project && originals == crashFixture::hashes(f.root, true), "Idempotent recovery leaves originals unchanged");
    });
    test("Latest valid backup selected by revision, corrupt primary retained", []
    {
        Fixture f; auto newer = f.initial; newer.name = "latest"; newer.editRevision = 4;
        RecoveryScanner::writeCheckpoint(f.root.getChildFile("project.recorder.bak"), newer); RecoveryReport report; check(RecoveryScanner().run(f.root, report));
        require(report.usedBackup && report.project.editRevision == 4, "Newest backup ignored");
        require(f.root.getChildFile("project.recorder").replaceWithText("{torn"), "Corrupt primary fixture");
        const auto hash = sha256(f.root.getChildFile("project.recorder")); check(RecoveryScanner().run(f.root, report)); require(report.project.editRevision == 4 && sha256(f.root.getChildFile("project.recorder")) == hash, "Corrupt primary changed");
    });
    test("Invalid checksum and unsupported schema never become checkpoints", []
    {
        Fixture f; auto v = juce::JSON::parse(RecorderSerializer::toJson(f.initial)); set(v, "schemaVersion", 99);
        require(f.root.getChildFile("project.recorder").replaceWithText(juce::JSON::toString(v)), "Future schema fixture");
        const auto before = crashFixture::hashes(f.root, true); RecoveryReport report;
        require(RecoveryScanner().run(f.root, report).failed() && report.attempt.getChildFile("diagnostic.json").existsAsFile(), "Missing no-checkpoint diagnostic");
        require(before == crashFixture::hashes(f.root, true), "No-checkpoint recovery changed original");
    });
    test("Project writer lock excludes recovery without a mutation", []
    {
        Fixture f; WriterLock lock(f.root.getChildFile("project.writer.lock")); const auto before = crashFixture::hashes(f.root);
        RecoveryReport report; require(RecoveryScanner().run(f.root, report).failed(), "Recovery ignored project lock");
        // A lock failure must not even create a diagnostic attempt.
        require(before == crashFixture::hashes(f.root), "Lock failure wrote project files");
    });
    test("Legacy WavTrackWriter journal lock excludes scanner", []
    {
        Fixture f; RecordingJournal writer; check(writer.open(f.root.getChildFile("journal")));
        RecoveryReport report; require(RecoveryScanner().run(f.root, report).failed(), "Legacy writer not excluded"); check(writer.close());
    });
    for (const bool crc : {false, true}) test(crc ? "Edit CRC stops all later commits and segments" : "Truncated edit commit retains previous revision", [crc]
    {
        Fixture f; RecorderDocument doc; adopt(f, doc); edit(f, doc, "saved"); const auto durableEnd = f.editLog().getSize(); edit(f, doc, "unsaved");
        auto b = bytes(f.editLog());
        if (crc) static_cast<std::uint8_t*>(b.getData())[std::size_t(durableEnd) + 49] ^= 0x20; else b.setSize(b.getSize() - 3);
        raw(f.editLog(), b);
        crashFixture::TicketSink sink; doc.setJournalSink(&sink); check(doc.performEdit("later", [](EditState& state) { state.name = "later"; })); doc.setJournalSink(nullptr);
        RecoveryScanner::appendEdit(f.root.getChildFile("journal/edits-000002.log"), sink.ticket, doc.getProject());
        const auto before = crashFixture::hashes(f.root, true); RecoveryReport report; check(RecoveryScanner().run(f.root, report));
        require(report.ignoredEditTail && report.project.name == "saved" && report.project.editRevision == 1, "Invalid tail/later segment applied");
        require(before == crashFixture::hashes(f.root, true), "Damaged original journal repaired in place");
    });
    test("Damaged edit log cannot be resumed in place", []
    {
        Fixture f; RecorderDocument doc; adopt(f, doc); const auto ticket = edit(f, doc, "saved"); auto b = bytes(f.editLog()); b.append("x", 1); raw(f.editLog(), b);
        bool rejected = false; try { RecoveryScanner::appendEdit(f.editLog(), ticket, doc.getProject()); } catch (...) { rejected = true; }
        RecoveryReport report; check(RecoveryScanner().run(f.root, report)); require(rejected && report.ignoredEditTail && report.project.name == "saved", "Damaged edit tail handling");
    });
    test("Duplicate transaction ignored across records and segments", []
    {
        Fixture f; RecorderDocument doc; adopt(f, doc); auto ticket = edit(f, doc, "once");
        RecoveryScanner::appendEdit(f.editLog(), ticket, doc.getProject());
        RecoveryScanner::appendEdit(f.root.getChildFile("journal/edits-000002.log"), ticket, doc.getProject());
        RecoveryReport report; check(RecoveryScanner().run(f.root, report)); require(report.project.editRevision == 1 && report.duplicateTransactions == 2, "Duplicate edit applied");
    });
    test("Orphan reported and preserved, including second scan", []
    {
        Fixture f; writeNew(f.root.getChildFile("media/unknown.bin"), "orphan", 6); const auto hash = sha256(f.root.getChildFile("media/unknown.bin"));
        RecoveryReport a, b; check(RecoveryScanner().run(f.root, a)); check(RecoveryScanner().run(f.root, b));
        require(a.orphans.contains("media/unknown.bin") && b.changedTakes == 0 && sha256(f.root.getChildFile("media/unknown.bin")) == hash, "Orphan deletion/missing report");
    });
    test("WAV torn header and partial bytes recover only durable complete samples", []
    {
        Fixture f; const auto take = audio(f); const auto wav = f.root.getChildFile(WavTrackWriter::chunkPath(take, 1, 1)); auto b = bytes(wav);
        std::memset(static_cast<std::uint8_t*>(b.getData()) + 40, 0, 4); b.setSize(b.getSize() - 4); raw(wav, b);
        const auto before = crashFixture::hashes(f.root, true); RecoveryReport report; check(RecoveryScanner().run(f.root, report));
        const auto* t = report.project.media->findTake(take.toString()); require(t && t->logicalLength == 96000, "Logical take length was truncated");
        const auto* a = report.project.media->findAsset(t->microphoneAssetIds[0]); const auto* good = report.project.media->findAsset(t->microphoneAssetIds[1]);
        require(a->availableRanges[0].length == 95998 && a->gaps.back().length == 2 && good->availableRanges[0].length == 96000, "Partial byte/good mic range mismatch");
        require(crashFixture::verifyPcm(f.root, *a, 0) && crashFixture::verifyPcm(f.root, *good, 1) && before == crashFixture::hashes(f.root, true), "PCM oracle/original hash mismatch");
    });
    test("Truncated take journal tail and duplicate take transaction", []
    {
        Fixture f; audio(f); const auto file = RecordingJournal::logFile(f.root.getChildFile("journal"), 1); auto b = bytes(file); b.setSize(b.getSize() - 3); raw(file, b);
        RecoveryReport report; check(RecoveryScanner().run(f.root, report)); require(report.ignoredTakeTail && report.changedTakes == 1, "Truncated take finalization not recovered");
    });
    for (const auto* stage : {"recovery-before-commit", "recovery-after-commit"}) test(stage, [stage]
    {
        Fixture f; audio(f); const auto before = crashFixture::hashes(f.root, true); RecorderDocument document; adopt(f, document);
        RecoveryScanner scanner({nullptr, [stage](const char* name) { if (std::strcmp(stage, name) == 0) throw std::runtime_error("Injected recovery interruption"); }});
        RecoveryReport first; require(scanner.run(f.root, first, &document).failed(), "Recovery crash seam did not fire"); require(document.getProject().media->takes.empty(), "Document adopted before commit boundary");
        RecoveryReport second, third; check(RecoveryScanner().run(f.root, second, &document)); const auto all = crashFixture::hashes(f.root); check(RecoveryScanner().run(f.root, third));
        require(second.project.media->takes.size() == 1 && third.changedTakes == 0 && third.addedClips == 0 && all == crashFixture::hashes(f.root), "Recovery re-crash duplicated/rewrote result");
        require(second.changedTakes == (std::strcmp(stage, "recovery-before-commit") == 0 ? 1u : 0u), "Uncommitted/committed recovery candidate selection");
        require(before == crashFixture::hashes(f.root, true), "Re-crash recovery changed originals");
    });
    test("Partial cam2 reports 1.2s gap without truncating cam1 or WAV", []
    {
        Fixture f; auto config = crashFixture::wavConfig(f.root, 2); const auto prefix = "media/takes/" + config.takeId.toDashedString() + "/";
        for (int c = 1; c <= 2; ++c) config.additionalFiles.push_back({juce::Uuid().toDashedString(), prefix + "cam" + juce::String(c) + ".recording.mp4", prefix + "cam" + juce::String(c) + ".mp4"});
        { crashFixture::Camera camera(f.root.getChildFile(prefix + "cam1.mp4")); for (int i = 0; i < 150; ++i) camera.frame(); camera.finish(); }
        { crashFixture::Camera camera(f.root.getChildFile(prefix + "cam2.mp4")); for (int i = 0; i < 114; ++i) camera.frame(); camera.finish(); }
        WavTrackWriter wav(config); check(wav.start()); for (unsigned i = 0; i < 150; ++i) { crashFixture::push(wav, std::uint64_t(i) * 1600, 1600, 2); while (wav.queueFrames() > 48000) Sleep(1); } check(wav.stop(240000, juce::Uuid()));
        const auto before = crashFixture::hashes(f.root, true); RecoveryReport report; check(RecoveryScanner().run(f.root, report));
        const auto* take = report.project.media->findTake(config.takeId.toString()); require(take != nullptr, "Partial take absent");
        const auto* cam1 = report.project.media->findAsset(take->cam1AssetId); const auto* cam2 = report.project.media->findAsset(take->cam2AssetId);
        require(cam1->availableRanges.size() == 1 && cam1->availableRanges[0].length == 240000 && cam1->gaps.empty(), "Good camera shortened");
        require(cam2->availableRanges.size() == 1 && cam2->availableRanges[0].length == 182400 && cam2->gaps.back().length == 57600, "Cam2 gap wrong");
        for (const auto& mic : take->microphoneAssetIds) require(report.project.media->findAsset(mic)->availableRanges[0].length == 240000, "Good WAV shortened");
        require(report.messages.joinIntoString(" ").contains(juce::String::fromUTF8("캠2 마지막 1.2초 없음")), "Korean actual gap report missing");
        require(before == crashFixture::hashes(f.root, true), "Camera recovery changed original bytes");
        RecoveryReport again; const auto all = crashFixture::hashes(f.root); check(RecoveryScanner().run(f.root, again)); require(again.changedTakes == 0 && all == crashFixture::hashes(f.root), "MP4 recovery not idempotent");
    });
    test("Completed manifest preserved and durable edit delta replayed", []
    {
        const auto root = crashFixture::directory("completed"); const auto expected = crashFixture::baseline(root); const auto before = crashFixture::hashes(root, true);
        RecoveryReport report; check(RecoveryScanner().run(root, report)); require(report.changedTakes == 0 && RecorderSerializer::toJson(report.project) == RecorderSerializer::toJson(expected), "Completed take/edit changed");
        require(before == crashFixture::hashes(root, true), "Completed file remuxed or modified");
    });
    test("Completed manifest restores missing registration without remux", []
    {
        const auto root = crashFixture::directory("manifest-registry"); const auto saved = crashFixture::baseline(root); const auto& take = saved.media->takes.front();
        RecorderProject empty; empty.projectId = saved.projectId;
        require(root.getChildFile("project.recorder").replaceWithText(RecorderSerializer::toJson(empty)), "Reset isolated checkpoint");
        require(root.getChildFile("journal/edits-000001.log").replaceWithData(nullptr, 0), "Reset isolated edit journal");
        const auto* asset = saved.media->findAsset(take.cam1AssetId); JournalTakeStarted started; started.takeId = juce::Uuid(take.takeId);
        started.files.push_back({juce::Uuid(asset->assetId).toDashedString(), asset->relativePath.replace(".mp4", ".recording.mp4"), asset->relativePath});
        RecordingJournal journal; check(journal.open(root.getChildFile("journal"))); check(journal.append(started)); check(journal.append(JournalTakeStopped{started.takeId, 48000, juce::Uuid()})); check(journal.append(JournalTakeFinalized{started.takeId})); check(journal.close());
        const auto before = crashFixture::hashes(root, true); RecoveryReport report; check(RecoveryScanner().run(root, report));
        const auto* restored = report.project.media->findTake(take.takeId); require(restored && restored->state == TakeState::complete, "Completed take registry not restored");
        require(report.project.media->findAsset(take.cam1AssetId)->relativePath == asset->relativePath && report.addedClips == 1, "Completed media was recopied/remuxed");
        require(before == crashFixture::hashes(root, true), "Completed original changed");
    });
    test("No durable sample take produces one diagnostic commit and zero clips", []
    {
        Fixture f; const auto take = audio(f, 0); RecoveryReport a, b; check(RecoveryScanner().run(f.root, a)); const auto all = crashFixture::hashes(f.root);
        check(RecoveryScanner().run(f.root, b)); require(!a.project.media->findTake(take.toString()) && a.addedClips == 0 && b.changedTakes == 0 && all == crashFixture::hashes(f.root), "Empty take recovery repeated or invented media");
    });
    test("Output clock and dub placement retain exact origin and Pstart", []
    {
        Fixture f; auto c = crashFixture::wavConfig(f.root, 1); c.usesOutputOrigin = true; c.placementMode = "dub"; c.n0 = 123; c.o0 = 9007199254740993LL; c.pstart = 37;
        WavTrackWriter writer(c); check(writer.start()); for (unsigned i = 0; i < 30; ++i) crashFixture::push(writer, std::uint64_t(i) * 1600, 1600, 1); check(writer.stop(c.o0 + 48000, juce::Uuid()));
        RecoveryReport report; check(RecoveryScanner().run(f.root, report)); const auto* take = report.project.media->findTake(c.takeId.toString());
        require(take && take->mode == TakeMode::dub && take->O0 == c.o0 && take->N0 == c.n0 && take->logicalLength == 48000
            && report.project.tracks[0].clips.items()[0].timelineStartSample == 37, "Output origin or dub Pstart changed");
    });
    test("Saving recovered checkpoint and deleting clips never resurrects placement", []
    {
        Fixture f; audio(f); RecorderDocument doc; RecoveryReport a; check(RecoveryScanner().run(f.root, a, &doc));
        check(doc.performEdit("delete recovered clips", [](EditState& state) { state.tracks.clear(); state.linkGroups.clear(); }));
        RecoveryScanner::writeCheckpoint(f.root.getChildFile("project.recorder"), doc.getProject()); RecoveryReport b; check(RecoveryScanner().run(f.root, b));
        require(b.changedTakes == 0 && b.addedClips == 0 && b.project.tracks.empty() && b.project.media->takes.size() == 1, "Deleted take clips reappeared");
    });
    test("MP4 packet CRC excludes corrupt fragment and all following packets", []
    {
        const auto root = crashFixture::directory("mp4-crc"); const auto file = root.getChildFile("cam1.mp4");
        { crashFixture::Camera camera(file); for (int i = 0; i < 91; ++i) camera.frame(); } // Leave fMP4 open-ended.
        const auto source = root.getChildFile("cam1.recording.mp4"), index = Mp4RecoveryIndex::pathFor(source);
        std::uint64_t damage = 0; unsigned fragments = 0;
        Log::read(index, [&](const Record& record) { if (record.kind == Kind::fragment && ++fragments == 2) damage = std::uint64_t(number(record.payload["data"])); return true; });
        require(damage > 0, "Missing second fragment fixture"); auto b = bytes(source); static_cast<std::uint8_t*>(b.getData())[std::size_t(damage)] ^= 1; raw(source, b);
        const auto hash = sha256(source); const auto recovered = Mp4RecoveryIndex::recover(source, index, root.getChildFile("copy.mp4"), 48000);
        require(recovered.ignoredTail && recovered.videoFrames == 30 && recovered.samples == 48000 && sha256(source) == hash, "Corrupt fragment included or preceding frames lost");
    });
    test("Legacy intact fMP4 without persisted index can rebuild into a new attempt", []
    {
        const auto root = crashFixture::directory("legacy-mp4"); const auto file = root.getChildFile("cam1.mp4");
        { crashFixture::Camera camera(file); for (int i = 0; i < 61; ++i) camera.frame(); }
        const auto source = root.getChildFile("cam1.recording.mp4"); const auto hash = sha256(source); const auto index = root.getChildFile("rebuilt.log");
        Mp4RecoveryIndex::rebuild(source, index); const auto recovered = Mp4RecoveryIndex::recover(source, index, root.getChildFile("recovered.mp4"), 48000);
        require(recovered.samples >= 48000 && recovered.videoFrames >= 30 && sha256(source) == hash, "Legacy complete fragment recovery failed");
    });
    for (const char* phase : {"journal-before-commit", "journal-after-commit"}) test(phase, [phase]
    {
        Fixture f; RecorderDocument doc; adopt(f, doc); crashFixture::TicketSink sink; doc.setJournalSink(&sink); check(doc.performEdit("placed", [](EditState& state) { state.name = "placed"; }));
        bool interrupted = false;
        try { RecoveryScanner::appendEdit(f.editLog(), sink.ticket, doc.getProject(), nullptr, [phase](const char* stage) { if (std::strcmp(stage, phase) == 0) throw std::runtime_error("journal interruption"); }); } catch (...) { interrupted = true; }
        RecoveryReport report; check(RecoveryScanner().run(f.root, report));
        require(interrupted && report.project.editRevision == (std::strcmp(phase, "journal-before-commit") == 0 ? 0 : 1), "Edit commit boundary replay wrong");
    });
    std::cout << "RecoveryTests: " << passed << " passed, " << failed << " failed\n"; return failed ? 1 : 0;
}
