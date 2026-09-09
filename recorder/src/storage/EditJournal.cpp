#include "EditJournal.h"
#include "StorageEncoding.h"
#include <algorithm>
#include <array>
#include <map>

namespace gocue::recorder
{
namespace
{
using namespace recovery;
juce::String hash(const RecorderProject& p) { return RecorderSerializer::fingerprint(juce::JSON::parse(RecorderSerializer::toJson(p))); }
void resign(juce::var& v)
{ v.getDynamicObject()->removeProperty("checksum"); set(v, "checksum", RecorderSerializer::fingerprint(v)); }
void mergeEntities(juce::var& edit, const juce::var& changes, const Id& project)
{
    require(changes.isArray(), "Missing entity deltas");
    const std::map<juce::String, juce::String> fields{{"tracks", "trackId"}, {"markers", "markerId"}, {"linkGroups", "linkGroupId"}, {"takeStacks", "stackId"}};
    std::set<std::pair<juce::String, Id>> seen;
    for (const auto& d : *changes.getArray())
    {
        const auto collection = d["collection"].toString(), id = d["id"].toString(); const auto value = d["value"];
        require(isId(id) && d["removed"].isBool() && seen.emplace(collection, id).second, "Invalid/duplicate entity delta");
        const bool removed = bool(d["removed"]);
        if (collection == "project")
        { require(!removed && id == project && value.isString(), "Invalid project delta"); set(edit, "name", value); continue; }
        const bool order = collection.endsWith("Order"); const auto name = order ? collection.dropLastCharacters(5) : collection;
        const auto field = fields.find(name); require(field != fields.end(), "Unknown edit collection");
        auto* list = edit.getDynamicObject()->getProperty(juce::Identifier(name)).getArray(); require(list != nullptr, "Missing collection");
        if (order)
        {
            require(!removed && id == project && value.isArray() && value.size() == list->size(), "Invalid entity order");
            juce::Array<juce::var> next; std::set<Id> ids;
            for (const auto& v : *value.getArray())
            {
                require(v.isString() && ids.insert(v.toString()).second, "Duplicate order ID"); bool found = false;
                for (const auto& item : *list) if (item.getProperty(juce::Identifier(field->second), {}).toString() == v.toString())
                { next.add(item); found = true; break; }
                require(found, "Missing ordered entity");
            }
            edit.getDynamicObject()->setProperty(name, next);
        }
        else
        {
            int found = -1;
            for (int i = 0; i < list->size(); ++i) if ((*list)[i].getProperty(juce::Identifier(field->second), {}).toString() == id) { found = i; break; }
            if (removed) { require(found >= 0, "Removal of missing entity"); list->remove(found); }
            else
            {
                require(value.isObject() && value.getProperty(juce::Identifier(field->second), {}).toString() == id, "Entity ID mismatch");
                if (found >= 0) list->set(found, value.clone()); else list->add(value.clone());
            }
        }
    }
}
void registryInvariant(const RecorderProject& before, const RecorderProject& after)
{
    const auto old = juce::JSON::parse(RecorderSerializer::toJson(before))["media"];
    const auto next = juce::JSON::parse(RecorderSerializer::toJson(after))["media"];
    for (const auto& asset : before.media->assets)
    {
        const auto* a = after.media->findAsset(asset.assetId); require(a != nullptr, "Journal cannot remove registered media");
        require(a->kind == asset.kind && a->contentIdentity == asset.contentIdentity && a->mediaGeneration >= asset.mediaGeneration, "Media identity/generation regression");
        juce::var x, y;
        for (const auto& v : *old["assets"].getArray()) if (v["assetId"].toString() == asset.assetId) x = v;
        for (const auto& v : *next["assets"].getArray()) if (v["assetId"].toString() == asset.assetId) y = v;
        require(RecorderSerializer::fingerprint(x["originalFormat"]) == RecorderSerializer::fingerprint(y["originalFormat"]), "Original format changed");
        require(a->mediaGeneration != asset.mediaGeneration || RecorderSerializer::fingerprint(x) == RecorderSerializer::fingerprint(y), "Changed media requires a new generation");
    }
    for (const auto& t : before.media->takes)
    {
        const auto* n = after.media->findTake(t.takeId); require(n != nullptr, "Journal cannot remove registered takes");
        require(t.cam1AssetId == n->cam1AssetId && t.cam2AssetId == n->cam2AssetId && t.microphoneAssetIds == n->microphoneAssetIds
            && t.mode == n->mode && t.N0 == n->N0 && t.O0 == n->O0 && t.placementSample == n->placementSample, "Take identity/origin changed");
    }
}
}
EditJournal::EditJournal() : EditJournal(Options{}) {}
EditJournal::EditJournal(Options o) : options(std::move(o)), journal(options.faults) {}
EditJournal::~EditJournal() { close(); }
juce::var EditJournal::payload(const RecorderProject& before, const EditDelta& delta, const RecorderProject& after)
{
    using namespace recovery;
    check(after.validate()); require(isId(delta.transactionId) && delta.projectId == after.projectId && before.projectId == after.projectId
        && delta.baseRevision == before.editRevision && delta.revision == after.editRevision, "Invalid edit ticket identity/revision");
    auto p = object(); set(p, "projectId", delta.projectId); set(p, "baseRevision", integer(delta.baseRevision)); set(p, "revision", integer(delta.revision));
    set(p, "name", delta.name); set(p, "validationHash", delta.validationHash); set(p, "baseHash", hash(before)); set(p, "resultHash", hash(after));
    juce::Array<juce::var> entities;
    for (const auto& d : delta.entities)
    { auto v = object(); set(v, "collection", d.collection); set(v, "id", d.entityId); set(v, "removed", d.removed); set(v, "value", d.value.clone()); entities.add(v); }
    set(p, "entities", entities);
    const auto previous = juce::JSON::parse(RecorderSerializer::toJson(before)), result = juce::JSON::parse(RecorderSerializer::toJson(after));
    if (RecorderSerializer::fingerprint(previous["media"]) != RecorderSerializer::fingerprint(result["media"])) set(p, "media", result["media"]);
    if (before.Fs != after.Fs || before.fps != after.fps)
    { auto timebase = object(); set(timebase, "Fs", result["Fs"]); set(timebase, "fps", result["fps"]); set(p, "timebase", timebase); }
    return p;
}
RecorderProject EditJournal::apply(const RecorderProject& before, const juce::var& record, bool registryOnly)
{
    using namespace recovery;
    const auto revision = number(record["revision"]), base = number(record["baseRevision"]);
    require(record["projectId"].toString() == before.projectId && base == before.editRevision && base >= 0
        && (registryOnly ? revision == base : base < INT64_MAX && revision == base + 1), "Edit revision/project chain mismatch");
    require(hash(before) == record["baseHash"].toString(), "Edit base hash mismatch");
    auto root = juce::JSON::parse(RecorderSerializer::toJson(before)); auto edit = root["edit"];
    if (registryOnly) require(record["entities"].isArray() && record["entities"].size() == 0, "Registry commit cannot edit clips");
    mergeEntities(edit, record["entities"], before.projectId);
    require(RecorderSerializer::fingerprint(edit) == record["validationHash"].toString(), "Edit validation hash mismatch");
    if (record.hasProperty("media")) set(root, "media", record["media"]);
    if (record.hasProperty("timebase"))
    {
        require(registryOnly && before.media->assets.empty(), "Timebase is locked after media registration");
        set(root, "Fs", record["timebase"]["Fs"]); set(root, "fps", record["timebase"]["fps"]);
    }
    set(root, "editRevision", integer(revision)); set(root, "checkpointRevision", integer(revision)); resign(root);
    RecorderProject after; check(RecorderSerializer::fromJson(juce::JSON::toString(root), after)); registryInvariant(before, after);
    require(hash(after) == record["resultHash"].toString(), "Edit result hash mismatch"); return after;
}
juce::Result EditJournal::replay(const juce::File& root, RecorderProject& project, const CheckpointInfo& cursor, EditJournalReplay& out)
{
    using namespace recovery; out = {};
    try
    {
        const auto dir = child(root, cursor.journalPath);
        const auto first = RecordingJournal::editLogFile(dir, cursor.journalSegment);
        if (first.existsAsFile())
        {
            juce::FileInputStream input(first); check(input.getStatus()); std::array<std::uint8_t, 4> bytes{};
            if (input.read(bytes.data(), 4) == 4 && storageEncoding::get<std::uint32_t>(bytes.data()) == 0x31564352)
            { out.legacy = true; return juce::Result::ok(); }
        }
        check(RecordingJournal::replayEdits(dir, out.framing, cursor.journalSegment, cursor.journalSequence));
        for (const auto& r : out.framing.records)
        {
            try
            {
                require(r.payload["projectId"].toString() == project.projectId, "Foreign project in edit journal");
                if (r.kind == JournalKind::EditCheckpoint) continue;
                const auto revision = number(r.payload["revision"]);
                if (revision < project.editRevision || (r.kind != JournalKind::MediaRegistry && revision == project.editRevision)) continue;
                if (r.kind == JournalKind::MediaRegistry && hash(project) == r.payload["resultHash"].toString()) continue;
                project = apply(project, r.payload, r.kind == JournalKind::MediaRegistry);
                out.transactions.insert(r.transaction.toString());
                if (r.kind == JournalKind::MediaRegistry) ++out.registryCommits; else ++out.appliedEdits;
            }
            catch (const std::exception& e)
            { out.framing.ignoredTail = true; out.framing.tailReason = e.what(); break; }
        }
        return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(e.what()); }
}
juce::Result EditJournal::open(const juce::File& projectRoot, const RecorderProject& durable, const CheckpointInfo& position)
{
    using namespace recovery;
    try
    {
        require(!lock, "Edit journal already open"); check(durable.validate()); root = projectRoot;
        lock = std::make_unique<WriterLock>(child(root, "project.writer.lock")); current = durable; cursor = position;
        const auto file = child(root, "project.recorder");
        if (!file.existsAsFile()) check(RecorderSerializer::writeCheckpoint(file, current, cursor, options.faults, options.hook));
        else
        {
            // A recovered continuation is selected by RecoveryScanner, not inferred
            // from a stale root checkpoint. The caller passes its verified cursor.
            RecorderProject saved; CheckpointInfo savedInfo;
            const auto selected = position.sourceFile != juce::File() ? position.sourceFile : file;
            require(selected.isAChildOf(root), "Checkpoint provenance is outside project");
            child(root, selected.getRelativePathFrom(root).replaceCharacter('\\', '/'));
            check(RecorderSerializer::readCheckpoint(selected, saved, &savedInfo, position.sourceFile == juce::File()));
            require(saved.projectId == current.projectId && saved.editRevision <= current.editRevision, "Stale/foreign journal opening");
            if (position.generation == 0) cursor = savedInfo;
            else require(position.generation == savedInfo.generation, "Checkpoint generation changed before journal opening");
            RecorderProject replayed = saved; EditJournalReplay read; check(replay(root, replayed, cursor, read));
            require(!read.legacy && !read.framing.ignoredTail, "Run RecoveryScanner before extending a legacy/damaged journal");
            require(hash(replayed) == hash(current), "Document must adopt the recovered durable revision before editing");
            for (const auto& r : read.framing.records) if (r.kind == JournalKind::EditCheckpoint)
                committedGenerations.emplace(number(r.payload["generation"]), r.payload["checksum"].toString());
        }
        directory = child(root, cursor.journalPath);
        check(journal.openEdits(directory, cursor.journalSegment, cursor.journalSequence, options.rotationBytes)); return juce::Result::ok();
    }
    catch (const std::exception& e) { close(); return juce::Result::fail(e.what()); }
}
juce::Result EditJournal::append(const EditDelta& delta, const RecorderProject& result, JournalKind kind)
{
    using namespace recovery;
    try
    {
        require(lock != nullptr, "Edit journal is closed");
        if (kind == JournalKind::EditTransaction && result.media->takes.size() > current.media->takes.size()) kind = JournalKind::TakePlacement;
        require(kind == JournalKind::EditTransaction || kind == JournalKind::TakePlacement || kind == JournalKind::RetakeVersionSwitch, "Invalid edit transaction kind");
        const auto p = payload(current, delta, result); const auto next = apply(current, p);
        check(journal.appendEditRecord(kind, p, juce::Uuid(delta.transactionId), options.hook)); current = next; return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(e.what()); }
}
juce::Result EditJournal::appendRegistry(const RecorderProject& result)
{
    using namespace recovery;
    try
    {
        if (hash(current) == hash(result)) return juce::Result::ok();
        EditDelta d; d.projectId = result.projectId; d.baseRevision = current.editRevision; d.revision = result.editRevision;
        d.name = "media registry"; d.validationHash = RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(result));
        const auto p = payload(current, d, result); const auto next = apply(current, p, true);
        check(journal.appendEditRecord(JournalKind::MediaRegistry, p, juce::Uuid(d.transactionId), options.hook)); current = next; return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(e.what()); }
}
juce::Result EditJournal::checkpoint()
{
    using namespace recovery;
    try
    {
        require(lock != nullptr, "Edit journal is closed"); check(journal.rotate());
        auto next = cursor; next.generation = (std::max)(Sample{1}, cursor.generation);
        next.journalSegment = journal.currentSegment(); next.journalSequence = journal.durableSequence();
        auto file = child(root, "project.recorder");
        RecorderProject primary;
        const bool preserveDamagedPrimary = file.existsAsFile() && RecorderSerializer::fromJson(file.loadFileAsString(), primary).failed();
        if (preserveDamagedPrimary) file = child(root, "recovery/" + juce::Uuid().toDashedString() + "/project.recorder");
        check(RecorderSerializer::writeCheckpoint(file, current, next, options.faults, options.hook));
        auto p = object(); set(p, "projectId", current.projectId); set(p, "revision", integer(current.editRevision));
        set(p, "generation", integer(next.generation)); set(p, "checksum", juce::JSON::parse(RecorderSerializer::toJson(current, next))["checksum"]);
        set(p, "journalPath", next.journalPath); set(p, "segment", integer(next.journalSegment)); set(p, "sequence", integer(static_cast<Sample>(next.journalSequence)));
        hit(options.hook, "checkpoint-before-commit");
        check(journal.appendEditRecord(JournalKind::EditCheckpoint, p)); hit(options.hook, "checkpoint-after-commit");
        if (preserveDamagedPrimary)
        {
            // Keep a corrupt original checkpoint intact. Publish a new discoverable
            // recovery checkpoint, linked to the same append-only continuation.
            auto commit = object(); set(commit, "checkpoint", file.getRelativePathFrom(root).replaceCharacter('\\', '/'));
            set(commit, "projectId", current.projectId); set(commit, "sha256", sha256(file)); set(commit, "files", juce::Array<juce::var>{});
            set(commit, "lastSavedEditRevision", integer(current.editRevision)); set(commit, "recoveredTakeIds", juce::Array<juce::var>{});
            set(commit, "messages", juce::Array<juce::var>{}); set(commit, "takes", juce::Array<juce::var>{});
            Log published(options.faults); published.open(file.getParentDirectory().getChildFile("commit.log"), true);
            published.append(Kind::recovery, commit); published.close();
        }
        cursor = next; committedGenerations.emplace(next.generation, p["checksum"].toString());
        RecorderProject previous; CheckpointInfo prior;
        const auto backup = file.getSiblingFile(file.getFileName() + ".bak");
        if (RecorderSerializer::readCheckpoint(backup, previous, &prior, false).wasOk()
            && prior.generation < next.generation && committedGenerations.count(prior.generation)
            && committedGenerations.at(prior.generation) == juce::JSON::parse(backup.loadFileAsString())["checksum"].toString()
            && prior.journalPath == next.journalPath && previous.projectId == current.projectId)
        {
            // Both checkpoints and their commits are verified. Keep every segment
            // needed by .bak; a crash during GC can always start at that cursor.
            hit(options.hook, "checkpoint-before-collect");
            for (const auto& candidate : directory.findChildFiles(juce::File::findFiles, false, "edits-*.log"))
            {
                const auto n = candidate.getFileNameWithoutExtension().substring(6).getLargeIntValue();
                if (n > 0 && n < prior.journalSegment && candidate == RecordingJournal::editLogFile(directory, static_cast<unsigned>(n)))
                { child(root, candidate.getRelativePathFrom(root).replaceCharacter('\\', '/')); require(candidate.deleteFile(), "Could not collect old edit segment"); ++collected; }
            }
        }
        return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(e.what()); }
}
juce::Result EditJournal::close()
{ const auto r = journal.close(); lock.reset(); return r; }

