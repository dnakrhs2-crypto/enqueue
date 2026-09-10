#include "RecorderSerializer.h"
#include "../app/ProductIdentity.h"
#include "model/SafeFileWrite.h"
#include "storage/DurableFile.h"
#include <charconv>
#include <limits>
#include <stdexcept>
#include <mutex>

namespace gocue::recorder
{
namespace
{
using V = juce::var;
V object() { return new juce::DynamicObject(); }
void put(V& o, const char* key, V value) { o.getDynamicObject()->setProperty(key, value); }
V integer(Sample n) { return V(static_cast<juce::int64>(n)); }
template<class T, class F> V array(const std::vector<T>& input, F convert)
{ juce::Array<V> out; for (const auto& value : input) out.add(convert(value)); return out; }
V strings(const std::vector<Id>& values) { return array(values, [](const auto& value) { return V(value); }); }
V rate(FrameRate r) { auto o = object(); put(o, "numerator", integer(r.numerator)); put(o, "denominator", integer(r.denominator)); return o; }
V range(SampleRange r) { auto o = object(); put(o, "start", integer(r.start)); put(o, "length", integer(r.length)); return o; }
constexpr const char* assetNames[] { "camera", "mic", "import" };
constexpr const char* trackNames[] { "cam1", "cam2", "mic", "importAudio" };
constexpr const char* modeNames[] { "normal", "dub" };
constexpr const char* stateNames[] { "recording", "stopped", "finalising", "complete", "partial" };
template<class E, size_t N> V enumName(E value, const char* const (&names)[N])
{ const auto index = static_cast<size_t>(value); return index < N ? V(names[index]) : V("invalid"); }
V format(const OriginalFormat& f)
{
    auto o = object(); put(o, "codec", f.codec); put(o, "sampleRate", integer(f.sampleRate));
    put(o, "channels", f.channels); put(o, "bitsPerSample", f.bitsPerSample);
    put(o, "width", f.width); put(o, "height", f.height); put(o, "fps", rate(f.fps)); return o;
}
V asset(const MediaAsset& a)
{
    auto o = object(); put(o, "assetId", a.assetId); put(o, "kind", enumName(a.kind, assetNames));
    put(o, "relativePath", a.relativePath);
    put(o, "chunks", array(a.chunks, [](const auto& c) { auto v = object(); put(v, "relativePath", c.relativePath); put(v, "sourceRange", range(c.sourceRange)); return v; }));
    put(o, "originalFormat", format(a.originalFormat)); put(o, "contentIdentity", a.contentIdentity);
    put(o, "logicalLength", integer(a.logicalLength)); put(o, "availableRanges", array(a.availableRanges, range)); put(o, "gaps", array(a.gaps, range));
    put(o, "sourceUnitsNumerator", integer(static_cast<Sample>(a.sourceUnitsNumerator)));
    put(o, "sourceUnitsDenominator", integer(static_cast<Sample>(a.sourceUnitsDenominator)));
    put(o, "mediaGeneration", integer(a.mediaGeneration)); return o;
}
V capture(const CaptureSnapshot& c)
{
    auto o = object();
    juce::Array<V> cameras;
    for (size_t i = 0; i < 2; ++i)
    { auto v = object(); put(v, "deviceId", c.cameraDeviceIds[i]); put(v, "mode", c.cameraModes[i]); put(v, "offsetSamples", integer(c.cameraOffsetSamples[i])); cameras.add(v); }
    put(o, "cameras", cameras); put(o, "asioDeviceId", c.asioDeviceId);
    put(o, "physicalInputs", array(c.physicalInputs, [](int i) { return V(i); }));
    if (!c.physicalInputsRight.empty()) put(o, "physicalInputsRight", array(c.physicalInputsRight, [](int i) { return V(i); }));
    put(o, "inputOffsetSamples", integer(c.inputOffsetSamples)); put(o, "outputOffsetSamples", integer(c.outputOffsetSamples));
    put(o, "calibrationDate", c.calibrationDate); put(o, "calibrationIdentity", c.calibrationIdentity); return o;
}
V take(const Take& t)
{
    auto o = object(); put(o, "takeId", t.takeId); put(o, "number", t.number); put(o, "createdAt", t.createdAt); put(o, "name", t.name);
    put(o, "mode", enumName(t.mode, modeNames)); put(o, "N0", integer(t.N0)); put(o, "O0", integer(t.O0));
    put(o, "placementSample", integer(t.placementSample)); put(o, "logicalLength", integer(t.logicalLength));
    put(o, "cam1AssetId", t.cam1AssetId); put(o, "cam2AssetId", t.cam2AssetId); put(o, "microphoneAssetIds", strings(t.microphoneAssetIds));
    put(o, "capture", capture(t.capture)); put(o, "state", enumName(t.state, stateNames)); return o;
}
V clip(const Clip& c)
{
    auto o = object(); put(o, "clipId", c.clipId); put(o, "trackId", c.trackId); put(o, "assetId", c.assetId);
    put(o, "sourceIn", integer(c.sourceIn)); put(o, "lengthSamples", integer(c.lengthSamples)); put(o, "timelineStartSample", integer(c.timelineStartSample));
    put(o, "linkGroupId", c.linkGroupId); put(o, "takeStackId", c.takeStackId); put(o, "versionId", c.versionId); return o;
}
V track(const Track& t)
{
    auto o = object(); put(o, "trackId", t.trackId); put(o, "kind", enumName(t.kind, trackNames)); put(o, "name", t.name);
    put(o, "mute", t.mute); put(o, "solo", t.solo); put(o, "microphoneIndex", t.microphoneIndex); put(o, "clips", array(t.clips.items(), clip)); return o;
}
V marker(const Marker& m)
{ auto o = object(); put(o, "markerId", m.markerId); put(o, "sample", integer(m.sample)); put(o, "name", m.name); put(o, "colour", m.colour); return o; }
V link(const LinkGroup& g)
{ auto o = object(); put(o, "linkGroupId", g.linkGroupId); put(o, "clipIds", strings(g.clipIds)); return o; }
V stack(const TakeStack& s)
{
    auto o = object(); put(o, "stackId", s.stackId); put(o, "anchorSample", integer(s.anchorSample)); put(o, "spanSamples", integer(s.spanSamples));
    put(o, "versions", array(s.versions, [](const TakeVersion& v) { auto x = object(); put(x, "versionId", v.versionId); put(x, "clipIds", strings(v.clipIds)); return x; }));
    put(o, "activeVersionId", s.activeVersionId); return o;
}
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
void checkIntegerTokens(const juce::String& json)
{
    // JUCE 8's integer parser multiplies in int64 without an overflow check. Validate
    // raw tokens before it can wrap; project schema 1 contains no floating-point numbers.
    const auto utf8 = json.toStdString(); bool quoted = false, escaped = false;
    for (size_t i = 0; i < utf8.size(); ++i)
    {
        const auto c = utf8[i];
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') { quoted = true; continue; }
        if (c != '-' && (c < '0' || c > '9')) continue;
        const auto start = i; if (c == '-') ++i;
        const auto digits = i;
        while (i < utf8.size() && utf8[i] >= '0' && utf8[i] <= '9') ++i;
        require(i > digits && (i == digits + 1 || utf8[digits] != '0'), "잘못된 JSON 정수입니다.");
        require(i == utf8.size() || (utf8[i] != '.' && utf8[i] != 'e' && utf8[i] != 'E'), "프로젝트 시간과 번호는 정수여야 합니다.");
        Sample parsed = 0;
        const auto result = std::from_chars(utf8.data() + start, utf8.data() + i, parsed);
        require(result.ec == std::errc{} && result.ptr == utf8.data() + i, "JSON 정수가 64비트 범위를 벗어납니다.");
        --i;
    }
}
const V& field(const V& o, const char* name)
{
    auto* d = o.getDynamicObject(); require(d != nullptr && d->hasProperty(name), "필수 프로젝트 필드가 없습니다.");
    return d->getProperty(name);
}
juce::String textValue(const V& v) { require(v.isString(), "문자열 필드 형식이 잘못되었습니다."); return v.toString(); }
juce::String stringField(const V& o, const char* key) { return textValue(field(o, key)); }
Sample number(const V& v) { require(v.isInt() || v.isInt64(), "샘플과 번호는 정수여야 합니다."); return static_cast<juce::int64>(v); }
Sample num(const V& o, const char* key) { return number(field(o, key)); }
int intField(const V& o, const char* key)
{ const auto n = num(o, key); require(n >= (std::numeric_limits<int>::min)() && n <= (std::numeric_limits<int>::max)(), "정수 필드 범위를 초과했습니다."); return static_cast<int>(n); }
std::uint32_t u32(const V& o, const char* key)
{ const auto n = num(o, key); require(n >= 0 && n <= (std::numeric_limits<std::uint32_t>::max)(), "양수 필드 범위가 잘못되었습니다."); return static_cast<std::uint32_t>(n); }
bool boolean(const V& o, const char* key) { const auto& v = field(o, key); require(v.isBool(), "참/거짓 필드가 잘못되었습니다."); return static_cast<bool>(v); }
const juce::Array<V>& list(const V& v) { require(v.isArray(), "목록 필드가 잘못되었습니다."); return *v.getArray(); }
template<class T, class F> std::vector<T> readArray(const V& v, F convert)
{ std::vector<T> out; for (const auto& x : list(v)) out.push_back(convert(x)); return out; }
std::vector<Id> readStrings(const V& v) { return readArray<Id>(v, textValue); }
template<class E, size_t N> E readEnum(const V& v, const char* const (&names)[N])
{ const auto s = textValue(v); for (size_t i = 0; i < N; ++i) if (s == names[i]) return static_cast<E>(i); throw std::invalid_argument("지원하지 않는 종류입니다."); }
FrameRate readRate(const V& v) { return {u32(v, "numerator"), u32(v, "denominator")}; }
SampleRange readRange(const V& v) { return {num(v, "start"), num(v, "length")}; }
OriginalFormat readFormat(const V& v)
{
    OriginalFormat f; f.codec = stringField(v, "codec"); f.sampleRate = u32(v, "sampleRate"); f.channels = intField(v, "channels");
    f.bitsPerSample = intField(v, "bitsPerSample"); f.width = intField(v, "width"); f.height = intField(v, "height"); f.fps = readRate(field(v, "fps")); return f;
}
MediaAsset readAsset(const V& v)
{
    MediaAsset a; a.assetId = stringField(v, "assetId"); a.kind = readEnum<AssetKind>(field(v, "kind"), assetNames); a.relativePath = stringField(v, "relativePath");
    a.chunks = readArray<MediaChunk>(field(v, "chunks"), [](const V& c) { return MediaChunk{stringField(c, "relativePath"), readRange(field(c, "sourceRange"))}; });
    a.originalFormat = readFormat(field(v, "originalFormat")); a.contentIdentity = stringField(v, "contentIdentity"); a.logicalLength = num(v, "logicalLength");
    a.availableRanges = readArray<SampleRange>(field(v, "availableRanges"), readRange); a.gaps = readArray<SampleRange>(field(v, "gaps"), readRange);
    const auto n = num(v, "sourceUnitsNumerator"), d = num(v, "sourceUnitsDenominator"); require(n > 0 && d > 0, "원본 매핑은 양수여야 합니다.");
    a.sourceUnitsNumerator = static_cast<std::uint64_t>(n); a.sourceUnitsDenominator = static_cast<std::uint64_t>(d); a.mediaGeneration = num(v, "mediaGeneration"); return a;
}
CaptureSnapshot readCapture(const V& v)
{
    CaptureSnapshot c; const auto& cameras = list(field(v, "cameras")); require(cameras.size() == 2, "카메라 설정 슬롯 수가 잘못되었습니다.");
    for (size_t i = 0; i < 2; ++i) { const auto& x = cameras[static_cast<int>(i)]; c.cameraDeviceIds[i] = stringField(x, "deviceId"); c.cameraModes[i] = stringField(x, "mode"); c.cameraOffsetSamples[i] = num(x, "offsetSamples"); }
    c.asioDeviceId = stringField(v, "asioDeviceId"); c.calibrationDate = stringField(v, "calibrationDate"); c.calibrationIdentity = stringField(v, "calibrationIdentity");
    c.inputOffsetSamples = num(v, "inputOffsetSamples"); c.outputOffsetSamples = num(v, "outputOffsetSamples");
    c.physicalInputs = readArray<int>(field(v, "physicalInputs"), [](const V& x) { const auto n = number(x); require(n >= 0 && n <= (std::numeric_limits<int>::max)(), "물리 입력 범위가 잘못되었습니다."); return static_cast<int>(n); });
    if (v.hasProperty("physicalInputsRight"))
        c.physicalInputsRight = readArray<int>(field(v, "physicalInputsRight"), [](const V& x) { const auto n = number(x); require(n >= -1 && n <= 255, "Invalid right physical input"); return static_cast<int>(n); });
    return c;
}
Take readTake(const V& v)
{
    Take t; t.takeId = stringField(v, "takeId"); t.number = intField(v, "number"); t.createdAt = stringField(v, "createdAt"); t.name = stringField(v, "name");
    t.mode = readEnum<TakeMode>(field(v, "mode"), modeNames); t.state = readEnum<TakeState>(field(v, "state"), stateNames);
    t.N0 = num(v, "N0"); t.O0 = num(v, "O0"); t.placementSample = num(v, "placementSample"); t.logicalLength = num(v, "logicalLength");
    t.cam1AssetId = stringField(v, "cam1AssetId"); t.cam2AssetId = stringField(v, "cam2AssetId"); t.microphoneAssetIds = readStrings(field(v, "microphoneAssetIds")); t.capture = readCapture(field(v, "capture")); return t;
}
Clip readClip(const V& v)
{
    Clip c; c.clipId = stringField(v, "clipId"); c.trackId = stringField(v, "trackId"); c.assetId = stringField(v, "assetId");
    c.sourceIn = num(v, "sourceIn"); c.lengthSamples = num(v, "lengthSamples"); c.timelineStartSample = num(v, "timelineStartSample");
    c.linkGroupId = stringField(v, "linkGroupId"); c.takeStackId = stringField(v, "takeStackId"); c.versionId = stringField(v, "versionId"); return c;
}
Track readTrack(const V& v)
{
    Track t; t.trackId = stringField(v, "trackId"); t.kind = readEnum<TrackKind>(field(v, "kind"), trackNames); t.name = stringField(v, "name");
    t.mute = boolean(v, "mute"); t.solo = boolean(v, "solo"); t.microphoneIndex = intField(v, "microphoneIndex"); t.clips.edit() = readArray<Clip>(field(v, "clips"), readClip); return t;
}
EditState readEdit(const V& v)
{
    EditState e; e.name = stringField(v, "name"); e.tracks = readArray<Track>(field(v, "tracks"), readTrack);
    e.markers = readArray<Marker>(field(v, "markers"), [](const V& x) { Marker m; m.markerId = stringField(x, "markerId"); m.sample = num(x, "sample"); m.name = stringField(x, "name"); m.colour = stringField(x, "colour"); return m; });
    e.linkGroups = readArray<LinkGroup>(field(v, "linkGroups"), [](const V& x) { return LinkGroup{stringField(x, "linkGroupId"), readStrings(field(x, "clipIds"))}; });
    e.takeStacks = readArray<TakeStack>(field(v, "takeStacks"), [](const V& x)
    {
        TakeStack s; s.stackId = stringField(x, "stackId"); s.anchorSample = num(x, "anchorSample"); s.spanSamples = num(x, "spanSamples"); s.activeVersionId = stringField(x, "activeVersionId");
        s.versions = readArray<TakeVersion>(field(x, "versions"), [](const V& a) { return TakeVersion{stringField(a, "versionId"), readStrings(field(a, "clipIds"))}; }); return s;
    }); return e;
}
bool foreignOrFuture(const juce::String& json)
{
    try { checkIntegerTokens(json); } catch (const std::exception&) { return true; } // never downgrade an unparseable version number
    const auto v = juce::JSON::parse(json);
    const auto formatId = v.getProperty("format", {});
    const auto schema = v.getProperty("schemaVersion", {});
    return (formatId.isString() && formatId.toString() != ProductIdentity::fileType())
        || ((schema.isInt() || schema.isInt64()) && static_cast<juce::int64>(schema) > RecorderProject::currentSchemaVersion);
}
bool sameFields(const V& a, const V& b)
{
    if (auto* x = a.getDynamicObject())
    {
        const auto* y = b.getDynamicObject();
        if (y == nullptr || x->getProperties().size() != y->getProperties().size()) return false;
        for (const auto& property : x->getProperties())
            if (!y->hasProperty(property.name) || !sameFields(property.value, y->getProperty(property.name))) return false;
        return true;
    }
    if (a.isArray())
    {
        if (!b.isArray() || a.size() != b.size()) return false;
        for (int i = 0; i < a.size(); ++i) if (!sameFields(a[i], b[i])) return false;
        return true;
    }
    return a == b;
}
}
juce::var RecorderSerializer::editStateToVar(const EditState& e)
{
    auto o = object(); put(o, "name", e.name); put(o, "tracks", array(e.tracks, track)); put(o, "markers", array(e.markers, marker));
    put(o, "linkGroups", array(e.linkGroups, link)); put(o, "takeStacks", array(e.takeStacks, stack)); return o;
}
juce::String RecorderSerializer::fingerprint(const juce::var& value)
{
    const auto encoded = juce::JSON::toString(value, true);
    const auto bytes = encoded.toUTF8();
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto* p = reinterpret_cast<const unsigned char*>(bytes.getAddress()); *p != 0; ++p) { hash ^= *p; hash *= 1099511628211ull; }
    return juce::String::toHexString(static_cast<juce::int64>(hash)).paddedLeft('0', 16);
}
juce::String RecorderSerializer::toJson(const RecorderProject& p)
{
    auto root = object(); put(root, "format", ProductIdentity::fileType()); put(root, "schemaVersion", p.schemaVersion);
    put(root, "projectId", p.projectId); put(root, "Fs", integer(p.Fs)); put(root, "fps", rate(p.fps));
    put(root, "editRevision", integer(p.editRevision)); put(root, "checkpointRevision", integer(p.editRevision));
    put(root, "edit", editStateToVar(p)); auto registry = object();
    put(registry, "assets", array(p.media->assets, asset)); put(registry, "takes", array(p.media->takes, take)); put(root, "media", registry);
    put(root, "checksum", fingerprint(root)); return juce::JSON::toString(root, false) + "\n";
}
juce::String RecorderSerializer::toJson(const RecorderProject& p, const CheckpointInfo& info)
{
    auto root = juce::JSON::parse(toJson(p)); root.getDynamicObject()->removeProperty("checksum");
    auto cursor = object(); put(cursor, "generation", integer(info.generation)); put(cursor, "journalPath", info.journalPath);
    put(cursor, "segment", integer(info.journalSegment)); put(cursor, "sequence", integer(static_cast<Sample>(info.journalSequence)));
    put(root, "checkpoint", cursor); put(root, "checksum", fingerprint(root)); return juce::JSON::toString(root, false) + "\n";
}
juce::Result RecorderSerializer::fromJson(const juce::String& json, RecorderProject& out, CheckpointInfo* info)
{
    try
    {
        checkIntegerTokens(json);
        V root; const auto parsed = juce::JSON::parse(json, root); if (parsed.failed()) return juce::Result::fail(juce::String::fromUTF8("프로젝트 JSON을 읽을 수 없습니다: ") + parsed.getErrorMessage());
        require(stringField(root, "format") == ProductIdentity::fileType(), "다른 앱의 프로젝트 형식입니다.");
        const auto version = intField(root, "schemaVersion"); require(version <= RecorderProject::currentSchemaVersion, "더 최신 버전에서 만든 프로젝트입니다.");
        require(version == RecorderProject::currentSchemaVersion, "지원하지 않는 프로젝트 버전입니다.");
        const auto hash = stringField(root, "checksum"); root.getDynamicObject()->removeProperty("checksum");
        require(hash == fingerprint(root), "프로젝트 검증 값이 일치하지 않습니다.");
        RecorderProject p; p.schemaVersion = version; p.projectId = stringField(root, "projectId"); p.Fs = u32(root, "Fs"); p.fps = readRate(field(root, "fps"));
        p.editRevision = num(root, "editRevision"); require(num(root, "checkpointRevision") == p.editRevision, "저장 지점과 편집 이력이 일치하지 않습니다.");
        static_cast<EditState&>(p) = readEdit(field(root, "edit")); auto registry = std::make_shared<MediaRegistry>();
        const auto& m = field(root, "media"); registry->assets = readArray<MediaAsset>(field(m, "assets"), readAsset); registry->takes = readArray<Take>(field(m, "takes"), readTake); p.media = registry;
        const auto valid = p.validate(); if (valid.failed()) return valid;
        CheckpointInfo cursor; cursor.checkpointRevision = p.editRevision;
        if (root.hasProperty("checkpoint"))
        {
            const auto c = field(root, "checkpoint");
            require(c.isObject() && c.getDynamicObject()->getProperties().size() == 4, "잘못된 checkpoint 세대입니다.");
            cursor.generation = num(c, "generation"); cursor.journalPath = stringField(c, "journalPath");
            cursor.journalSegment = u32(c, "segment"); const auto seq = num(c, "sequence");
            require(cursor.generation > 0 && cursor.journalSegment > 0 && seq >= 0 && isProjectRelativePath(cursor.journalPath), "잘못된 checkpoint 저널 위치입니다.");
            require(cursor.journalPath == "journal" || (cursor.journalPath.startsWith("recovery/") && cursor.journalPath.endsWith("/journal")), "지원하지 않는 checkpoint 저널 위치입니다.");
            cursor.journalSequence = static_cast<std::uint64_t>(seq);
            root.getDynamicObject()->removeProperty("checkpoint");
        }
        // Reject unrecognised/lossy fields in this schema instead of silently dropping them on save.
        auto canonical = juce::JSON::parse(toJson(p)); canonical.getDynamicObject()->removeProperty("checksum");
        require(sameFields(canonical, root), "프로젝트에 지원하지 않는 필드가 있습니다.");
        out = std::move(p); if (info != nullptr) *info = cursor; return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
juce::Result RecorderSerializer::readCheckpoint(const juce::File& file, RecorderProject& out, CheckpointInfo* info, bool allowBackup)
{
    const auto text = file.loadFileAsString();
    RecorderProject primary; CheckpointInfo primaryInfo;
    const auto result = fromJson(text, primary, &primaryInfo);
    primaryInfo.sourceFile = file;
    if (!allowBackup || foreignOrFuture(text)) { if (result.wasOk()) { out = primary; if (info) *info = primaryInfo; } return result; }
    const auto backup = file.getSiblingFile(file.getFileName() + ".bak");
    if (!backup.existsAsFile()) { if (result.wasOk()) { out = primary; if (info) *info = primaryInfo; } return result; }
    CheckpointInfo recovered;
    RecorderProject previous; const auto fallback = fromJson(backup.loadFileAsString(), previous, &recovered);
    recovered.sourceFile = backup;
    const bool backupNewer = fallback.wasOk() && (recovered.generation > primaryInfo.generation
        || (recovered.generation == primaryInfo.generation && previous.editRevision > primary.editRevision));
    if (result.wasOk() && (!backupNewer || previous.projectId != primary.projectId))
    { out = primary; if (info) *info = primaryInfo; return result; }
    if (fallback.failed()) return juce::Result::fail(result.getErrorMessage() + juce::String::fromUTF8(" 이전 저장본도 열 수 없습니다: ") + fallback.getErrorMessage());
    out = previous;
    recovered.usedBackup = true; recovered.recoveryMessage = juce::String::fromUTF8("이전 저장본을 열었습니다. 원본 파일은 보존됩니다. 마지막 저장 이력: ") + juce::String(static_cast<juce::int64>(out.editRevision));
    if (info != nullptr) *info = recovered;
    return juce::Result::ok();
}
juce::Result RecorderSerializer::writeCheckpoint(const juce::File& file, const RecorderProject& p)
{ CheckpointInfo written; return writeCheckpoint(file, p, written); }
juce::Result RecorderSerializer::writeCheckpoint(const juce::File& file, const RecorderProject& p, CheckpointInfo& written,
                                                 FileIoFaultAdapter* faults, const std::function<void(const char*)>& hook)
{
    // Also serializes the legacy TakeController's checkpoint call with the edit worker.
    static std::mutex checkpointMutex; const std::lock_guard<std::mutex> guard(checkpointMutex);
    try
    {
    const auto valid = p.validate(); if (valid.failed()) return valid;
    if (file == juce::File()) return juce::Result::fail(juce::String::fromUTF8("프로젝트 저장 위치가 없습니다."));
    if (file.getFullPathName().startsWith("\\\\")) return juce::Result::fail(juce::String::fromUTF8("프로젝트는 로컬 폴더에 저장하세요."));
    const auto verify = [](const juce::String& text) { RecorderProject checked; return fromJson(text, checked); };
    const auto flush = [&](const juce::File& target)
    {
        DurableFile durable(faults); auto r = durable.open(target); if (r.failed()) return r;
        r = durable.flushData(); const auto closed = durable.close(); return r.failed() ? r : closed;
    };
    auto nextInfo = written; nextInfo.checkpointRevision = p.editRevision;
    Sample generation = nextInfo.generation; juce::String unchanged;
    if (file.exists())
    {
        if (!file.existsAsFile()) return juce::Result::fail(juce::String::fromUTF8("프로젝트 저장 경로가 파일이 아닙니다."));
        const auto old = file.loadFileAsString(); RecorderProject previous; CheckpointInfo prior;
        const auto checked = fromJson(old, previous, &prior);
        if (checked.failed()) return juce::Result::fail(juce::String::fromUTF8("기존 저장본이 손상되어 덮어쓰지 않았습니다. ") + checked.getErrorMessage());
        if (previous.projectId != p.projectId || previous.editRevision > p.editRevision)
            return juce::Result::fail(juce::String::fromUTF8("다른 프로젝트 또는 더 최신 저장본을 덮어쓸 수 없습니다."));
        generation = (std::max)(generation, prior.generation);
        // Identical explicit saves preserve bytes, while a new journal cursor
        // always creates a generation even at the same user edit revision.
        if (written.generation == 0 && written.journalSequence == 0 && written.journalSegment == 1
            && written.journalPath == "journal")
        {
            nextInfo = prior;
            if (toJson(previous) == toJson(p)) unchanged = old;
        }
        const auto backup = file.getSiblingFile(file.getFileName() + ".bak");
        RecorderProject bakProject; CheckpointInfo bakInfo;
        if (backup.existsAsFile() && fromJson(backup.loadFileAsString(), bakProject, &bakInfo).wasOk())
        {
            if (bakProject.projectId != p.projectId || bakProject.editRevision > p.editRevision)
                return juce::Result::fail("Cannot replace a foreign/newer backup checkpoint");
            generation = (std::max)(generation, bakInfo.generation);
        }
        const auto savedBackup = gocue::SafeFileWrite::writeTextVerified(backup, old, verify);
        if (savedBackup.failed()) return savedBackup;
        const auto flushed = flush(backup); if (flushed.failed()) return flushed;
        if (hook) hook("checkpoint-backup-flushed");
    }
    for (const auto* path : {"journal", "media/takes", "media/imports", "cache", "recovery", "exports"})
    {
        const auto created = file.getParentDirectory().getChildFile(path).createDirectory();
        if (created.failed()) return created;
    }
    if (unchanged.isNotEmpty())
    { const auto r = flush(file); if (r.wasOk()) written = nextInfo; return r; }
    if (generation == (std::numeric_limits<Sample>::max)()) return juce::Result::fail("Checkpoint generation exhausted");
    nextInfo.generation = generation + 1; nextInfo.checkpointRevision = p.editRevision;
    const auto json = toJson(p, nextInfo);
    if (hook) hook("checkpoint-replace");
    const auto saved = gocue::SafeFileWrite::writeTextVerified(file, json, [&](const juce::String& text)
    { const auto r = verify(text); if (r.wasOk() && hook) hook("checkpoint-verified-before-replace"); return r; });
    if (saved.failed()) return saved;
    if (hook) hook("checkpoint-after-replace");
    const auto flushed = flush(file); if (flushed.failed()) return flushed;
    RecorderProject verified; CheckpointInfo checked; const auto read = fromJson(file.loadFileAsString(), verified, &checked);
    if (read.failed()) return read;
    if (toJson(verified) != toJson(p) || checked.generation != nextInfo.generation) return juce::Result::fail("Checkpoint read-back mismatch");
    written = checked; written.sourceFile = file; if (hook) hook("checkpoint-after-flush"); return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(e.what()); }
}
}
