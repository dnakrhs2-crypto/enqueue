#include "RecoveryScanner.h"
#include "Mp4RecoveryIndex.h"
#include "StorageEncoding.h"
#include "EditJournal.h"
#include "record/Mp4TakeWriter.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>

namespace gocue::recorder
{
namespace
{
using namespace recovery;
juce::String id(const juce::var& v)
{ require(v.isString() && !juce::Uuid(v.toString()).isNull(), "Invalid recovery UUID"); return juce::Uuid(v.toString()).toString(); }
std::uint32_t u32(const juce::var& v)
{ const auto n = number(v); require(n >= 0 && n <= UINT32_MAX, "Recovery coordinate exceeds uint32 schema field"); return std::uint32_t(n); }
juce::String rel(const juce::File& root, const juce::File& file) { return file.getRelativePathFrom(root).replaceCharacter('\\', '/'); }
std::vector<Id> assetIds(const Take& t)
{
    auto ids = t.microphoneAssetIds; if (t.cam1AssetId.isNotEmpty()) ids.push_back(t.cam1AssetId); if (t.cam2AssetId.isNotEmpty()) ids.push_back(t.cam2AssetId); return ids;
}
std::vector<juce::String> paths(const MediaAsset& a)
{
    std::vector<juce::String> out; if (a.relativePath.isNotEmpty()) out.push_back(a.relativePath);
    for (const auto& c : a.chunks) out.push_back(c.relativePath); return out;
}
juce::var fileEntry(const juce::File& root, const juce::String& path)
{
    auto v = object(); const auto file = child(root, path); require(file.existsAsFile(), "Missing file: " + path);
    set(v, "path", path); set(v, "bytes", integer(file.getSize())); set(v, "sha256", sha256(file)); return v;
}
bool verifyFiles(const juce::File& root, const juce::var& files, bool hash)
{
    if (!files.isArray()) return false;
    for (const auto& v : *files.getArray())
    {
        const auto file = child(root, v["path"].toString());
        if (!file.existsAsFile() || file.getSize() != number(v["bytes"]) || (hash && sha256(file) != v["sha256"].toString())) return false;
    }
    return true;
}
bool manifest(const juce::File& root, const Id& takeId, RecorderProject& out, const RecorderProject* known = nullptr)
{
    try
    {
    const auto file = child(root, "media/takes/" + juce::Uuid(takeId).toDashedString() + "/take.json");
    if (!file.existsAsFile()) return false;
    auto v = juce::JSON::parse(file.loadFileAsString()); if (!v.isObject()) return false;
    if (!v.hasProperty("checksum"))
    {
        // Round-10 product manifests predate the round-07 recovery envelope.
        // Bind their IDs/origins and exact media lengths to the checksummed
        // registry; never reinterpret an arbitrary JSON file as a finished take.
        if (!known || number(v["schemaVersion"]) != 1 || id(v["takeId"]) != takeId || v["state"].toString() != "done") return false;
        const auto* take = known->media->findTake(takeId);
        if (!take || take->cam1AssetId != v["cameraAssetId"].toString()
            || number(v["Fs"]) != known->Fs || number(v["fpsNumerator"]) != known->fps.numerator || number(v["fpsDenominator"]) != known->fps.denominator
            || number(v["N0"]) != take->N0 || number(v["Nstop"]) - take->N0 != take->logicalLength
            || number(v["placementSample"]) != take->placementSample || number(v["logicalLength"]) != take->logicalLength) return false;
        const auto cameras = v["cameras"];
        if (take->cam2AssetId.isNotEmpty() && (!cameras.isArray() || cameras.size() != 2)) return false;
        if (cameras.isArray())
        {
            if (cameras.size() != (take->cam2AssetId.isEmpty() ? 1 : 2)) return false;
            for (int i = 0; i < cameras.size(); ++i)
                if (number(cameras[i]["slot"]) != i + 1 || cameras[i]["assetId"].toString() != (i ? take->cam2AssetId : take->cam1AssetId)
                    || number(cameras[i]["N0"]) != take->N0 || number(cameras[i]["Nstop"]) != take->N0 + take->logicalLength) return false;
        }
        const auto microphones = v["microphones"]; if (!microphones.isArray() || microphones.size() != static_cast<int>(take->microphoneAssetIds.size())) return false;
        for (int i = 0; i < microphones.size(); ++i)
        {
            const auto index = static_cast<size_t>(i); const auto m = microphones[i];
            const auto right = take->capture.physicalInputsRight.empty() ? -1 : take->capture.physicalInputsRight[index];
            if (m["assetId"].toString() != take->microphoneAssetIds[index] || number(m["physicalIndex"]) != take->capture.physicalInputs[index]) return false;
            if (right >= 0 && (!m.hasProperty("rightPhysical") || !m.hasProperty("channels"))) return false;
            if (m.hasProperty("rightPhysical") && number(m["rightPhysical"]) != right) return false;
            if (m.hasProperty("leftPhysical") && number(m["leftPhysical"]) != take->capture.physicalInputs[index]) return false;
            if (m.hasProperty("channels") && number(m["channels"]) != (right >= 0 ? 2 : 1)) return false;
        }
        auto registry = std::make_shared<MediaRegistry>(*known->media);
        for (auto& asset : registry->assets)
        {
            if (asset.assetId == take->cam1AssetId || asset.assetId == take->cam2AssetId)
            {
                const bool second = asset.assetId == take->cam2AssetId;
                const auto video = cameras.isArray() ? cameras[second ? 1 : 0]["video"] : v["video"];
                const auto path = "media/takes/" + juce::Uuid(takeId).toDashedString() + (second ? "/cam2.mp4" : "/cam1.mp4");
                const auto media = child(root, path);
                if (!bool(video["mux"]["finalized"]) || !media.existsAsFile() || media.getSize() != number(video["mux"]["fileSizeBytes"])) return false;
                const auto inspection = Mp4TakeWriter::inspect(media);
                if (!bool(inspection["presentationStartsAtZero"]) || RecorderSerializer::fingerprint(inspection["streams"]) != RecorderSerializer::fingerprint(video["inspection"]["streams"])) return false;
                if (asset.relativePath != path) { if (asset.mediaGeneration == INT64_MAX) return false; ++asset.mediaGeneration; asset.relativePath = path; }
            }
            else if (std::find(take->microphoneAssetIds.begin(), take->microphoneAssetIds.end(), asset.assetId) != take->microphoneAssetIds.end())
            {
                if (asset.chunks.empty()) return false;
                for (const auto& chunk : asset.chunks)
                {
                    const auto wav = child(root, chunk.relativePath); juce::FileInputStream in(wav); std::array<std::uint8_t, 44> h{};
                    if (in.failedToOpen() || in.read(h.data(), 44) != 44 || std::memcmp(h.data(), "RIFF", 4) || std::memcmp(h.data() + 36, "data", 4)) return false;
                    const auto channels = asset.originalFormat.channels;
                    if ((channels != 1 && channels != 2) || storageEncoding::get<std::uint16_t>(h.data() + 22) != channels
                        || storageEncoding::get<std::uint16_t>(h.data() + 32) != channels * 3
                        || storageEncoding::get<std::uint32_t>(h.data() + 24) != asset.originalFormat.sampleRate
                        || storageEncoding::get<std::uint16_t>(h.data() + 34) != 24) return false;
                    const auto bytes = std::uint64_t(chunk.sourceRange.length) * channels * 3;
                    if (bytes > UINT32_MAX || storageEncoding::get<std::uint32_t>(h.data() + 40) != bytes || wav.getSize() != static_cast<juce::int64>(44 + bytes + (bytes & 1))) return false;
                }
            }
        }
        for (auto& item : registry->takes) if (item.takeId == takeId) item.state = TakeState::complete;
        out = *known; out.media = registry; return out.validate().wasOk();
    }
    const auto hash = v["checksum"].toString(); v.getDynamicObject()->removeProperty("checksum");
    if (number(v["schemaVersion"]) != 1 || id(v["takeId"]) != takeId || hash != RecorderSerializer::fingerprint(v)
        || !verifyFiles(root, v["files"], false) || RecorderSerializer::fromJson(v["project"].toString(), out).failed()) return false;
    const auto* take = out.media->findTake(takeId); if (!take || take->state != TakeState::complete) return false;
    std::set<juce::String> expected, actual;
    for (const auto& aid : assetIds(*take)) for (const auto& p : paths(*out.media->findAsset(aid))) expected.insert(p);
    for (const auto& f : *v["files"].getArray()) if (!actual.insert(f["path"].toString()).second) return false;
    return expected == actual && !actual.empty();
    }
    catch (const std::exception&) { return false; }
}
void resign(juce::var& v)
{ v.getDynamicObject()->removeProperty("checksum"); set(v, "checksum", RecorderSerializer::fingerprint(v)); }
RecorderProject apply(const RecorderProject& before, const juce::var& record)
{
    RecorderProject expected; check(RecorderSerializer::fromJson(record["result"].toString(), expected));
    require(expected.projectId == before.projectId && number(record["baseRevision"]) == before.editRevision
        && expected.editRevision == before.editRevision + 1 && number(record["revision"]) == expected.editRevision, "Edit revision/project chain mismatch");
    auto root = juce::JSON::parse(RecorderSerializer::toJson(before)); auto edit = root["edit"];
    const auto deltas = record["entities"]; require(deltas.isArray(), "Edit delta array missing");
    const std::map<juce::String, juce::String> fields{{"tracks", "trackId"}, {"markers", "markerId"}, {"linkGroups", "linkGroupId"}, {"takeStacks", "stackId"}};
    for (const auto& d : *deltas.getArray())
    {
        const auto collection = d["collection"].toString(), entity = d["id"].toString(); const auto value = d["value"];
        require(d["removed"].isBool() && isId(entity), "Invalid entity delta"); const bool removed = bool(d["removed"]);
        if (collection == "project")
        { require(!removed && entity == before.projectId && value.isString(), "Invalid project delta"); set(edit, "name", value); continue; }
        const bool order = collection.endsWith("Order"); const auto name = order ? collection.dropLastCharacters(5) : collection;
        const auto key = fields.find(name); require(key != fields.end(), "Unknown edit collection");
        auto list = edit.getProperty(juce::Identifier(name), {}).getArray(); require(list != nullptr, "Missing edit collection");
        if (order)
        {
            require(!removed && entity == before.projectId && value.isArray() && value.size() == list->size(), "Invalid collection order");
            juce::Array<juce::var> next; std::set<Id> seen;
            for (const auto& v : *value.getArray())
            {
                require(seen.insert(v.toString()).second, "Duplicate ordered entity"); bool found = false;
                for (const auto& item : *list) if (item.getProperty(juce::Identifier(key->second), {}).toString() == v.toString()) { next.add(item); found = true; break; }
                require(found, "Unknown ordered entity");
            }
            edit.getDynamicObject()->setProperty(name, next);
        }
        else
        {
            int found = -1; for (int i = 0; i < list->size(); ++i) if ((*list)[i].getProperty(juce::Identifier(key->second), {}).toString() == entity) { found = i; break; }
            if (removed) { require(found >= 0, "Removal of missing entity"); list->remove(found); }
            else { require(value.isObject() && value.getProperty(juce::Identifier(key->second), {}).toString() == entity, "Delta entity ID mismatch"); if (found >= 0) list->set(found, value); else list->add(value); }
        }
    }
    require(RecorderSerializer::fingerprint(edit) == record["validationHash"].toString(), "Edit validation hash mismatch");
    set(root, "media", juce::JSON::parse(RecorderSerializer::toJson(expected))["media"]);
    set(root, "editRevision", integer(expected.editRevision)); set(root, "checkpointRevision", integer(expected.editRevision)); resign(root);
    RecorderProject result; check(RecorderSerializer::fromJson(juce::JSON::toString(root), result));
    require(RecorderSerializer::toJson(result) == RecorderSerializer::toJson(expected), "Delta/result mismatch"); return result;
}
struct TakeLog
{
    juce::var start;
    std::map<juce::String, JournalFilePosition> positions;
    bool stopped = false, finalized = false;
    Sample stop = 0;
    Id placementTransaction;
};
void fillGaps(MediaAsset& a, Sample length)
{
    a.logicalLength = length; a.gaps.clear();
    std::sort(a.availableRanges.begin(), a.availableRanges.end(), [](auto x, auto y) { return x.start < y.start; });
    std::vector<SampleRange> merged;
    for (const auto& r : a.availableRanges)
    {
        require(r.start >= 0 && r.length > 0 && r.start <= length && r.length <= length - r.start, "Recovery range outside take");
        if (!merged.empty() && r.start == merged.back().start + merged.back().length) merged.back().length += r.length;
        else { require(merged.empty() || r.start >= merged.back().start + merged.back().length, "Overlapping recovered ranges"); merged.push_back(r); }
    }
    a.availableRanges = merged; Sample end = 0;
    for (const auto& r : merged) { if (r.start > end) a.gaps.push_back({end, r.start - end}); end = r.start + r.length; }
    if (end < length) a.gaps.push_back({end, length - end});
}
std::uint64_t copyWav(const juce::File& source, const juce::File& dest, const JournalPcmFormat& fmt,
                      const JournalFilePosition& pos, FileIoFaultAdapter* faults)
{
    require((fmt.channels == 1 || fmt.channels == 2) && fmt.bitsPerSample == 24 && fmt.blockAlign == fmt.channels * 3 && pos.blockAlign == fmt.blockAlign
        && pos.dataOffset == fmt.dataOffset && fmt.dataOffset >= 44 && fmt.sampleRate > 0, "Journal WAV fmt/offset mismatch");
    juce::FileInputStream input(source); check(input.getStatus());
    // RIFF length/data length may be torn. The durable journal, stable fmt
    // fields, actual length and block alignment are the authority.
    std::array<std::uint8_t, 36> old{};
    require(input.read(old.data(), int(old.size())) == old.size(), "Truncated WAV fmt");
    require(std::memcmp(old.data(), "RIFF", 4) == 0 && std::memcmp(old.data() + 8, "WAVEfmt ", 8) == 0
        && storageEncoding::get<std::uint32_t>(old.data() + 16) == 16 && storageEncoding::get<std::uint16_t>(old.data() + 20) == 1
        && storageEncoding::get<std::uint16_t>(old.data() + 22) == fmt.channels && storageEncoding::get<std::uint32_t>(old.data() + 24) == fmt.sampleRate
        && storageEncoding::get<std::uint16_t>(old.data() + 32) == fmt.blockAlign && storageEncoding::get<std::uint16_t>(old.data() + 34) == 24, "WAV fmt differs from journal");
    std::array<std::uint8_t, 8> dataHeader{};
    require(input.setPosition(fmt.dataOffset - 8) && input.read(dataHeader.data(), 8) == 8 && std::memcmp(dataHeader.data(), "data", 4) == 0, "WAV data offset differs from journal");
    const auto samples = RecoveryScanner::wavSamples(std::uint64_t(input.getTotalLength()), pos);
    const auto bytes = samples * fmt.blockAlign; require(bytes <= UINT32_MAX - 37, "WAV chunk exceeds RIFF; RF64 export remains separate");
    std::array<std::uint8_t, 44> h{}; std::memcpy(h.data(), old.data(), old.size());
    storageEncoding::put(h.data() + 4, std::uint32_t(36 + bytes + (bytes & 1)));
    storageEncoding::put(h.data() + 28, fmt.sampleRate * fmt.blockAlign); std::memcpy(h.data() + 36, "data", 4); storageEncoding::put(h.data() + 40, std::uint32_t(bytes));
    check(dest.getParentDirectory().createDirectory()); DurableFile output(faults); check(output.open(dest, DurableFile::OpenMode::createNew)); check(output.write(h.data(), h.size()));
    require(input.setPosition(pos.dataOffset), "Seek WAV data offset"); std::array<std::uint8_t, 3 * 16384> buffer{};
    for (std::uint64_t left = bytes; left;)
    {
        const int n = int(std::min<std::uint64_t>(left, buffer.size())); require(input.read(buffer.data(), n) == n, "Short WAV PCM read"); check(output.write(buffer.data(), std::size_t(n))); left -= std::uint64_t(n);
    }
    if (bytes & 1) { const std::uint8_t pad = 0; check(output.write(&pad, 1)); }
    check(input.getStatus()); check(output.flushData()); check(output.close()); return samples;
}
}
juce::var RecoveryReport::toJson() const
{
    auto v = recovery::object();
    recovery::set(v, "schemaVersion", 1); recovery::set(v, "checkpoint", checkpoint.getFullPathName()); recovery::set(v, "attempt", attempt.getFullPathName());
    recovery::set(v, "changedTakes", recovery::integer(std::int64_t(changedTakes))); recovery::set(v, "addedClips", recovery::integer(std::int64_t(addedClips)));
    recovery::set(v, "lastSavedEditRevision", recovery::integer(lastSavedEditRevision)); recovery::set(v, "resultRevision", recovery::integer(project.editRevision));
    recovery::set(v, "duplicateTransactions", recovery::integer(std::int64_t(duplicateTransactions))); recovery::set(v, "ignoredEditTail", ignoredEditTail); recovery::set(v, "ignoredTakeTail", ignoredTakeTail);
    recovery::set(v, "usedBackup", usedBackup); recovery::set(v, "messages", juce::var(messages)); recovery::set(v, "warnings", juce::var(warnings)); recovery::set(v, "orphans", juce::var(orphans)); recovery::set(v, "takes", takes);
    recovery::set(v, "checkpointGeneration", recovery::integer(checkpointInfo.generation));
    recovery::set(v, "editJournalPath", checkpointInfo.journalPath);
    recovery::set(v, "replayedEdits", recovery::integer(static_cast<Sample>(replayedEdits)));
    recovery::set(v, "registryCommits", recovery::integer(static_cast<Sample>(registryCommits)));
    recovery::set(v, "powerLoss", juce::String::fromUTF8("미확인 → 스파이크 4")); return v;
}
std::uint64_t RecoveryScanner::wavSamples(std::uint64_t actual, const JournalFilePosition& pos)
{
    recovery::require(pos.blockAlign > 0 && pos.validBytes >= pos.dataOffset, "Invalid durable WAV coordinates");
    const auto bytes = std::min(actual, pos.validBytes);
    return bytes < pos.dataOffset ? 0 : std::min(pos.validSamples, (bytes - pos.dataOffset) / pos.blockAlign);
}
void RecoveryScanner::appendEdit(const juce::File& file, const EditDelta& delta, const RecorderProject& result, FileIoFaultAdapter* faults, const recovery::Hook& hook)
{
    using namespace recovery; check(result.validate());
    require(isId(delta.transactionId) && delta.projectId == result.projectId && delta.revision == result.editRevision
        && delta.baseRevision >= 0 && delta.baseRevision < INT64_MAX && delta.revision == delta.baseRevision + 1
        && delta.validationHash == RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(result)), "Invalid durable edit ticket");
    auto v = object(); set(v, "projectId", delta.projectId); set(v, "baseRevision", integer(delta.baseRevision)); set(v, "revision", integer(delta.revision));
    set(v, "validationHash", delta.validationHash); set(v, "name", delta.name); set(v, "result", RecorderSerializer::toJson(result));
    juce::Array<juce::var> entities;
    for (const auto& d : delta.entities) { auto x = object(); set(x, "collection", d.collection); set(x, "id", d.entityId); set(x, "removed", d.removed); set(x, "value", d.value); entities.add(x); }
    set(v, "entities", entities); Log log(faults); log.open(file); log.append(Kind::edit, v, juce::Uuid(delta.transactionId), hook); log.close();
}
void RecoveryScanner::writeCheckpoint(const juce::File& file, const RecorderProject& project, FileIoFaultAdapter* faults, const recovery::Hook& hook)
{
    CheckpointInfo written; recovery::check(RecorderSerializer::writeCheckpoint(file, project, written, faults, hook));
}
void RecoveryScanner::writeTakeManifest(const juce::File& root, const RecorderProject& project, const Id& takeId)
{
    using namespace recovery; check(project.validate()); const auto* take = project.media->findTake(takeId); require(take != nullptr, "Manifest take missing");
    auto v = object(); set(v, "schemaVersion", 1); set(v, "takeId", takeId); juce::Array<juce::var> files;
    for (const auto& asset : assetIds(*take)) for (const auto& path : paths(*project.media->findAsset(asset))) files.add(fileEntry(root, path));
    set(v, "files", files); set(v, "project", RecorderSerializer::toJson(project)); set(v, "checksum", RecorderSerializer::fingerprint(v));
    writeJson(child(root, "media/takes/" + juce::Uuid(takeId).toDashedString() + "/take.json"), v);
}
juce::Result RecoveryScanner::run(const juce::File& root, RecoveryReport& report, RecorderDocument* document)
{
    using namespace recovery; report = {};
    try
    {
        require(root.isDirectory(), "Project folder does not exist");
        WriterLock projectLock(child(root, "project.writer.lock")); WriterLock takeLock(child(root, "journal/takes.writer.lock"));
        const auto primary = child(root, "project.recorder"), backup = child(root, "project.recorder.bak");
        bool selected = false; std::set<Id> committedRecoveryTakes; juce::var chosenCommit;
        Sample maximumGeneration = 0;
        std::vector<std::pair<RecorderProject, juce::var>> recoveryCandidates;
        const auto choose = [&](const juce::File& file, bool isBackup, const juce::var& commit)
        {
            if (!file.existsAsFile()) return;
            RecorderProject candidate; CheckpointInfo candidateInfo; const auto r = RecorderSerializer::fromJson(file.loadFileAsString(), candidate, &candidateInfo);
            candidateInfo.sourceFile = file;
            if (r.failed()) { report.warnings.add("Invalid checkpoint: " + rel(root, file) + ": " + r.getErrorMessage()); return; }
            if (selected && candidate.projectId != report.project.projectId) { report.warnings.add("Checkpoint project UUID mismatch: " + rel(root, file)); return; }
            maximumGeneration = (std::max)(maximumGeneration, candidateInfo.generation);
            if (!selected || candidateInfo.generation > report.checkpointInfo.generation
                || (candidateInfo.generation == report.checkpointInfo.generation && candidate.editRevision > report.project.editRevision))
            { report.project = candidate; report.checkpointInfo = candidateInfo; report.checkpoint = file; report.usedBackup = isBackup; selected = true; chosenCommit = commit; }
            // Primary wins a same-revision backup tie: media finalization can
            // update registry state without a new user edit revision (round 08).
            else if (candidateInfo.generation == report.checkpointInfo.generation && candidate.editRevision == report.project.editRevision && !isBackup)
                require(RecorderSerializer::toJson(candidate) == RecorderSerializer::toJson(report.project), "Conflicting checkpoints at the same revision");
        };
        choose(primary, false, {}); choose(backup, true, {});
        const auto recoveryRoot = child(root, "recovery");
        for (const auto& dir : recoveryRoot.findChildFiles(juce::File::findDirectories, false))
        {
            child(root, rel(root, dir)); const auto log = dir.getChildFile("commit.log");
            bool valid = false;
            const auto replay = Log::read(log, [&](const Record& record)
            {
                if (record.kind != Kind::recovery || record.sequence != 1) return false;
                const auto& p = record.payload; const auto file = child(root, p["checkpoint"].toString());
                if (file.getParentDirectory() != dir || !file.existsAsFile() || sha256(file) != p["sha256"].toString() || !verifyFiles(root, p["files"], true)) return false;
                RecorderProject candidate; if (RecorderSerializer::fromJson(file.loadFileAsString(), candidate).failed()) return false;
                if (selected && candidate.projectId != report.project.projectId) return false;
                choose(file, false, p);
                recoveryCandidates.emplace_back(candidate, p);
                valid = true; return true;
            });
            if (!valid || replay.ignoredTail) report.orphans.add(rel(root, dir) + "/ (uncommitted recovery attempt)");
        }
        if (!selected)
        {
            report.warnings.add("No valid checksum/schema checkpoint; originals preserved");
            report.attempt = child(root, "recovery/" + juce::Uuid().toDashedString()); writeJson(report.attempt.getChildFile("diagnostic.json"), report.toJson(), options.faults);
            throw std::runtime_error("No valid checksum/schema checkpoint; originals preserved");
        }
        if (chosenCommit.isObject())
        {
            if (const auto* list = chosenCommit["recoveredTakeIds"].getArray()) for (const auto& v : *list) committedRecoveryTakes.insert(id(v));
            if (const auto* list = chosenCommit["messages"].getArray()) for (const auto& v : *list) report.messages.add(v.toString());
            if (const auto* list = chosenCommit["takes"].getArray()) report.takes = *list;
        }
        // A newer primary can include earlier recovered asset generations. Only
        // suppress recovery when those exact generations are still connected.
        for (const auto& candidate : recoveryCandidates)
            if (const auto* ids = candidate.second["recoveredTakeIds"].getArray()) for (const auto& v : *ids)
            {
                const auto takeId = id(v); const auto* take = report.project.media->findTake(takeId);
                const auto* previous = candidate.first.media->findTake(takeId);
                bool connected = take && previous && assetIds(*take) == assetIds(*previous);
                if (connected) for (const auto& aid : assetIds(*take))
                {
                    const auto* a = report.project.media->findAsset(aid); const auto* b = candidate.first.media->findAsset(aid);
                    connected &= a && b && a->mediaGeneration == b->mediaGeneration && paths(*a) == paths(*b);
                }
                if (connected) committedRecoveryTakes.insert(takeId);
            }
        report.lastSavedEditRevision = chosenCommit.isObject() ? number(chosenCommit["lastSavedEditRevision"]) : report.project.editRevision;
        const auto selectedRevision = report.project.editRevision;
        std::set<Id> transactions; unsigned segment = 0;
        EditJournalReplay native; check(EditJournal::replay(root, report.project, report.checkpointInfo, native));
        report.ignoredEditTail = native.framing.ignoredTail; report.replayedEdits = native.appliedEdits; report.registryCommits = native.registryCommits;
        report.duplicateTransactions += native.framing.duplicateTransactions; transactions = native.transactions;
        if (report.ignoredEditTail) report.warnings.add("Edit tail ignored: " + native.framing.tailReason);
        if (native.appliedEdits) report.lastSavedEditRevision = report.project.editRevision;
        auto edits = native.legacy ? child(root, report.checkpointInfo.journalPath).findChildFiles(juce::File::findFiles, false, "edits-*.log") : juce::Array<juce::File>{}; edits.sort();
        for (const auto& file : edits)
        {
            const auto expected = "edits-" + juce::String(++segment).paddedLeft('0', 6) + ".log";
            if (file.getFileName() != expected) { report.ignoredEditTail = true; report.warnings.add("Edit journal segment gap"); break; }
            const auto replay = Log::read(file, [&](const Record& record)
            {
                try
                {
                    if (record.kind != Kind::edit || record.payload["projectId"].toString() != report.project.projectId) return false;
                    RecorderProject expectedResult; check(RecorderSerializer::fromJson(record.payload["result"].toString(), expectedResult));
                    require(expectedResult.editRevision == number(record.payload["revision"]) && expectedResult.projectId == report.project.projectId, "Invalid edit result");
                    if (!transactions.insert(record.transaction.toString()).second) { ++report.duplicateTransactions; return true; }
                    if (expectedResult.editRevision <= report.project.editRevision) return true;
                    report.project = apply(report.project, record.payload); report.lastSavedEditRevision = report.project.editRevision; ++report.replayedEdits; return true;
                }
                catch (const std::exception& e) { transactions.erase(record.transaction.toString()); report.warnings.add(juce::String::fromUTF8(e.what())); return false; }
            });
            report.duplicateTransactions += replay.duplicates;
            if (replay.ignoredTail) { report.ignoredEditTail = true; report.warnings.add("Edit tail ignored: " + replay.reason); break; }
        }
        JournalReplay journal; check(RecordingJournal::replay(child(root, "journal"), journal)); report.ignoredTakeTail = journal.ignoredTail;
        report.duplicateTransactions += journal.duplicateTransactions; if (journal.ignoredTail) report.warnings.add("Take tail ignored: " + journal.tailReason);
        std::map<Id, TakeLog> started; std::vector<Id> order; std::set<juce::String> referenced;
        for (const auto& r : journal.records)
        {
            const auto takeId = id(r.payload["takeId"]);
            if (r.kind == JournalKind::TakeStarted)
            {
                if (started.count(takeId)) { report.warnings.add("Duplicate TakeStarted ignored: " + takeId); continue; }
                started[takeId].start = r.payload; order.push_back(takeId);
                for (const auto& f : *r.payload["files"].getArray())
                { referenced.insert(f["path"].toString()); if (!f["plannedPath"].toString().containsChar('{')) referenced.insert(f["plannedPath"].toString()); }
            }
            else if (started.count(takeId))
            {
                auto& t = started.at(takeId);
                if (r.kind == JournalKind::Checkpoint)
                    for (const auto& f : *r.payload["files"].getArray())
                    {
                        JournalFilePosition p; p.path = f["path"].toString(); p.validBytes = std::uint64_t(number(f["validBytes"])); p.validSamples = std::uint64_t(number(f["validSamples"]));
                        p.firstSample = std::uint64_t(number(f["firstSample"])); p.dataOffset = u32(f["dataOffset"]); p.blockAlign = u32(f["blockAlign"]);
                        p.pts = number(f["pts"]); p.ptsTimeBaseNum = u32(f["ptsTimeBaseNum"]); p.ptsTimeBaseDen = u32(f["ptsTimeBaseDen"]);
                        const auto prior = t.positions.find(p.path);
                        require(prior == t.positions.end() || (p.firstSample == prior->second.firstSample && p.validSamples >= prior->second.validSamples && p.validBytes >= prior->second.validBytes), "Regressing take checkpoint");
                        t.positions[p.path] = p; referenced.insert(p.path);
                    }
                else if (r.kind == JournalKind::TakeStopped) { t.stopped = true; t.stop = number(r.payload["Nstop"]); t.placementTransaction = id(r.payload["placementEditId"]); }
                else if (r.kind == JournalKind::TakeFinalized) t.finalized = true;
            }
        }
        // Validate completed manifests; a registered normal take is not remuxed
        // merely because recovery ran. Unknown/missing files remain diagnostics.
        std::set<Id> completed;
        for (const auto& take : report.project.media->takes)
        {
            for (const auto& aid : assetIds(take)) for (const auto& path : paths(*report.project.media->findAsset(aid)))
            { referenced.insert(path); if (path.endsWith(".mp4")) referenced.insert(rel(root, Mp4RecoveryIndex::pathFor(child(root, path)))); }
            const auto manifestPath = "media/takes/" + juce::Uuid(take.takeId).toDashedString() + "/take.json"; referenced.insert(manifestPath);
            if (take.state == TakeState::complete)
            {
                RecorderProject recorded; bool valid = manifest(root, take.takeId, recorded, &report.project) && recorded.projectId == report.project.projectId;
                if (valid)
                {
                    valid = assetIds(take) == assetIds(*recorded.media->findTake(take.takeId));
                    for (const auto& aid : assetIds(take))
                    { const auto* asset = recorded.media->findAsset(aid); valid &= asset && paths(*asset) == paths(*report.project.media->findAsset(aid)); }
                }
                if (valid) completed.insert(take.takeId); else report.warnings.add("Completed take manifest/file mismatch: " + take.takeId);
            }
        }
        auto registry = std::make_shared<MediaRegistry>(*report.project.media); report.project.media = registry;
        juce::Array<juce::var> outputFiles;
        if (const auto* prior = chosenCommit["files"].getArray()) outputFiles = *prior;
        const auto attempt = [&]() -> juce::File
        {
            if (report.attempt == juce::File()) { report.attempt = child(root, "recovery/" + juce::Uuid().toDashedString()); check(report.attempt.createDirectory()); }
            return report.attempt;
        };
        for (const auto& takeId : order)
        {
            auto& state = started.at(takeId);
            if (completed.count(takeId) || committedRecoveryTakes.count(takeId)) continue;
            const Take* existing = registry->findTake(takeId); Take take; RecorderProject finished;
            const bool restoreComplete = state.finalized && manifest(root, takeId, finished, &report.project) && finished.projectId == report.project.projectId;
            if (restoreComplete) take = *finished.media->findTake(takeId);
            else if (existing) take = *existing;
            else
            {
                take.takeId = takeId; take.number = 1; for (const auto& t : registry->takes) take.number = std::max(take.number, t.number + 1);
                take.createdAt = juce::Time::getCurrentTime().toISO8601(true); take.name = juce::String::fromUTF8("테이크 ") + juce::String(take.number).paddedLeft('0', 3);
                take.N0 = number(state.start["N0"]); take.O0 = number(state.start["O0"]); take.placementSample = number(state.start["Pstart"]);
                take.mode = state.start["placementMode"].toString() == "dub" ? TakeMode::dub : TakeMode::normal;
                if (!state.start.hasProperty("placementMode") && bool(state.start["usesOutputOrigin"]))
                    report.warnings.add("Legacy take has output-clock origin but no placement mode; preserving Pstart, using normal mode: " + takeId);
                // Pstart is an absolute timeline position, including an intentional
                // overwrite at zero. It is independent of the input/output origin.
            }
            JournalPcmFormat fmt; const auto pcm = state.start["pcm"]; fmt.sampleRate = u32(pcm["sampleRate"]); fmt.dataOffset = u32(pcm["dataOffset"]);
            require(fmt.sampleRate == report.project.Fs, "Take/project sample-rate mismatch");
            const bool fixedEnd = state.stopped || take.logicalLength > 0;
            Sample length = take.logicalLength; if (state.stopped) { const auto origin = bool(state.start["usesOutputOrigin"]) ? take.O0 : take.N0; require(state.stop >= origin, "Negative stopped duration"); length = state.stop - origin; }
            for (const auto& entry : state.positions)
            { require(entry.second.firstSample <= std::uint64_t(INT64_MAX) - entry.second.validSamples, "WAV range overflow"); if (!fixedEnd) length = std::max(length, Sample(entry.second.firstSample + entry.second.validSamples)); }
            std::vector<MediaAsset> assets; juce::Array<juce::var> cameraReports;
            int microphone = 0;
            if (restoreComplete)
            {
                for (const auto& aid : assetIds(take))
                { assets.push_back(*finished.media->findAsset(aid)); for (const auto& p : paths(assets.back())) { referenced.insert(p); outputFiles.add(fileEntry(root, p)); } }
                referenced.insert("media/takes/" + juce::Uuid(takeId).toDashedString() + "/take.json");
            }
            else
            for (const auto& f : *state.start["files"].getArray())
            {
                const auto sourcePath = f["path"].toString(); const auto source = child(root, sourcePath);
                if (!source.hasFileExtension("wav;mp4")) continue;
                MediaAsset a; a.assetId = id(f["assetId"]); a.mediaGeneration = 1; a.contentIdentity = "recovered-original:" + a.assetId;
                if (const auto* prior = registry->findAsset(a.assetId)) { a = *prior; a.mediaGeneration++; a.chunks.clear(); a.availableRanges.clear(); a.gaps.clear(); }
                if (source.hasFileExtension("wav"))
                {
                    auto slotFormat = fmt; const auto slotPcm = f.hasProperty("pcm") ? f["pcm"] : pcm;
                    slotFormat.channels = std::uint16_t(u32(slotPcm["channels"])); slotFormat.blockAlign = std::uint16_t(u32(slotPcm["blockAlign"]));
                    a.kind = AssetKind::mic; a.originalFormat.codec = "pcm_s24le"; a.originalFormat.sampleRate = fmt.sampleRate; a.originalFormat.channels = slotFormat.channels; a.originalFormat.bitsPerSample = 24;
                    a.sourceUnitsNumerator = 1; a.sourceUnitsDenominator = 1; a.relativePath.clear();
                    auto prefix = sourcePath.upToLastOccurrenceOf("/", true, false);
                    for (const auto& entry : state.positions)
                    {
                        const auto& pos = entry.second; if (!pos.path.startsWith(prefix)) continue;
                        const auto input = child(root, pos.path); if (!input.existsAsFile()) { report.warnings.add("Missing WAV chunk: " + pos.path); continue; }
                        const auto dest = attempt().getChildFile(pos.path);
                        const auto samples = copyWav(input, dest, slotFormat, pos, options.faults);
                        outputFiles.add(fileEntry(root, rel(root, dest)));
                        if (samples) { a.chunks.push_back({rel(root, dest), {Sample(pos.firstSample), Sample(samples)}}); a.availableRanges.push_back({Sample(pos.firstSample), Sample(samples)}); }
                    }
                    if (a.chunks.empty()) a.relativePath = sourcePath; // All-gap source, never read by a render plan.
                    if (!existing)
                    {
                        take.microphoneAssetIds.push_back(a.assetId); int physical = microphone, right = -1;
                        const auto logical = source.getParentDirectory().getFileName().substring(3).getIntValue();
                        for (const auto& d : *state.start["devices"].getArray()) if (number(d["mic"]) == logical)
                        { physical = int(number(d["physicalIndex"])); if (d.hasProperty("rightPhysicalIndex")) right = int(number(d["rightPhysicalIndex"])); }
                        take.capture.physicalInputs.push_back(physical); take.capture.physicalInputsRight.push_back(right);
                    }
                    ++microphone;
                }
                else
                {
                    a.kind = AssetKind::camera; const bool cam2 = source.getFileName().startsWith("cam2");
                    if (cam2) take.cam2AssetId = a.assetId; else take.cam1AssetId = a.assetId;
                    a.originalFormat.codec = "h264"; a.originalFormat.width = 1920; a.originalFormat.height = 1080; a.originalFormat.fps = report.project.fps;
                    a.sourceUnitsNumerator = report.project.fps.numerator; a.sourceUnitsDenominator = std::uint64_t(report.project.Fs) * report.project.fps.denominator;
                    auto input = source; if (!input.existsAsFile()) { const auto planned = child(root, f["plannedPath"].toString()); if (planned.existsAsFile()) input = planned; }
                    auto index = Mp4RecoveryIndex::pathFor(source); referenced.insert(rel(root, index));
                    auto details = object(); set(details, "camera", cam2 ? 2 : 1); set(details, "source", sourcePath);
                    if (input.existsAsFile())
                    {
                        try
                        {
                            const auto dest = attempt().getChildFile("media/" + takeId + "/" + (cam2 ? "cam2.mp4" : "cam1.mp4"));
                            if (!index.existsAsFile()) { index = attempt().getChildFile("index/" + takeId + (cam2 ? "-cam2.log" : "-cam1.log")); Mp4RecoveryIndex::rebuild(input, index); }
                            const auto recovered = Mp4RecoveryIndex::recover(input, index, dest, report.project.Fs, options.faults);
                            a.relativePath = rel(root, dest); a.originalFormat = recovered.format;
                            if (!fixedEnd) length = std::max(length, recovered.samples);
                            const auto end = fixedEnd ? std::min(length, recovered.samples) : recovered.samples;
                            const auto* prior = registry->findAsset(a.assetId);
                            if (prior && !prior->gaps.empty())
                            {
                                // Encoded padding/repeats cannot fill a previously
                                // committed camera gap or a failed generation.
                                for (const auto& range : prior->availableRanges)
                                    if (range.start < end) a.availableRanges.push_back({range.start, std::min(range.length, end - range.start)});
                            }
                            else if (end > 0) a.availableRanges.push_back({0, end});
                            a.sourceUnitsNumerator = a.originalFormat.fps.numerator; a.sourceUnitsDenominator = std::uint64_t(report.project.Fs) * a.originalFormat.fps.denominator;
                            outputFiles.add(fileEntry(root, a.relativePath)); set(details, "samples", integer(recovered.samples)); set(details, "videoFrames", integer(std::int64_t(recovered.videoFrames)));
                            set(details, "audioSamples", integer(std::int64_t(recovered.audioSamples))); set(details, "fragments", integer(std::int64_t(recovered.fragments))); set(details, "fullDecode", true); set(details, "ignoredTail", recovered.ignoredTail);
                        }
                        catch (const OutputError&) { throw; }
                        catch (const std::exception& e)
                        {
                            // A recovery output I/O failure must abort the transaction,
                            // never masquerade as a short camera.
                            if (options.faults) throw;
                            report.warnings.add(sourcePath + ": " + juce::String::fromUTF8(e.what())); set(details, "error", juce::String::fromUTF8(e.what()));
                        }
                    }
                    if (a.availableRanges.empty()) a.relativePath = sourcePath;
                    cameraReports.add(details);
                }
                assets.push_back(a);
            }
            if (length <= 0 || assets.empty())
            {
                report.warnings.add("No durable media range for take: " + takeId);
                require(report.project.editRevision < INT64_MAX, "Recovery revision overflow"); ++report.project.editRevision; ++report.changedTakes; committedRecoveryTakes.insert(takeId);
                auto summary = object(); set(summary, "takeId", takeId); set(summary, "noDurableMedia", true); report.takes.add(summary); continue;
            }
            take.logicalLength = length; take.state = restoreComplete ? TakeState::complete : TakeState::partial;
            juce::String message = juce::String::fromUTF8("복구 완료: 테이크 ") + juce::String(take.number).paddedLeft('0', 3);
            juce::Array<juce::var> assetReports;
            for (auto& a : assets)
            {
                fillGaps(a, length);
                if (!a.gaps.empty())
                {
                    const auto& gap = a.gaps.back(); if (gap.start + gap.length == length)
                    {
                        const auto label = a.kind == AssetKind::camera ? juce::String::fromUTF8("캠") + (a.assetId == take.cam2AssetId ? "2" : "1")
                            : juce::String::fromUTF8("마이크") + juce::String(int(std::find(take.microphoneAssetIds.begin(), take.microphoneAssetIds.end(), a.assetId) - take.microphoneAssetIds.begin()) + 1);
                        message += ", " + label + juce::String::fromUTF8(" 마지막 ") + juce::String(double(gap.length) / report.project.Fs, 1) + juce::String::fromUTF8("초 없음");
                    }
                }
                auto detail = object(); set(detail, "assetId", a.assetId); set(detail, "kind", a.kind == AssetKind::camera ? "camera" : "mic"); set(detail, "logicalSamples", integer(length));
                const auto ranges = [](const std::vector<SampleRange>& list) { juce::Array<juce::var> values; for (const auto& r : list) { auto v = object(); set(v, "start", integer(r.start)); set(v, "length", integer(r.length)); values.add(v); } return values; };
                set(detail, "availableRanges", ranges(a.availableRanges)); set(detail, "gaps", ranges(a.gaps)); juce::StringArray filePaths;
                for (const auto& p : paths(a)) { filePaths.add(p); if (p.endsWith(".mp4") && p.startsWith("media/")) referenced.insert(rel(root, Mp4RecoveryIndex::pathFor(child(root, p)))); }
                set(detail, "paths", juce::var(filePaths)); assetReports.add(detail);
                auto found = std::find_if(registry->assets.begin(), registry->assets.end(), [&](const auto& v) { return v.assetId == a.assetId; });
                if (found == registry->assets.end()) registry->assets.push_back(a); else *found = a;
            }
            auto found = std::find_if(registry->takes.begin(), registry->takes.end(), [&](const auto& v) { return v.takeId == takeId; });
            if (found == registry->takes.end()) registry->takes.push_back(take); else *found = take;
            // Existing take registration means placement (possibly undone/deleted)
            // has already been decided by the edit journal. Never recreate clips.
            if (!existing && !transactions.count(state.placementTransaction))
            {
                LinkGroup link;
                std::vector<std::pair<Id, Id>> placements;
                std::vector<Id> targets;
                for (const auto& a : assets)
                {
                    const auto kind = a.kind == AssetKind::mic ? TrackKind::mic : a.assetId == take.cam2AssetId ? TrackKind::cam2 : TrackKind::cam1;
                    int mic = -1;
                    if (kind == TrackKind::mic)
                    {
                        mic = int(std::find(take.microphoneAssetIds.begin(), take.microphoneAssetIds.end(), a.assetId) - take.microphoneAssetIds.begin());
                        for (const auto& f : *state.start["files"].getArray()) if (id(f["assetId"]) == a.assetId)
                        {
                            const auto folder = child(root, f["path"].toString()).getParentDirectory().getFileName();
                            const auto logical = folder.substring(3).getIntValue();
                            if (folder.startsWith("mic") && logical >= 1 && logical <= 8) mic = logical - 1;
                        }
                    }
                    auto lane = std::find_if(report.project.tracks.begin(), report.project.tracks.end(), [&](const auto& t) { return t.kind == kind && t.microphoneIndex == mic; });
                    if (lane == report.project.tracks.end()) { Track t; t.kind = kind; t.microphoneIndex = mic; t.name = kind == TrackKind::mic ? "Mic " + juce::String(mic + 1) : kind == TrackKind::cam2 ? "Cam 2" : "Cam 1"; report.project.tracks.push_back(t); lane = report.project.tracks.end() - 1; }
                    placements.emplace_back(a.assetId, lane->trackId); targets.push_back(lane->trackId);
                }
                if (take.mode == TakeMode::normal)
                {
                    auto carved = ClipEdits::carveOut(report.project, targets, {take.placementSample, length});
                    check(carved.status); report.project = std::move(carved.project);
                }
                for (const auto& placement : placements)
                {
                    auto lane = std::find_if(report.project.tracks.begin(), report.project.tracks.end(), [&](const auto& t) { return t.trackId == placement.second; });
                    Clip clip; clip.assetId = placement.first; clip.trackId = lane->trackId; clip.timelineStartSample = take.placementSample; clip.lengthSamples = length;
                    if (assets.size() > 1) { clip.linkGroupId = link.linkGroupId; link.clipIds.push_back(clip.clipId); }
                    lane->clips.edit().push_back(clip); ++report.addedClips;
                }
                if (link.clipIds.size() > 1) report.project.linkGroups.push_back(link);
            }
            require(report.project.editRevision < INT64_MAX, "Recovery revision overflow"); ++report.project.editRevision; ++report.changedTakes; committedRecoveryTakes.insert(takeId); report.messages.add(message);
            auto summary = object(); set(summary, "takeId", takeId); set(summary, "number", take.number); set(summary, "logicalSamples", integer(length)); set(summary, "cameras", cameraReports); set(summary, "assets", assetReports);
            set(summary, "captureStopKnown", state.stopped); set(summary, "completeManifestReused", restoreComplete); set(summary, "message", message); report.takes.add(summary);
        }
        for (const auto& file : child(root, "media").findChildFiles(juce::File::findFiles, true))
        {
            const auto name = rel(root, file); child(root, name);
            if (!referenced.count(name)) report.orphans.add(name);
        }
        check(report.project.validate());
        if (report.changedTakes || report.project.editRevision != selectedRevision || report.registryCommits
            || report.ignoredEditTail || native.legacy)
        {
            const auto dir = attempt(); const auto checkpoint = dir.getChildFile("project.recorder");
            require(maximumGeneration < INT64_MAX, "Recovery checkpoint generation overflow");
            auto cursor = report.checkpointInfo; cursor.generation = maximumGeneration + 1; cursor.checkpointRevision = report.project.editRevision;
            cursor.sourceFile = checkpoint;
            cursor.journalPath = rel(root, dir) + "/journal"; cursor.journalSegment = 1; cursor.journalSequence = 0;
            const auto json = RecorderSerializer::toJson(report.project, cursor);
            writeNew(checkpoint, json.toRawUTF8(), json.getNumBytesAsUTF8(), options.faults);
            RecorderProject verified; check(RecorderSerializer::fromJson(checkpoint.loadFileAsString(), verified));
            hit(options.hook, "recovery-before-commit");
            auto commit = object(); set(commit, "checkpoint", rel(root, checkpoint)); set(commit, "sha256", sha256(checkpoint)); set(commit, "files", outputFiles);
            set(commit, "projectId", report.project.projectId); set(commit, "lastSavedEditRevision", integer(report.lastSavedEditRevision));
            juce::Array<juce::var> ids; for (const auto& t : committedRecoveryTakes) ids.add(t); set(commit, "recoveredTakeIds", ids);
            set(commit, "messages", juce::var(report.messages)); set(commit, "takes", report.takes);
            Log log(options.faults); log.open(dir.getChildFile("commit.log"), true); log.append(Kind::recovery, commit); log.close();
            hit(options.hook, "recovery-after-commit"); report.checkpoint = checkpoint; report.checkpointInfo = cursor;
            writeJson(dir.getChildFile("report.json"), report.toJson(), options.faults);
        }
        if (document)
        {
            auto info = report.checkpointInfo; info.checkpointRevision = report.project.editRevision; info.usedBackup = report.usedBackup;
            info.recoveryMessage = report.messages.joinIntoString("\n") + juce::String::fromUTF8("\n마지막 편집 revision: ") + juce::String(juce::int64(report.lastSavedEditRevision));
            check(document->adopt(report.project, primary, info));
        }
        return juce::Result::ok();
    }
    catch (const std::exception& e)
    {
        report.warnings.add(juce::String::fromUTF8(e.what()));
        return juce::Result::fail(juce::String::fromUTF8(e.what()));
    }
}
}
