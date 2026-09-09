#include "TestSupport.h"
#include "app/RecorderDocument.h"
#include "model/TakeStackEdits.h"
#include "media/AudioImport.h"
#include <algorithm>

using namespace gocue::recorder;
using recorder_test::require;
namespace
{
void ok(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
juce::String hash(const RecorderProject& p) { return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)); }
struct Fixture
{
    RecorderDocument document;
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("TakeStackTests-" + newId());
    std::vector<Id> versions; Id stack, importedClip;
    Fixture()
    {
        document.newProject("Retake fixture", 48000, {60,1});
        ok(document.performEdit("import fixture", [&](EditState& edit)
        {
            auto& p = static_cast<RecorderProject&>(edit); auto media = std::make_shared<MediaRegistry>(*p.media);
            MediaAsset a; a.kind = AssetKind::importAudio; a.relativePath = "media/imports/finished.wav"; a.logicalLength = 960000;
            a.availableRanges = {{0,a.logicalLength}}; a.contentIdentity = "finished-original-hash";
            a.originalFormat.codec = "pcm_s24le"; a.originalFormat.sampleRate = 48000; a.originalFormat.channels = 2; a.originalFormat.bitsPerSample = 24;
            media->assets.push_back(a); p.media = media; Track t; t.kind = TrackKind::importAudio; t.name = "완성 오디오";
            Clip c; c.trackId = t.trackId; c.assetId = a.assetId; c.lengthSamples = a.logicalLength; importedClip = c.clipId;
            t.clips.edit().push_back(c); p.tracks.push_back(t);
        }));
    }
    void add(Sample start, Sample span, Sample length, bool mic = true, const Id& retake = {})
    {
        Take t; t.mode = TakeMode::dub; t.O0 = t.N0 = 100000; t.placementSample = start; t.logicalLength = length;
        std::vector<MediaAsset> assets;
        for (int i = 0; i < (mic ? 3 : 2); ++i)
        {
            MediaAsset a; a.kind = i < 2 ? AssetKind::camera : AssetKind::mic; a.logicalLength = length; a.availableRanges = {{0,length}};
            a.relativePath = "media/takes/" + a.assetId + ".fixture";
            auto f = directory.getChildFile(a.relativePath); ok(f.getParentDirectory().createDirectory()); require(f.replaceWithText("original " + a.assetId), "write source fixture");
            AudioImportControl control; a.contentIdentity = AudioImport::hashFile(f, control);
            if (i < 2)
            {
                a.originalFormat.codec = "h264"; a.originalFormat.width = 1920; a.originalFormat.height = 1080; a.originalFormat.fps = {60,1};
                a.sourceUnitsNumerator = 60; a.sourceUnitsDenominator = 48000; (i == 0 ? t.cam1AssetId : t.cam2AssetId) = a.assetId;
            }
            else
            {
                a.originalFormat.codec = "pcm_s24le"; a.originalFormat.sampleRate = 48000; a.originalFormat.channels = 1; a.originalFormat.bitsPerSample = 24;
                t.microphoneAssetIds.push_back(a.assetId); t.capture.physicalInputs.push_back(2);
            }
            assets.push_back(a);
        }
        ok(document.placeDubbingTake(t, assets, mic ? std::vector<int>{5} : std::vector<int>{}, {start,span}, retake));
        for (const auto& track : document.getProject().tracks) for (const auto& c : track.clips.items()) if (c.assetId == t.cam1AssetId) { stack = c.takeStackId; versions.push_back(c.versionId); }
    }
    Id activeMic() const
    {
        for (const auto& t : document.getProject().tracks) if (t.kind == TrackKind::mic)
            for (const auto& c : t.clips.items()) if (document.getProject().isActive(c) && c.takeStackId == stack) return c.clipId;
        return {};
    }
};
}
int runTakeStackTests()
{
    recorder_test::Suite tests;
    tests.test("Three retakes, second restore, one undo/redo, checkpoint and original SHA256", []
    {
        Fixture f; f.add(137, 9600, 9600); const auto stack = f.stack;
        for (int i = 0; i < 3; ++i) f.add(137, 9600, 9600 - i * 800, true, stack);
        const auto before = hash(f.document.getProject()); const auto depth = f.document.getHistory().undoDepth();
        ok(f.document.useTakeVersion(stack, f.versions[1])); const auto restored = hash(f.document.getProject());
        require(f.document.getHistory().undoDepth() == depth + 1, "Version activation is exactly one undo step");
        ok(f.document.undo()); require(hash(f.document.getProject()) == before, "Undo restores all placements");
        ok(f.document.redo()); require(hash(f.document.getProject()) == restored, "Redo restores selected whole version");
        const auto file = f.directory.getChildFile("project.recorder"); ok(f.document.saveCheckpoint(file)); RecorderDocument reopened; ok(reopened.openCheckpoint(file));
        require(hash(reopened.getProject()) == restored, "Reopened source/position metadata hash");
        for (const auto& asset : reopened.getProject().media->assets) if (asset.kind != AssetKind::importAudio)
        { AudioImportControl c; require(AudioImport::hashFile(f.directory.getChildFile(asset.relativePath), c) == asset.contentIdentity, "Every original source SHA256 preserved"); }
        for (const auto& t : reopened.getProject().tracks) if (t.kind == TrackKind::cam1 || t.kind == TrackKind::cam2)
            for (const auto& c : t.clips.items()) if (reopened.getProject().isActive(c)) require(c.versionId == f.versions[1] && c.timelineStartSample == 137, "Two cameras select one whole version");
        require(!RecorderSerializer::toJson(reopened.getProject()).containsIgnoreCase("angleEvent"), "No time-varying camera selector");
        const auto* imported = reopened.getProject().findClip(f.importedClip); require(imported && imported->takeStackId.isEmpty() && imported->timelineStartSample == 0 && imported->lengthSamples == 960000, "Completed audio remains outside stack");
    });
    tests.test("Short active version leaves black/silent tail without old media", []
    {
        Fixture f; f.add(137,9600,9600); f.add(137,9600,3201,false,f.stack);
        const auto& p = f.document.getProject(); const auto* stack = TakeStackEdits::find(p,f.stack);
        require(stack->spanSamples == 9600 && stack->versions.size() == 2, "First stop fixes retake range");
        const auto plan = RenderPlanCompiler::compile(p);
        for (const auto& t : plan->tracks) if (t.kind != TrackKind::importAudio)
            for (const auto& span : t.spans) if (span.timeline.start <= 5000 && span.timeline.start + span.timeline.length > 5000) require(span.isGap(), "No old-version tail fallback");
        require(f.activeMic().isEmpty(), "Mic-off new version disables older optional mic");
    });
    tests.test("Off-grid overlap splits two old stacks and archives the visible combination", []
    {
        Fixture f; f.add(0,4800,4800); const auto firstAsset = f.document.getProject().media->takes.back().cam1AssetId;
        f.add(6000,4800,4800); const auto secondAsset = f.document.getProject().media->takes.back().cam1AssetId;
        f.add(2407,6000,6000); const auto& p = f.document.getProject(); const auto* s = TakeStackEdits::find(p,f.stack);
        require(s && s->versions.size() == 2, "Combined previous version recorded");
        ok(f.document.useTakeVersion(f.stack,s->versions[1].versionId));
        bool left = false, right = false, oldA = false, oldB = false;
        for (const auto& t : f.document.getProject().tracks) if (t.kind == TrackKind::cam1)
            for (const auto& c : t.clips.items()) if (f.document.getProject().isActive(c))
            {
                left |= c.assetId == firstAsset && c.timelineStartSample == 0 && c.lengthSamples == 2407;
                right |= c.assetId == secondAsset && c.timelineStartSample == 8407 && c.sourceIn == 2407;
                oldA |= c.assetId == firstAsset && c.timelineStartSample == 2407 && c.sourceIn == 2407;
                oldB |= c.assetId == secondAsset && c.timelineStartSample == 6000 && c.lengthSamples == 2407;
            }
        require(left && right && oldA && oldB, "Boundary source mapping preserved inside and outside overlap");
    });
    tests.test("Global ripple and stack split transform every version", []
    {
        Fixture f; f.add(1600,9600,9600); f.add(1600,9600,8000,true,f.stack);
        ok(f.document.performEdit("split", {}, [&](const RecorderProject& p) { return TakeStackEdits::split(p,f.stack,4800); }));
        const auto before = f.document.getProject();
        ok(f.document.performEdit("ripple", {}, [&](const RecorderProject& p) { return TakeStackEdits::rippleDeleteAll(p,{0,800}); }));
        for (const auto& t : before.tracks) for (const auto& c : t.clips.items()) if (c.takeStackId == f.stack)
        { const auto* after = f.document.getProject().findClip(c.clipId); require(after && after->sourceIn == c.sourceIn && after->timelineStartSample == c.timelineStartSample - 800, "All versions ripple equally"); }
        const auto* s = TakeStackEdits::find(f.document.getProject(),f.stack); require(s->anchorSample == 800 && s->spanSamples == 9600, "Retake span moves with timeline");
        for (const auto& v : s->versions) require(v.clipIds.size() == 6, "Both cams and mic split in every version");
    });
    tests.test("Unlinked mic edits only active version; activation previews outside-span impact and rejects collision", []
    {
        Fixture f; f.add(1600,9600,9600); const auto first = f.versions.back(); const auto mic = f.activeMic();
        ok(f.document.performEdit("unlink", {}, [&](const RecorderProject& p) { return ClipEdits::unlink(p,{mic}); }));
        ok(f.document.performEdit("move mic", {}, [&](const RecorderProject& p) { return ClipEdits::move(p,{mic},20000); }));
        f.add(1600,9600,9600,true,f.stack);
        const auto* moved = f.document.getProject().findClip(mic); require(moved && moved->timelineStartSample == 21600 && moved->versionId == first, "Inactive mic retained outside stack");
        const auto impact = TakeStackEdits::impact(f.document.getProject(),f.stack,first); ok(impact.status);
        require(impact.range.start == 1600 && impact.range.start + impact.range.length == 31200, "Impact includes moved original mic");
        const auto active = f.activeMic(); ok(f.document.performEdit("unlink new", {}, [&](const RecorderProject& p) { return ClipEdits::unlink(p,{active}); }));
        ok(f.document.performEdit("duplicate unrelated fixture", [&](EditState& e)
        {
            for (auto& t : e.tracks) if (t.kind == TrackKind::mic) { auto c = *f.document.getProject().findClip(active); c.clipId = newId(); c.takeStackId.clear(); c.versionId.clear(); c.linkGroupId.clear(); c.timelineStartSample = 21600; t.clips.edit().push_back(c); }
        }));
        const auto before = hash(f.document.getProject()); const auto depth = f.document.getHistory().undoDepth();
        require(f.document.useTakeVersion(f.stack,first).failed(), "Actual restored mic collision rejected");
        require(hash(f.document.getProject()) == before && f.document.getHistory().undoDepth() == depth, "Rejected activation changes nothing");
    });
    tests.test("Outer trim uses all versions and never mutates original handles", []
    {
        Fixture f; f.add(1600,9600,9600); f.add(1600,9600,9600,true,f.stack);
        ok(f.document.performEdit("trim stack", {}, [&](const RecorderProject& p) { return TakeStackEdits::trimIn(p,f.stack,2400); }));
        for (const auto& t : f.document.getProject().tracks) for (const auto& c : t.clips.items()) if (c.takeStackId == f.stack)
            require(c.sourceIn == 800 && c.timelineStartSample == 2400 && c.lengthSamples == 8800, "All-version trim coordinates");
        require(TakeStackEdits::find(f.document.getProject(),f.stack)->spanSamples == 8800, "Trim updates retake span");
    });
    tests.test("Trim a split stack with a shorter active version preserves its blank tail", []
    {
        Fixture f; f.add(1600,9600,9600); f.add(1600,9600,3200,true,f.stack);
        ok(f.document.performEdit("split",{},[&](const RecorderProject& p) { return TakeStackEdits::split(p,f.stack,3200); }));
        ok(f.document.performEdit("trim out",{},[&](const RecorderProject& p) { return TakeStackEdits::trimOut(p,f.stack,9600); }));
        const auto& p = f.document.getProject(); require(TakeStackEdits::find(p,f.stack)->spanSamples == 8000,"Absolute stack edge trimmed");
        for (const auto& t : p.tracks) for (const auto& c : t.clips.items()) if (c.takeStackId == f.stack)
            require(c.timelineEnd() <= (p.isActive(c) ? 4800 : 9600),"Trim neither extends short take nor shifts split interior");
    });
    return tests.result("take-stack-edits");
}
