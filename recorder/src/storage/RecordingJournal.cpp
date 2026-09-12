#include "RecordingJournal.h"
#include "StorageEncoding.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace gocue::recorder
{
namespace
{
using namespace storageEncoding;
constexpr std::uint32_t magic = 0x314a4352; // RCJ1
constexpr std::uint64_t committed = 0x313054494d4d4f43; // COMMIT01
juce::var object() { return juce::var(new juce::DynamicObject()); }
void set(juce::var& value, const char* key, const juce::var& item) { value.getDynamicObject()->setProperty(key, item); }
juce::var integer(std::uint64_t n) { return juce::var(static_cast<juce::int64>(n)); }
juce::var take(const juce::Uuid& id) { auto v = object(); set(v, "takeId", id.toDashedString()); return v; }
bool uuid(const juce::var& v)
{
    return v.isString() && juce::Uuid(v.toString()).toDashedString() == v.toString()
        && !juce::Uuid(v.toString()).isNull();
}
bool number(const juce::var& v) { return v.isInt() || v.isInt64(); }
bool nonnegative(const juce::var& v) { return number(v) && static_cast<juce::int64>(v) >= 0; }
bool relative(const juce::var& v)
{
    if (!v.isString()) return false;
    const auto s = v.toString();
    if (s.isEmpty() || s.startsWithChar('/') || s.containsChar('\\') || s.containsChar(':')) return false;
    for (const auto& component : juce::StringArray::fromTokens(s, "/", ""))
        if (component.isEmpty() || component == "." || component == "..") return false;
    return true;
}
bool validPcm(const juce::var& fmt)
{
    for (const auto* key : {"sampleRate", "channels", "bitsPerSample", "blockAlign", "dataOffset"})
        if (!nonnegative(fmt[key]) || static_cast<juce::int64>(fmt[key]) == 0) return false;
    const auto channels = static_cast<juce::int64>(fmt["channels"]);
    return (channels == 1 || channels == 2) && static_cast<juce::int64>(fmt["bitsPerSample"]) == 24
        && static_cast<juce::int64>(fmt["blockAlign"]) == channels * 3
        && static_cast<juce::int64>(fmt["dataOffset"]) == 44 && static_cast<juce::int64>(fmt["sampleRate"]) <= 768000
        && fmt["nativeFormat"].isString();
}
bool validPayload(JournalKind kind, const juce::var& p)
{
    if (kind >= JournalKind::EditTransaction && kind <= JournalKind::MediaRegistry)
    {
        const auto project = p["projectId"].toString();
        if (!p.isObject() || juce::Uuid(project).isNull()
            || (project != juce::Uuid(project).toString() && project != juce::Uuid(project).toDashedString())) return false;
        if (kind == JournalKind::EditCheckpoint)
            return nonnegative(p["revision"]) && nonnegative(p["generation"]) && p["checksum"].isString();
        return nonnegative(p["baseRevision"]) && nonnegative(p["revision"])
            && p["entities"].isArray() && p["validationHash"].isString() && p["resultHash"].isString();
    }
    if (!p.isObject() || !uuid(p["takeId"])) return false;
    if (kind == JournalKind::TakeFinalized) return true;
    if (kind == JournalKind::TakeStopped) return number(p["Nstop"]) && uuid(p["placementEditId"]);
    const auto* files = p["files"].getArray();
    if (!files || files->size() > 128) return false;
    std::unordered_set<std::string> paths;
    for (const auto& f : *files)
    {
        if (!f.isObject() || !relative(f["path"]) || !paths.insert(f["path"].toString().toStdString()).second) return false;
        if (kind == JournalKind::TakeStarted)
        {
            if (!uuid(f["assetId"]) || !relative(f["plannedPath"])) return false;
            if (f.hasProperty("pcm") && (!validPcm(f["pcm"]) || f["pcm"]["sampleRate"] != p["pcm"]["sampleRate"])) return false;
        }
        else if (kind == JournalKind::Checkpoint)
        {
            for (const auto* key : {"validBytes", "validSamples", "firstSample", "dataOffset", "blockAlign", "ptsTimeBaseNum", "ptsTimeBaseDen"})
                if (!nonnegative(f[key])) return false;
            if (!number(f["pts"]) || static_cast<juce::int64>(f["blockAlign"]) == 0
                || static_cast<juce::int64>(f["ptsTimeBaseNum"]) == 0 || static_cast<juce::int64>(f["ptsTimeBaseDen"]) == 0
                || static_cast<juce::int64>(f["validBytes"]) < static_cast<juce::int64>(f["dataOffset"])) return false;
        }
        else return false;
    }
    if (kind == JournalKind::Checkpoint) return true;
    if (kind != JournalKind::TakeStarted || !number(p["N0"]) || !number(p["O0"]) || !number(p["Pstart"])
        || !p["usesOutputOrigin"].isBool()) return false;
    if (p.hasProperty("placementMode") && p["placementMode"].toString() != "normal" && p["placementMode"].toString() != "dub") return false;
    const auto fmt = p["pcm"];
    if (!validPcm(fmt)) return false;
    const auto* devices = p["devices"].getArray();
    if (!devices || devices->size() > 8) return false;
    for (const auto& d : *devices)
    {
        if (!d.isObject() || !d["deviceId"].isString() || !d["name"].isString()
            || !nonnegative(d["mic"]) || static_cast<int>(d["mic"]) < 1 || static_cast<int>(d["mic"]) > 8
            || !nonnegative(d["activeIndex"]) || !nonnegative(d["physicalIndex"])) return false;
        if (d.hasProperty("rightPhysicalIndex") || d.hasProperty("rightActiveIndex"))
            if (!nonnegative(d["rightPhysicalIndex"]) || !nonnegative(d["rightActiveIndex"])
                || static_cast<juce::int64>(d["rightPhysicalIndex"]) != static_cast<juce::int64>(d["physicalIndex"]) + 1
                || static_cast<juce::int64>(d["rightPhysicalIndex"]) > 255
                || static_cast<juce::int64>(d["rightActiveIndex"]) != static_cast<juce::int64>(d["activeIndex"]) + 1) return false;
    }
    return true;
}
}
RecordingJournal::~RecordingJournal() { if (writerLock != INVALID_HANDLE_VALUE) CloseHandle(writerLock); }
juce::Result RecordingJournal::fail(juce::Result r) { if (error.wasOk() && r.failed()) error = r; return error; }
juce::File RecordingJournal::logFile(const juce::File& dir, unsigned n)
{
    return dir.getChildFile("takes-" + juce::String(n).paddedLeft('0', 6) + ".log");
}
juce::Result RecordingJournal::open(const juce::File& dir, std::uint64_t rotationBytes)
{ return openDomain(dir, rotationBytes, false, 1, 0); }
juce::File RecordingJournal::editLogFile(const juce::File& dir, unsigned n)
{ return dir.getChildFile("edits-" + juce::String(n).paddedLeft('0', 6) + ".log"); }
juce::Result RecordingJournal::openEdits(const juce::File& dir, unsigned first, std::uint64_t preceding, std::uint64_t rotation)
{ return openDomain(dir, rotation, true, first, preceding); }
juce::Result RecordingJournal::openDomain(const juce::File& dir, std::uint64_t rotationBytes, bool editDomain, unsigned first, std::uint64_t preceding)
{
    if (writerLock != INVALID_HANDLE_VALUE) return fail(juce::Result::fail("Journal already open"));
    error = juce::Result::ok(); sequence = 0; segment = 0; directory = dir; edits = editDomain;
    if (first == 0) return fail(juce::Result::fail("Invalid first journal segment"));
    if (rotationBytes < headerBytes + commitBytes) return fail(juce::Result::fail("Journal rotation limit too small"));
    rotateAt = rotationBytes;
    if (fail(directory.createDirectory()).failed()) return error;
    const auto lockFile = directory.getChildFile(edits ? "edits.writer.lock" : "takes.writer.lock");
    writerLock = CreateFileW(lockFile.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writerLock == INVALID_HANDLE_VALUE)
        return fail(juce::Result::fail("Journal writer lock failed (Win32 " + juce::String(static_cast<int>(GetLastError())) + ")"));
    JournalReplay read;
    if (fail(replayDomain(directory, read, edits, first, preceding)).failed()) { close(); return error; }
    if (read.ignoredTail)
    {
        fail(juce::Result::fail("Journal needs recovery in a new copy; original tail preserved: " + read.tailReason));
        close(); return error;
    }
    sequence = read.lastSequence; segment = std::max(first, read.fileCount);
    if (fail(stream.open(edits ? editLogFile(directory, segment) : logFile(directory, segment))).failed()) close();
    return error;
}
juce::Result RecordingJournal::appendEditRecord(JournalKind kind, const juce::var& payload, const juce::Uuid& txn,
                                               const std::function<void(const char*)>& hook)
{
    if (!edits || kind < JournalKind::EditTransaction || kind > JournalKind::MediaRegistry)
        return fail(juce::Result::fail("Wrong journal domain/kind"));
    return appendRecord(kind, txn, payload, hook);
}
juce::Result RecordingJournal::rotate()
{
    if (error.failed()) return error;
    if (!stream.isOpen() || segment == std::numeric_limits<unsigned>::max()) return fail(juce::Result::fail("Cannot rotate journal"));
    if (fail(stream.close()).failed()) return error;
    return fail(stream.open(edits ? editLogFile(directory, ++segment) : logFile(directory, ++segment), DurableFile::OpenMode::createNew));
}
juce::Result RecordingJournal::appendRecord(JournalKind kind, const juce::Uuid& txn, const juce::var& payload,
                                           const std::function<void(const char*)>& hook)
{
    if (error.failed()) return error;
    if (writerLock == INVALID_HANDLE_VALUE) return fail(juce::Result::fail("Journal is not open"));
    if (txn.isNull() || !validPayload(kind, payload)) return fail(juce::Result::fail("Invalid journal payload/transaction"));
    const auto json = juce::JSON::toString(payload, true);
    const auto bytes = json.getNumBytesAsUTF8();
    const auto total = headerBytes + bytes + commitBytes;
    if (total > maxRecordBytes || sequence == std::numeric_limits<std::uint64_t>::max())
        return fail(juce::Result::fail("Journal record/sequence exceeds schema limits"));
    std::vector<std::uint8_t> record(total, 0);
    auto* h = record.data();
    put(h, magic); put(h + 4, static_cast<std::uint32_t>(total));
    put(h + 8, schemaVersion); put(h + 10, static_cast<std::uint16_t>(kind)); put(h + 12, sequence + 1);
    std::memcpy(h + 20, txn.getRawData(), 16);
    put(h + 36, static_cast<std::uint32_t>(bytes)); put(h + 40, crc32(json.toRawUTF8(), bytes));
    put(h + 44, crc32(h, 44));
    std::memcpy(h + headerBytes, json.toRawUTF8(), bytes); put(h + total - commitBytes, committed);
    if (stream.writtenBytes() && total > rotateAt - std::min(rotateAt, stream.writtenBytes()))
    {
        if (rotate().failed()) return error;
    }
    // Commit marker is a separate final append. A torn body/marker fails replay.
    if (fail(stream.write(h, total - commitBytes)).failed()) return error;
    if (hook) hook("journal-before-commit");
    if (fail(stream.write(h + total - commitBytes, commitBytes)).failed()
        || fail(stream.flushApplicationBuffers()).failed() || fail(stream.flushData()).failed()) return error;
    ++sequence;
    if (hook) hook("journal-after-commit");
    return error;
}
juce::Result RecordingJournal::append(const JournalTakeStarted& s, const juce::Uuid& txn)
{
    auto p = take(s.takeId), fmt = object();
    set(p, "N0", juce::var(static_cast<juce::int64>(s.n0))); set(p, "O0", juce::var(static_cast<juce::int64>(s.o0)));
    set(p, "Pstart", juce::var(static_cast<juce::int64>(s.pstart))); set(p, "usesOutputOrigin", s.usesOutputOrigin);
    set(p, "placementMode", s.placementMode);
    set(fmt, "sampleRate", integer(s.pcm.sampleRate)); set(fmt, "channels", s.pcm.channels);
    set(fmt, "bitsPerSample", s.pcm.bitsPerSample); set(fmt, "blockAlign", s.pcm.blockAlign);
    set(fmt, "dataOffset", integer(s.pcm.dataOffset)); set(fmt, "nativeFormat", s.pcm.nativeFormat); set(p, "pcm", fmt);
    juce::Array<juce::var> files, devices;
    for (const auto& f : s.files)
    {
        auto v = object(); set(v, "assetId", f.assetId); set(v, "path", f.path); set(v, "plannedPath", f.plannedPath);
        if (f.channels)
        {
            auto pcm = fmt.clone(); set(pcm, "channels", int(f.channels)); set(pcm, "blockAlign", int(f.channels * 3)); set(v, "pcm", pcm);
        }
        files.add(v);
    }
    for (const auto& d : s.devices)
    {
        auto v = object(); set(v, "deviceId", d.deviceId); set(v, "name", d.name); set(v, "mic", d.mic);
        set(v, "activeIndex", d.activeIndex); set(v, "physicalIndex", d.physicalIndex);
        if (d.rightPhysicalIndex >= 0) { set(v, "rightPhysicalIndex", d.rightPhysicalIndex); set(v, "rightActiveIndex", d.rightActiveIndex); }
        devices.add(v);
    }
    set(p, "files", files); set(p, "devices", devices);
    return appendRecord(JournalKind::TakeStarted, txn, p);
}
juce::Result RecordingJournal::append(const JournalCheckpoint& c, const juce::Uuid& txn)
{
    auto p = take(c.takeId); juce::Array<juce::var> files;
    for (const auto& f : c.files)
    {
        auto v = object(); set(v, "path", f.path); set(v, "validBytes", integer(f.validBytes)); set(v, "validSamples", integer(f.validSamples));
        set(v, "firstSample", integer(f.firstSample)); set(v, "pts", juce::var(static_cast<juce::int64>(f.pts)));
        set(v, "ptsTimeBaseNum", integer(f.ptsTimeBaseNum)); set(v, "ptsTimeBaseDen", integer(f.ptsTimeBaseDen));
        set(v, "dataOffset", integer(f.dataOffset)); set(v, "blockAlign", integer(f.blockAlign)); files.add(v);
    }
    set(p, "files", files); return appendRecord(JournalKind::Checkpoint, txn, p);
}
juce::Result RecordingJournal::append(const JournalTakeStopped& s, const juce::Uuid& txn)
{
    auto p = take(s.takeId); set(p, "Nstop", juce::var(static_cast<juce::int64>(s.nstop)));
    set(p, "placementEditId", s.placementEditId.toDashedString()); return appendRecord(JournalKind::TakeStopped, txn, p);
}
juce::Result RecordingJournal::append(const JournalTakeFinalized& f, const juce::Uuid& txn, const std::function<void(const char*)>& hook)
{
    return appendRecord(JournalKind::TakeFinalized, txn, take(f.takeId), hook);
}
juce::Result RecordingJournal::close()
{
    fail(stream.close());
    if (writerLock != INVALID_HANDLE_VALUE)
    {
        if (!CloseHandle(writerLock)) fail(juce::Result::fail("Close journal writer lock failed"));
        writerLock = INVALID_HANDLE_VALUE;
    }
    return error;
}
juce::Result RecordingJournal::replay(const juce::File& directory, JournalReplay& out)
{ return replayDomain(directory, out, false, 1, 0); }
juce::Result RecordingJournal::replayEdits(const juce::File& dir, JournalReplay& out, unsigned first, std::uint64_t preceding)
{ return replayDomain(dir, out, true, first, preceding); }
juce::Result RecordingJournal::replayDomain(const juce::File& directory, JournalReplay& out, bool edits, unsigned first, std::uint64_t preceding)
{
    out = {}; out.lastSequence = preceding; out.fileCount = first - 1;
    if (!directory.exists()) return juce::Result::ok();
    std::vector<std::pair<unsigned, juce::File>> segments;
    for (const auto& f : directory.findChildFiles(juce::File::findFiles, false, edits ? "edits-*.log" : "takes-*.log"))
    {
        const auto name = f.getFileNameWithoutExtension().substring(6);
        const auto n = name.getLargeIntValue();
        if (!name.containsOnly("0123456789") || name.length() < 6 || n < 1
            || n > std::numeric_limits<unsigned>::max()
            || (edits ? editLogFile(directory, static_cast<unsigned>(n)) : logFile(directory, static_cast<unsigned>(n))) != f)
            return juce::Result::fail("Invalid journal segment name: " + f.getFileName());
        if (n < first) continue;
        segments.emplace_back(static_cast<unsigned>(n), f);
    }
    std::sort(segments.begin(), segments.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::unordered_set<std::string> seen;
    for (const auto& entry : segments)
    {
        const auto stop = [&](const char* reason)
        {
            out.ignoredTail = true; out.tailReason = reason; out.tailFile = entry.second;
            return juce::Result::ok();
        };
        if (entry.first != out.fileCount + 1) return stop("Journal segment gap");
        ++out.fileCount; out.validBytesInLastFile = 0;
        juce::FileInputStream input(entry.second);
        if (input.failedToOpen()) return input.getStatus();
        const auto length = input.getTotalLength();
        while (input.getPosition() < length)
        {
            std::array<std::uint8_t, headerBytes> h{};
            if (length - input.getPosition() < static_cast<std::int64_t>(h.size())) return stop("Truncated record header");
            if (input.read(h.data(), static_cast<int>(h.size())) != static_cast<int>(h.size()))
                return input.getStatus().failed() ? input.getStatus() : juce::Result::fail("Journal header read was short");
            const auto total = get<std::uint32_t>(h.data() + 4), payloadSize = get<std::uint32_t>(h.data() + 36);
            const auto nextSequence = get<std::uint64_t>(h.data() + 12);
            const auto kind = static_cast<JournalKind>(get<std::uint16_t>(h.data() + 10));
            if (edits != (kind >= JournalKind::EditTransaction && kind <= JournalKind::MediaRegistry)) return stop("Wrong journal domain/kind");
            if (get<std::uint32_t>(h.data()) != magic || get<std::uint32_t>(h.data() + 44) != crc32(h.data(), 44))
                return stop("Record header magic/CRC mismatch");
            if (get<std::uint16_t>(h.data() + 8) != schemaVersion) return stop("Unsupported journal schema");
            if (total < headerBytes + commitBytes || total > maxRecordBytes || payloadSize != total - headerBytes - commitBytes)
                return stop("Invalid record length");
            if (out.lastSequence == std::numeric_limits<std::uint64_t>::max() || nextSequence != out.lastSequence + 1)
                return stop("Non-contiguous journal sequence");
            const auto remaining = static_cast<int>(total - headerBytes);
            if (length - input.getPosition() < remaining) return stop("Truncated record payload/commit");
            std::vector<std::uint8_t> body(static_cast<std::size_t>(remaining));
            if (input.read(body.data(), remaining) != remaining)
                return input.getStatus().failed() ? input.getStatus() : juce::Result::fail("Journal payload read was short");
            if (get<std::uint64_t>(body.data() + payloadSize) != committed) return stop("Missing commit marker");
            if (get<std::uint32_t>(h.data() + 40) != crc32(body.data(), payloadSize)) return stop("Payload CRC mismatch");
            juce::var payload;
            if (juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(body.data()), static_cast<int>(payloadSize)), payload).failed()
                || !validPayload(kind, payload)) return stop("Invalid record payload schema");
            juce::Uuid txn(h.data() + 20);
            if (txn.isNull()) return stop("Null transaction UUID");
            out.lastSequence = nextSequence; out.validBytesInLastFile = static_cast<std::uint64_t>(input.getPosition());
            if (seen.insert(txn.toString().toStdString()).second) out.records.push_back({kind, nextSequence, txn, payload});
            else ++out.duplicateTransactions;
        }
        if (input.getStatus().failed()) return input.getStatus();
    }
    return juce::Result::ok();
}
}