EditJournalWorker::EditJournalWorker(const juce::File& r, RecorderDocument::Snapshot p, CheckpointInfo c)
    : EditJournalWorker(r, std::move(p), std::move(c), Options{}) {}
EditJournalWorker::EditJournalWorker(const juce::File& r, RecorderDocument::Snapshot p, CheckpointInfo c, Options o)
    : options(std::move(o)), root(r), initial(std::move(p)), cursor(std::move(c)), worker([this] { run(); }) {}
EditJournalWorker::~EditJournalWorker() { shutdown(); detach(); }
void EditJournalWorker::attach(RecorderDocument& d)
{
    jassert(owner == std::this_thread::get_id()); detach(); document = &d; document->setJournalSink(this);
    if (auto* m = juce::MessageManager::getInstanceWithoutCreating(); m && m->isThisTheMessageThread()) startTimer(25);
}
void EditJournalWorker::detach()
{ jassert(owner == std::this_thread::get_id()); stopTimer(); if (document) document->setJournalSink(nullptr); document = nullptr; }
juce::Result EditJournalWorker::enqueue(const EditDelta&)
{ return juce::Result::fail("EditJournalWorker requires the resulting immutable snapshot"); }
juce::Result EditJournalWorker::enqueue(const EditDelta& d, RecorderDocument::Snapshot state)
{ auto copy = d; for (auto& entity : copy.entities) entity.value = entity.value.clone(); return push({std::move(copy), std::move(state), false, false}); }
juce::Result EditJournalWorker::enqueueRegistry(RecorderDocument::Snapshot state)
{ return push({{}, std::move(state), true, false}); }
juce::Result EditJournalWorker::requestCheckpoint() { return push({{}, {}, false, true}); }
juce::Result EditJournalWorker::checkpointAndWait()
{
    const auto queued = requestCheckpoint(); if (queued.failed()) return queued;
    const auto done = waitUntilIdle(); drain(); return done;
}
juce::Result EditJournalWorker::push(Work w)
{
    const std::lock_guard<std::mutex> guard(mutex);
    if (error.failed()) return error;
    if (stopping) return juce::Result::fail("Journal is shutting down");
    if (pending.size() + completions.size() >= options.maxPending) return juce::Result::fail("Edit journal queue full; edit remains unsaved");
    pending.push_back(std::move(w)); wake.notify_one(); return juce::Result::ok();
}
juce::Result EditJournalWorker::waitUntilIdle(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> guard(mutex);
    if (!idle.wait_for(guard, timeout, [&] { return finished || (!busy && pending.empty()); })) return juce::Result::fail("Timed out waiting for edit journal");
    return error;
}
juce::Result EditJournalWorker::shutdown()
{
    { const std::lock_guard<std::mutex> guard(mutex); stopping = true; wake.notify_one(); }
    if (worker.joinable()) worker.join();
    if (owner == std::this_thread::get_id()) drain();
    const std::lock_guard<std::mutex> guard(mutex); return error;
}
void EditJournalWorker::drain()
{
    jassert(owner == std::this_thread::get_id()); std::deque<Completion> done;
    { const std::lock_guard<std::mutex> guard(mutex); done.swap(completions); }
    if (!document) return;
    for (const auto& c : done)
    {
        if (c.work.checkpoint) document->checkpointFinished(c.work.state, root.getChildFile("project.recorder"), c.result);
        else
        {
            if (!c.work.registry) document->acknowledgeJournal(c.work.delta, c.result);
            document->acknowledgeJournalState(c.work.state, c.result);
        }
    }
}
void EditJournalWorker::run()
{
    EditJournal journal(options.journal); auto result = initial ? journal.open(root, *initial, cursor) : juce::Result::fail("Missing initial document");
    auto nextCheckpoint = std::chrono::steady_clock::now() + options.checkpointInterval;
    bool dirty = false;
    for (;;)
    {
        Work work; bool stop = false;
        {
            std::unique_lock<std::mutex> guard(mutex);
            if (result.failed() && error.wasOk()) error = result;
            busy = false; idle.notify_all();
            wake.wait_until(guard, nextCheckpoint, [&] { return stopping || !pending.empty(); });
            stop = stopping && pending.empty();
            if (!pending.empty()) { work = std::move(pending.front()); pending.pop_front(); }
            else if (dirty && (stop || std::chrono::steady_clock::now() >= nextCheckpoint)) work.checkpoint = true;
            else if (stop) break;
            else { nextCheckpoint = std::chrono::steady_clock::now() + options.checkpointInterval; continue; }
            busy = true;
        }
        if (work.checkpoint)
        {
            work.state = std::make_shared<const RecorderProject>(journal.durableProject());
            if (result.wasOk()) result = journal.checkpoint();
            dirty = false; nextCheckpoint = std::chrono::steady_clock::now() + options.checkpointInterval;
        }
        else if (result.wasOk())
        {
            result = work.registry ? journal.appendRegistry(*work.state) : journal.append(work.delta, *work.state);
            dirty = result.wasOk();
        }
        { const std::lock_guard<std::mutex> guard(mutex); completions.push_back({std::move(work), result}); }
        if (stop) break;
    }
    const auto closed = journal.close(); if (result.wasOk()) result = closed;
    { const std::lock_guard<std::mutex> guard(mutex); error = result; busy = false; finished = true; idle.notify_all(); }
}
}
