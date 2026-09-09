#include "app/ProductIdentity.h"
#include "app/RecorderDocument.h"
#include "app/RecorderSettings.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace gocue::recorder;
namespace
{
void require(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
void ok(const juce::Result& result) { if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString()); }
template<class F> void rejects(F f) { bool threw = false; try { f(); } catch (const std::exception&) { threw = true; } require(threw, "Expected exception"); }
struct TempProject
{
    const juce::File parent = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-r08-tests");
    const juce::File root = parent.getChildFile(newId());
    TempProject() { ok(root.createDirectory()); }
    ~TempProject() { if (root.isAChildOf(parent) && isId(root.getFileName())) root.deleteRecursively(); }
    juce::File checkpoint() const { return root.getChildFile(ProductIdentity::projectFileName()); }
};
struct TakeFixture { Take take; std::vector<MediaAsset> assets; };
MediaAsset source(AssetKind kind, Sample length, std::uint32_t Fs = 48000)
{
    MediaAsset a; a.kind = kind; a.logicalLength = length; a.availableRanges = {{0, length}}; a.contentIdentity = "fixture:" + a.assetId;
    a.relativePath = (kind == AssetKind::importAudio ? "media/imports/" : "media/takes/") + a.assetId + (kind == AssetKind::camera ? "/cam.mp4" : "/source.wav");
    auto& f = a.originalFormat;
    if (kind == AssetKind::camera) { f.codec = "h264"; f.width = 1920; f.height = 1080; f.fps = {30, 1}; a.sourceUnitsNumerator = 30; a.sourceUnitsDenominator = Fs; }
    else { f.codec = "pcm_s24le"; f.sampleRate = Fs; f.channels = kind == AssetKind::mic ? 1 : 2; f.bitsPerSample = 24; }
    return a;
}
TakeFixture takeFixture(int number = 1, Sample length = 48000, bool cam2 = false, int microphones = 0)
{
    TakeFixture f; f.take.number = number; f.take.logicalLength = length; f.take.createdAt = "2026-09-09T06:00:00Z"; f.take.name = "fixture"; f.take.state = TakeState::complete;
    f.assets.push_back(source(AssetKind::camera, length)); f.take.cam1AssetId = f.assets.back().assetId;
    if (cam2) { f.assets.push_back(source(AssetKind::camera, length)); f.take.cam2AssetId = f.assets.back().assetId; }
    for (int i = 0; i < microphones; ++i) { f.assets.push_back(source(AssetKind::mic, length)); f.take.microphoneAssetIds.push_back(f.assets.back().assetId); f.take.capture.physicalInputs.push_back(i * 2); }
    return f;
}
RecorderProject placedProject(bool cam2 = false, int microphones = 0)
{ RecorderDocument d; auto f = takeFixture(1, 48000, cam2, microphones); ok(d.placeTake(f.take, f.assets)); return d.getProject(); }
Clip& firstClip(RecorderProject& p) { return p.tracks.front().clips.edit().front(); }
void assertInvalid(RecorderProject p)
{
    require(p.validate().failed(), "Invalid model accepted"); RecorderProject retained; const auto identity = retained.projectId;
    require(RecorderSerializer::fromJson(RecorderSerializer::toJson(p), retained).failed(), "Invalid JSON accepted");
    require(retained.projectId == identity, "Failed parse mutated its output");
}
void addImport(RecorderDocument& d, Sample start, Sample length)
{
    const auto a = source(AssetKind::importAudio, length); ok(d.registerMedia({a}));
    ok(d.performEdit("import", [&](EditState& e) { Track t; t.kind = TrackKind::importAudio; t.name = "import"; Clip c; c.trackId = t.trackId; c.assetId = a.assetId; c.timelineStartSample = start; c.lengthSamples = length; t.clips.edit().push_back(c); e.tracks.push_back(t); }));
}
RecorderProject stackedProject()
{
    RecorderDocument d;
    auto first = takeFixture(1, 1000, true, 1), second = takeFixture(2, 500, true, 1);
    ok(d.placeTake(first.take, first.assets)); ok(d.placeTake(second.take, second.assets));
    RecorderProject p = d.getProject(); TakeStack s; s.spanSamples = 1000; TakeVersion v1, v2;
    for (auto& track : p.tracks)
        for (auto& c : track.clips.edit())
        {
            const bool old = c.timelineStartSample == 0; auto& v = old ? v1 : v2;
            c.timelineStartSample = 0; c.takeStackId = s.stackId; c.versionId = v.versionId; v.clipIds.push_back(c.clipId);
        }
    s.versions = {v1, v2}; s.activeVersionId = v2.versionId; p.takeStacks.push_back(s); ok(p.validate()); return p;
}
struct Journal : IEditJournalSink
{
    std::vector<EditDelta> edits;
    bool reject = false;
    juce::Result enqueue(const EditDelta& delta) override { edits.push_back(delta); return reject ? juce::Result::fail("journal queue failed") : juce::Result::ok(); }
};
void replay(juce::var& state, const EditDelta& delta)
{
    for (const auto& change : delta.entities)
    {
        if (change.collection == "project") { state.getDynamicObject()->setProperty("name", change.value); continue; }
        const bool order = change.collection.endsWith("Order");
        const auto collection = order ? change.collection.dropLastCharacters(5) : change.collection;
        auto values = state.getProperty(collection, {}); require(values.isArray(), "Delta collection exists");
        const auto idField = collection == "tracks" ? "trackId" : collection == "markers" ? "markerId" : collection == "linkGroups" ? "linkGroupId" : "stackId";
        auto& list = *values.getArray();
        if (order)
        {
            juce::Array<juce::var> sorted;
            for (const auto& id : *change.value.getArray()) for (const auto& value : list) if (value.getProperty(idField, {}) == id) sorted.add(value);
            require(sorted.size() == list.size(), "Delta order covers all entities"); list = sorted;
        }
        else
        {
            int index = -1; for (int i = 0; i < list.size(); ++i) if (list[i].getProperty(idField, {}).toString() == change.entityId) index = i;
            if (change.removed) { require(index >= 0, "Removed entity exists"); list.remove(index); }
            else if (index >= 0) list.set(index, change.value); else list.add(change.value);
        }
        state.getDynamicObject()->setProperty(collection, values);
    }
    require(RecorderSerializer::fingerprint(state) == delta.validationHash, "Result delta replays to the validation hash");
}
}   // namespace
int runProjectTests()
{
    int passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& body)
    {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("identity and isolated user settings root", []
    {
        TempProject f; require(ProductIdentity::settingsDirectory(f.root) == f.root, "Test settings stay isolated");
        require(ProductIdentity::settingsDirectory().getFileName() == RECORDER_SETTINGS_FOLDER, "AppData folder identity");
        require(ProductIdentity::projectFileName() == "project" + ProductIdentity::projectExtension(), "Project extension from identity");
        require(ProductIdentity::fileType() == RECORDER_FILE_TYPE && ProductIdentity::appId().isNotEmpty(), "Stable file and application IDs");
    });
    test("empty project save reopen resave is byte identical without devices", []
    {
        TempProject f; RecorderDocument a; a.newProject(juce::String::fromUTF8("마이크 없는 프로젝트")); ok(a.saveCheckpoint(f.checkpoint()));
        juce::MemoryBlock first, second; require(f.checkpoint().loadFileAsData(first), "Read saved bytes");
        RecorderDocument b; ok(b.openCheckpoint(f.checkpoint())); require(b.getProject().tracks.empty() && b.getProject().media->assets.empty(), "No fake camera or mic assets");
        ok(b.saveCheckpoint(f.checkpoint())); require(f.checkpoint().loadFileAsData(second) && first == second, "Byte-identical roundtrip");
        require(!b.isDirty() && b.getProject().projectId == a.getProject().projectId, "Document identity and saved state restored");
    });
    test("two cameras eight microphones and chunks roundtrip", []
    {
        auto p = placedProject(true, 8); auto registry = std::make_shared<MediaRegistry>(*p.media); auto& mic = registry->assets.back();
        mic.relativePath.clear(); mic.chunks = {{"media/takes/chunks/000001.wav", {0, 24000}}, {"media/takes/chunks/000002.wav", {24000, 24000}}}; p.media = registry;
        ok(p.validate()); require(p.tracks.size() == 10, "2 camera + 8 microphone lanes");
        RecorderProject r; const auto encoded = RecorderSerializer::toJson(p); ok(RecorderSerializer::fromJson(encoded, r)); require(RecorderSerializer::toJson(r) == encoded, "Full metadata roundtrip");
        require(r.tracks.back().clips.items().size() == 1, "Chunk boundary is not an edit boundary");
    });
    test("single-camera normal take with zero microphones", []
    {
        auto p = placedProject(); ok(p.validate()); require(p.tracks.size() == 1 && p.media->assets.size() == 1, "No dummy mic or camera 2");
        require(p.linkGroups.empty() && p.media->takes.front().cam2AssetId.isEmpty(), "Optional references stay absent");
    });
    test("checkpoint preserves exact prior generation and rejects invalid replacement", []
    {
        TempProject f; RecorderDocument d; ok(d.saveCheckpoint(f.checkpoint())); const auto first = f.checkpoint().loadFileAsString();
        ok(d.performEdit("rename", [](EditState& e) { e.name = "second"; })); ok(d.saveCheckpoint(f.checkpoint()));
        const auto backup = f.checkpoint().getSiblingFile(f.checkpoint().getFileName() + ".bak"); require(backup.loadFileAsString() == first, "Previous verified bytes in backup");
        auto invalid = d.getProject(); invalid.Fs = 0; const auto current = f.checkpoint().loadFileAsString();
        require(RecorderSerializer::writeCheckpoint(f.checkpoint(), invalid).failed(), "Invalid replacement rejected");
        require(f.checkpoint().loadFileAsString() == current && backup.loadFileAsString() == first, "Failed write leaves both generations intact");
    });
    test("backup fallback reports recovery without overwriting damaged primary", []
    {
        TempProject f; RecorderDocument d; ok(d.saveCheckpoint(f.checkpoint())); ok(d.performEdit("name", [](EditState& e) { e.name = "new"; })); ok(d.saveCheckpoint(f.checkpoint()));
        require(f.checkpoint().replaceWithText("{broken"), "Damage primary fixture"); RecorderDocument recovered; ok(recovered.openCheckpoint(f.checkpoint()));
        require(recovered.getRecoveryMessage().isNotEmpty() && recovered.getProject().editRevision == 0, "Backup revision and recovery visible");
        require(f.checkpoint().loadFileAsString() == "{broken", "Opening does not modify evidence");
        require(recovered.saveCheckpoint(f.checkpoint()).failed(), "Saving cannot overwrite corrupt primary");
    });
    test("future and foreign schema never fall back to an older backup", []
    {
        TempProject f; RecorderProject p; ok(RecorderSerializer::writeCheckpoint(f.checkpoint(), p)); ok(RecorderSerializer::writeCheckpoint(f.checkpoint(), p));
        for (bool future : {true, false})
        {
            auto root = juce::JSON::parse(RecorderSerializer::toJson(p));
            root.getDynamicObject()->setProperty(future ? "schemaVersion" : "format", future ? juce::var(99) : juce::var("LiveMix.Session"));
            require(f.checkpoint().replaceWithText(juce::JSON::toString(root)), "Write schema fixture"); RecorderProject retained; const auto before = retained.projectId;
            require(RecorderSerializer::readCheckpoint(f.checkpoint(), retained).failed() && retained.projectId == before, "Refuse downgrade and preserve output");
        }
    });
    test("checksum corruption missing fields and wrong numeric types rejected", []
    {
        RecorderProject p; const auto text = RecorderSerializer::toJson(p); RecorderProject out;
        require(RecorderSerializer::fromJson(text.replace("48000", "44100"), out).failed(), "Corruption detected");
        for (const auto& malformed : {juce::var(48000.25), juce::var(true), juce::var("48000")})
        {
            auto root = juce::JSON::parse(text); root.getDynamicObject()->removeProperty("checksum"); root.getDynamicObject()->setProperty("Fs", malformed);
            root.getDynamicObject()->setProperty("checksum", RecorderSerializer::fingerprint(root));
            require(RecorderSerializer::fromJson(juce::JSON::toString(root), out).failed(), "No coercion into integer fields");
        }
        auto root = juce::JSON::parse(text); root.getDynamicObject()->removeProperty("checksum"); root.getDynamicObject()->removeProperty("fps"); root.getDynamicObject()->setProperty("checksum", RecorderSerializer::fingerprint(root));
        require(RecorderSerializer::fromJson(juce::JSON::toString(root), out).failed(), "Missing required field rejected");
    });
    test("64-bit positions above double precision survive JSON exactly", []
    {
        RecorderDocument d; constexpr Sample start = 9007199254740993ll; addImport(d, start, 12345);
        RecorderProject loaded; const auto encoded = RecorderSerializer::toJson(d.getProject()); ok(RecorderSerializer::fromJson(encoded, loaded));
        require(loaded.tracks[0].clips.items()[0].timelineStartSample == start, "Integer sample above 2^53");
        require(RecorderSerializer::toJson(loaded) == encoded, "Large integer canonical bytes");
    });
    test("absolute traversal device and duplicate asset paths rejected", []
    {
        for (const char* bad : {"C:/outside.mp4", "../outside.mp4", "media/../outside.mp4", "media//cam.mp4", "media/a\\cam.mp4", "media/NUL.mp4", "media/cam.mp4:stream", "media/a./cam.mp4"})
        { auto p = placedProject(); auto m = std::make_shared<MediaRegistry>(*p.media); m->assets[0].relativePath = bad; p.media = m; assertInvalid(p); }
        auto p = placedProject(true); auto m = std::make_shared<MediaRegistry>(*p.media); m->assets[1].relativePath = m->assets[0].relativePath.toUpperCase(); p.media = m; assertInvalid(p);
    });
    test("invalid asset clip bounds IDs and overflow rejected", []
    {
        auto p = placedProject(); auto invalid = p; firstClip(invalid).lengthSamples = 0; assertInvalid(invalid);
        invalid = p; firstClip(invalid).sourceIn = -1; assertInvalid(invalid);
        invalid = p; firstClip(invalid).sourceIn = 1; assertInvalid(invalid);
        invalid = p; firstClip(invalid).timelineStartSample = (std::numeric_limits<Sample>::max)(); assertInvalid(invalid);
        invalid = p; firstClip(invalid).assetId = newId(); assertInvalid(invalid);
        invalid = p; firstClip(invalid).trackId = newId(); assertInvalid(invalid);
        invalid = p; firstClip(invalid).clipId = p.projectId; assertInvalid(invalid);
        invalid = p; auto m = std::make_shared<MediaRegistry>(*p.media); m->assets[0].logicalLength = 0; invalid.media = m; assertInvalid(invalid);
    });
    test("half-open adjacency accepted active overlap rejected", []
    {
        auto p = placedProject(); auto& clips = p.tracks[0].clips.edit(); auto next = clips[0]; next.clipId = newId(); next.timelineStartSample = clips[0].timelineEnd(); clips.push_back(next); ok(p.validate());
        clips[1].timelineStartSample -= 1; assertInvalid(p);
    });
    test("explicit gaps and partial camera lengths keep valid available source ranges", []
    {
        auto p = placedProject(true); auto m = std::make_shared<MediaRegistry>(*p.media); m->assets[1].availableRanges = {{0, 24000}}; m->assets[1].gaps = {{24000, 24000}}; p.media = m;
        ok(p.validate()); require(p.activeTimelineEnd() == 48000, "Gap does not truncate healthy camera");
        m = std::make_shared<MediaRegistry>(*p.media); m->assets[1].gaps = {{23999, 24001}}; p.media = m; assertInvalid(p);
    });
    test("link group bidirectional membership and independent relinking", []
    {
        auto p = placedProject(true, 1); require(p.linkGroups.size() == 1, "Take placed as one edit link");
        auto invalid = p; invalid.linkGroups[0].clipIds.pop_back(); assertInvalid(invalid);
        invalid = p; invalid.linkGroups[0].clipIds.push_back(invalid.linkGroups[0].clipIds[0]); assertInvalid(invalid);
        p.tracks[2].clips.edit()[0].linkGroupId.clear(); p.linkGroups[0].clipIds.pop_back(); p.tracks[2].clips.edit()[0].timelineStartSample = 101; ok(p.validate());
        p.tracks[2].clips.edit()[0].linkGroupId = p.linkGroups[0].linkGroupId; p.linkGroups[0].clipIds.push_back(p.tracks[2].clips.items()[0].clipId); ok(p.validate());
        require(p.tracks[2].clips.items()[0].timelineStartSample == 101, "Relink keeps independent offset");
    });
    test("take stack atomic active version excludes old cameras and audio tail", []
    {
        auto p = stackedProject(); auto& mic = p.tracks.back().clips.edit()[0]; mic.timelineStartSample = 9000; mic.linkGroupId.clear(); p.linkGroups[0].clipIds.pop_back();
        ok(p.validate()); require(p.activeTimelineEnd() == 500, "Inactive version and its moved microphone excluded");
        require(!p.isActive(p.tracks[0].clips.items()[0]) && p.isActive(p.tracks[1].clips.items()[1]), "Both cameras follow active version");
        p.takeStacks[0].activeVersionId = p.takeStacks[0].versions[0].versionId; ok(p.validate()); require(p.activeTimelineEnd() == 10000, "Whole old version including independent mic restored");
        RecorderProject roundtrip; ok(RecorderSerializer::fromJson(RecorderSerializer::toJson(p), roundtrip)); require(roundtrip.takeStacks[0].activeVersionId == p.takeStacks[0].activeVersionId, "Version IDs survive serialization");
    });
    test("take stack foreign membership dangling version and import membership rejected", []
    {
        auto p = stackedProject(); auto invalid = p; invalid.takeStacks[0].activeVersionId = newId(); assertInvalid(invalid);
        invalid = p; invalid.takeStacks[0].versions[0].clipIds.pop_back(); assertInvalid(invalid);
        invalid = p; invalid.tracks[0].clips.edit()[0].versionId = p.takeStacks[0].activeVersionId; assertInvalid(invalid);
        invalid = p; invalid.tracks[0].clips.edit()[0].timelineStartSample = 1; assertInvalid(invalid);
        invalid = p; auto a = source(AssetKind::importAudio, 1000); auto m = std::make_shared<MediaRegistry>(*p.media); m->assets.push_back(a); invalid.media = m;
        Track t; t.kind = TrackKind::importAudio; Clip c; c.trackId = t.trackId; c.assetId = a.assetId; c.lengthSamples = 1000; c.takeStackId = p.takeStacks[0].stackId; c.versionId = p.takeStacks[0].activeVersionId;
        t.clips.edit().push_back(c); invalid.tracks.push_back(t); invalid.takeStacks[0].versions[1].clipIds.push_back(c.clipId); assertInvalid(invalid);
    });
    test("origin-based frame/sample conversion has no three-hour cumulative drift", []
    {
        for (std::uint32_t Fs : {44100u, 48000u}) for (FrameRate fps : {FrameRate{30, 1}, FrameRate{60, 1}, FrameRate{60000, 1001}})
        {
            const Sample frames = 3ll * 60 * 60 * fps.numerator / fps.denominator;
            for (Sample frame = 0; frame <= frames; ++frame)
            {
                const auto sample = frameToSample(frame, Fs, fps); const auto exactNumerator = frame * Fs * fps.denominator;
                require(std::llabs(sample * fps.numerator - exactNumerator) * 2 <= fps.numerator, "Absolute rational boundary error <= 0.5 sample");
                require(sampleToFrame(sample, Fs, fps) == frame, "Each frame boundary roundtrips");
            }
        }
    });
    test("signed ties invalid rates and 128-bit intermediates", []
    {
        require(frameToSample(1, 10, {4, 1}) == 3 && frameToSample(-1, 10, {4, 1}) == -3, "Half ties away from zero");
        const auto maximum = (std::numeric_limits<Sample>::max)(), minimum = (std::numeric_limits<Sample>::min)();
        require(rescaleRound(maximum, 2, 2) == maximum && rescaleRound(minimum, 1, 1) == minimum, "Full int64 domain");
        rejects([&] { rescaleRound(maximum, 2, 1); }); rejects([] { frameToSample(1, 0, {30, 1}); }); rejects([] { sampleToFrame(1, 48000, {0, 1}); });
        rejects([] { frameToSample(1, 48000, {30, 0}); });
    });
    test("metadata snapshots share unchanged clips and invalid edits never publish", []
    {
        RecorderDocument d; const auto f = takeFixture(); ok(d.placeTake(f.take, f.assets)); const auto before = d.snapshot(); const auto depth = d.getHistory().undoDepth();
        require(d.performEdit("invalid", [](EditState& e) { e.tracks[0].clips.edit()[0].lengthSamples = 0; }).failed(), "Invalid edit rejected");
        require(d.snapshot() == before && d.getHistory().undoDepth() == depth, "Publication and history are atomic");
        ok(d.performEdit("marker", [](EditState& e) { Marker m; m.sample = 17; e.markers.push_back(m); }));
        require(&d.getProject().tracks[0].clips.items() == &before->tracks[0].clips.items(), "Unchanged clip lists share storage");
        ok(d.performEdit("trim", [](EditState& e) { e.tracks[0].clips.edit()[0].lengthSamples = 24000; }));
        require(before->tracks[0].clips.items()[0].lengthSamples == 48000, "Published snapshot immutable after COW edit");
    });
    test("undo redo restores selection and metadata while keeping media registry", []
    {
        RecorderDocument d; auto first = takeFixture(); ok(d.placeTake(first.take, first.assets)); const auto clipId = d.getProject().tracks[0].clips.items()[0].clipId;
        d.setSelection({clipId}); ok(d.performEdit("delete", [](EditState& e) { e.tracks[0].clips.edit().clear(); })); require(d.getSelection().empty(), "Deleted selection pruned");
        auto second = takeFixture(2); ok(d.registerMedia(second.assets, {second.take})); const auto registry = d.getProject().media; const auto revision = d.getProject().editRevision;
        ok(d.undo()); require(d.getSelection() == std::vector<Id>{clipId} && d.getProject().findClip(clipId) != nullptr, "Snapshot and selection restored");
        require(d.getProject().media == registry && registry->takes.size() == 2, "Registry excluded from undo");
        ok(d.redo()); require(d.getProject().findClip(clipId) == nullptr && d.getProject().editRevision == revision + 2, "Redo publishes a fresh revision");
    });
    test("700ms merge gesture merge history limit and redo barriers", []
    {
        RecorderDocument d;
        const auto rename = [&](const char* name, EditOptions options) { ok(d.performEdit("rename", [&](EditState& e) { e.name = name; }, options)); };
        rename("a", {"name", {}, 1000}); rename("b", {"name", {}, 1700}); require(d.getHistory().undoDepth() == 1, "Inclusive 700ms numeric merge");
        rename("c", {"name", {}, 2401}); require(d.getHistory().undoDepth() == 2, "Expired merge window");
        ok(d.undo()); require(d.getProject().name == "b", "Merged previous snapshot"); ok(d.redo());
        rename("d", {"name", {}, 2402}); require(d.getHistory().undoDepth() == 3, "Redo creates a merge barrier");
        rename("e", {{}, "drag-1", 3000}); rename("f", {{}, "drag-1", 10000}); require(d.getHistory().undoDepth() == 4, "Whole drag is one step regardless of duration");
        d.endGesture(); rename("g", {{}, "drag-1", 10001}); require(d.getHistory().undoDepth() == 5, "Explicit gesture end");
        for (int i = 0; i < 220; ++i) ok(d.performEdit("name", [i](EditState& e) { e.name = juce::String(i); }));
        require(d.getHistory().undoDepth() == 200, "Bounded 200-step history");
        for (int i = 0; i < 200; ++i) ok(d.undo()); require(d.getHistory().undoDepth() == 0 && d.getHistory().redoDepth() == 200, "All retained snapshots restorable");
    });
    test("selection and no-op edits do not create revisions or clear redo", []
    {
        RecorderDocument d; auto f = takeFixture(); ok(d.placeTake(f.take, f.assets)); ok(d.performEdit("name", [](EditState& e) { e.name = "name"; })); ok(d.undo());
        const auto revision = d.getProject().editRevision; d.setSelection({d.getProject().tracks[0].clips.items()[0].clipId}); ok(d.performEdit("noop", [](EditState&) {}));
        require(d.getProject().editRevision == revision && d.getHistory().redoDepth() == 1, "No-op leaves history and revision unchanged");
    });
    test("normal take placement includes independent audio and excludes markers", []
    {
        RecorderDocument d; auto a = takeFixture(1, 100); ok(d.placeTake(a.take, a.assets)); addImport(d, 1000, 2000);
        ok(d.performEdit("marker", [](EditState& e) { Marker m; m.sample = 50000; e.markers.push_back(m); }));
        auto b = takeFixture(2, 200); ok(d.placeTake(b.take, b.assets)); require(d.getProject().media->takes.back().placementSample == 3000, "Independent audio determines placement");
        require(d.getProject().tracks[0].clips.items().back().timelineStartSample == 3000, "No artificial gap");
        ok(d.undo()); require(d.getProject().media->takes.size() == 2, "Undo placement keeps take available"); ok(d.placeTake(b.take.takeId)); require(d.getProject().activeTimelineEnd() == 3200, "Original can be reinserted");
    });
    test("dub placement keeps off-grid Pstart and immutable timebase after media", []
    {
        RecorderDocument d; ok(d.setTimebase(48000, {60, 1})); addImport(d, 0, 100000);
        auto take = takeFixture(1, 1000); take.take.mode = TakeMode::dub; take.take.O0 = 9007199254740993ll; take.take.placementSample = 37;
        ok(d.placeTake(take.take, take.assets)); require(d.getProject().tracks.back().clips.items()[0].timelineStartSample == 37, "Pstart not frame-snapped or appended");
        const auto snapshot = d.snapshot(); require(d.setTimebase(44100, {60, 1}).failed() && d.snapshot() == snapshot, "Fs frozen after first media");
        require(d.setTimebase(48000, {30, 1}).failed(), "Project fps frozen after first media");
    });
    test("take number timestamp and display name assigned automatically", []
    {
        RecorderDocument d;
        for (int expected = 1; expected <= 2; ++expected)
        {
            auto f = takeFixture(); f.take.number = 0; f.take.createdAt.clear(); f.take.name.clear(); ok(d.placeTake(f.take, f.assets));
            const auto& take = d.getProject().media->takes.back(); require(take.number == expected && take.createdAt.isNotEmpty()
                && take.name.startsWith(juce::String::fromUTF8("테이크 ")), "Automatic take identity without caller numbering");
        }
    });
    test("journal result deltas replay name entities ordering deletion undo and redo", []
    {
        RecorderDocument d; Journal j; d.setJournalSink(&j); auto state = RecorderSerializer::editStateToVar(d.getProject());
        ok(d.performEdit("markers", [](EditState& e) { e.markers.push_back(Marker{}); e.markers.push_back(Marker{}); e.name = "renamed"; }));
        ok(d.performEdit("reverse", [](EditState& e) { std::reverse(e.markers.begin(), e.markers.end()); }));
        ok(d.performEdit("delete", [](EditState& e) { e.markers.erase(e.markers.begin()); })); ok(d.undo()); ok(d.redo());
        Sample expected = 0; for (const auto& edit : j.edits) { require(edit.baseRevision == expected && edit.revision == ++expected, "Contiguous revision delta"); replay(state, edit); }
        require(juce::JSON::toString(state, true) == juce::JSON::toString(RecorderSerializer::editStateToVar(d.getProject()), true), "Checkpoint plus deltas recovers exact state");
        j.reject = true; ok(d.performEdit("queue failure", [](EditState& e) { e.name = "unsaved"; }));
        require(d.getProject().name == "unsaved" && d.isDirty() && d.getError().isNotEmpty(), "Queue failure preserves edit and exposes unsaved state");
        auto foreign = j.edits.back(); foreign.projectId = newId(); d.acknowledgeJournal(foreign, juce::Result::ok()); require(d.durableRevision() == 0, "Foreign acknowledgement ignored");
        d.setJournalSink(nullptr);
    });
    test("raw JSON integer overflow cannot wrap into a valid schema", []
    {
        RecorderProject p, out;
        const auto encoded = RecorderSerializer::toJson(p);
        require(RecorderSerializer::fromJson(encoded.replace("48000", "18446744073709599616"), out).failed(), "Overflow that wraps to 48000 rejected before JUCE parsing");
        require(RecorderSerializer::fromJson(encoded.replace("48000", "048000"), out).failed(), "Leading-zero numeric token rejected");
    });
    test("checkpoint folder layout failed backup and stale revisions", []
    {
        TempProject f; RecorderProject p; ok(RecorderSerializer::writeCheckpoint(f.checkpoint(), p));
        for (const auto* path : {"journal", "media/takes", "media/imports", "cache", "recovery", "exports"}) require(f.root.getChildFile(path).isDirectory(), "Project-local directory layout");
        const auto before = f.checkpoint().loadFileAsString(); const auto backup = f.checkpoint().getSiblingFile(f.checkpoint().getFileName() + ".bak"); ok(backup.createDirectory());
        p.editRevision = 1; require(RecorderSerializer::writeCheckpoint(f.checkpoint(), p).failed(), "Backup failure propagated"); require(f.checkpoint().loadFileAsString() == before, "Primary survives backup failure");
        require(backup.deleteFile(), "Remove empty fixture directory"); ok(RecorderSerializer::writeCheckpoint(f.checkpoint(), p));
        p.editRevision = 0; require(RecorderSerializer::writeCheckpoint(f.checkpoint(), p).failed(), "Stale checkpoint cannot replace newer revision");
    });
    test("journal acknowledgement advances saved status only for matching transactions", []
    {
        TempProject f; RecorderDocument d; ok(d.saveCheckpoint(f.checkpoint())); Journal j; d.setJournalSink(&j);
        ok(d.performEdit("name", [](EditState& e) { e.name = "pending"; })); const auto first = j.edits.back();
        auto wrong = first; wrong.transactionId = newId(); d.acknowledgeJournal(wrong, juce::Result::ok()); require(d.isDirty(), "Unknown transaction cannot mark saved");
        d.acknowledgeJournal(first, juce::Result::ok()); require(!d.isDirty() && d.durableRevision() == 1, "Journal flush marks pure edit saved");
        const auto old = d.snapshot(); ok(d.performEdit("name", [](EditState& e) { e.name = "later"; }));
        d.checkpointFinished(old, f.checkpoint(), juce::Result::ok()); require(d.isDirty(), "Stale checkpoint completion cannot clear later edit");
        RecorderProject reopened = *old; ok(d.adopt(reopened, f.checkpoint(), {})); ok(d.performEdit("new opening", [](EditState& e) { e.name = "current opening"; }));
        auto stale = j.edits[j.edits.size() - 2]; d.acknowledgeJournal(stale, juce::Result::ok()); require(d.isDirty(), "Old opening transaction cannot acknowledge new opening at same revision");
        d.setJournalSink(nullptr);
    });
    test("finalisation registry changes survive undo and create no history", []
    {
        RecorderDocument d; auto take = takeFixture(); ok(d.placeTake(take.take, take.assets));
        ok(d.performEdit("name", [](EditState& e) { e.name = "edited"; })); const auto depth = d.getHistory().undoDepth(); const auto revision = d.getProject().editRevision;
        auto source = d.getProject().media->assets[0]; source.relativePath = "media/takes/final/cam1.mp4"; ++source.mediaGeneration;
        ok(d.updateMediaAsset(source)); ok(d.updateTakeState(take.take.takeId, TakeState::partial));
        require(d.getHistory().undoDepth() == depth && d.getProject().editRevision == revision, "Finalisation is not a user edit");
        ok(d.undo()); require(d.getProject().media->assets[0].relativePath == source.relativePath && d.getProject().media->takes[0].state == TakeState::partial, "Undo keeps latest registry availability");
        require(d.updateMediaAsset(source).failed(), "Generation regression or duplicate generation rejected");
    });
    test("import source mapping uses native rational rate and nested edits are rejected", []
    {
        RecorderDocument d; auto a = source(AssetKind::importAudio, 1000); a.originalFormat.sampleRate = 44100; a.sourceUnitsNumerator = 147; a.sourceUnitsDenominator = 160; ok(d.registerMedia({a}));
        auto invalid = d.getProject(); auto registry = std::make_shared<MediaRegistry>(*invalid.media); registry->assets[0].sourceUnitsNumerator = 1; invalid.media = registry; assertInvalid(invalid);
        ok(d.performEdit("outer", [&](EditState& e) { require(d.performEdit("inner", [](EditState& inner) { inner.name = "inner"; }).failed(), "Reentrant edit rejected"); e.name = "outer"; }));
        require(d.getProject().name == "outer" && d.getProject().editRevision == 1, "One atomic outer edit");
    });
    test("settings worker persists devices mapping correction recent paths and window", []
    {
        TempProject f;
        {
            RecorderSettings s(f.root); ok(s.load()); auto state = s.get(); state.asioDeviceId = "missing-but-preserved"; state.output = {false, 7, 2, -1};
            state.cameraDeviceIds = {"camera-one", "camera-two"}; state.cameraModes = {"1080p 60000/1001", "1080p 30/1"}; state.cameraEnabled[1] = true; state.physicalInputs = {0, 5, 7};
            state.calibration.cameraOffsetSamples = {37, -19}; state.calibration.inputOffsetSamples = -13; state.calibration.outputOffsetSamples = 200;
            state.calibration.calibrationDate = "2026-09-09"; state.calibration.calibrationIdentity = "profile"; state.windowState = "window-state";
            ok(s.set(state)); s.rememberProject(f.checkpoint()); auto first = s.save(); state = s.get(); state.bufferSize = 512; ok(s.set(state)); auto last = s.save(); ok(first.get()); ok(last.get());
        }
        RecorderSettings loaded(f.root); ok(loaded.load()); const auto& s = loaded.get();
        require(s.asioDeviceId == "missing-but-preserved" && s.output.left == 7 && s.output.right == 2 && s.bufferSize == 512, "FIFO saves keep explicit non-contiguous mapping");
        require(s.calibration.cameraOffsetSamples[1] == -19 && s.calibration.outputOffsetSamples == 200 && s.windowState == "window-state", "Correction and window retained");
        require(s.recentProjects[0] == f.checkpoint().getFullPathName(), "Recent projects retained");
    });
    test("settings invalid channels duplicate cameras and failed worker writes", []
    {
        require(OutputMapping{false, 2, 2, -1}.validate().failed(), "Duplicate L/R rejected"); ok(OutputMapping{true, -1, -1, 5}.validate());
        UserSettings s; s.cameraEnabled = {true, true}; s.cameraDeviceIds = {"same", "same"}; require(s.validate().failed(), "Duplicate camera slots rejected");
        s = {}; s.physicalInputs = {1, 1}; require(s.validate().failed(), "Duplicate physical inputs rejected");
        TempProject f; const auto blocked = f.root.getChildFile("blocked"); require(blocked.replaceWithText("file"), "Create settings path blocker");
        RecorderSettings settings(blocked); require(settings.save().get().failed(), "Worker propagates I/O failure");
    });
    test("corrupt settings integer does not silently reset a device mapping", []
    {
        TempProject f; RecorderSettings s(f.root); auto state = s.get(); state.output = {false, 7, 2, -1}; state.asioDeviceId = "selected"; ok(s.set(state)); ok(s.save().get());
        const auto xml = juce::parseXML(s.getFile().loadFileAsString()); require(xml != nullptr, "Properties XML available");
        juce::PropertySet properties; properties.restoreFromXml(*xml); properties.setValue("outputLeft", "invalid");
        require(s.getFile().replaceWithText(properties.createXml("RECORDER_SETTINGS")->toString()), "Write malformed property fixture");
        require(s.load().failed() && s.get().output.left == 7 && s.get().asioDeviceId == "selected", "Bad settings leave current mapping intact");
    });
    std::cout << "project-roundtrip: " << passed << " passed, " << failed << " failed\n"; return failed == 0 ? 0 : 1;
}
// Suite routing lives in tests/TestMain.cpp (registry); this file only provides runProjectTests().
