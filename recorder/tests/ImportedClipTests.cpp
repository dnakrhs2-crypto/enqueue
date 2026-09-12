#include "TestSupport.h"
#include "media/AudioImport.h"
#include <limits>

using namespace gocue::recorder;
using namespace recorder_test;
namespace recorder_import_test { juce::File writeWav(const juce::File&, std::uint32_t, int, Sample); }
namespace
{
struct Fixture
{
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-clip-test-" + newId());
    RecorderDocument document;
    AudioImportControl control;
    ~Fixture() { directory.deleteRecursively(); }
    std::unique_ptr<PreparedAudioImport> prepare(Sample playhead = 0)
    {
        AudioImportRequest request;
        request.projectDirectory = directory; request.projectId = document.getProject().projectId;
        request.projectFs = document.getProject().Fs; request.playhead = playhead;
        request.source = recorder_import_test::writeWav(directory.getChildFile("fixtures"), 44100, 2, 44101);
        std::unique_ptr<PreparedAudioImport> prepared;
        const auto r = AudioImport::prepare(request, control, prepared); require(r.wasOk(), r.getErrorMessage().toRawUTF8()); return prepared;
    }
};
}
int runImportedClipTests()
{
    Suite suite;
    suite.test("one atomic publication at exact off-grid playhead with independent track kind", []
    {
        Fixture f; const auto before = f.document.snapshot(); auto p = f.prepare(12347); int notifications = 0;
        f.document.onChanged = [&]
        {
            ++notifications; const auto& project = f.document.getProject();
            require(project.validate().wasOk() && project.media->assets.size() == 1 && project.tracks.size() == 1, "no partial publication");
        };
        require(commitImportedAudio(f.document, *p, f.control).wasOk(), "commit");
        const auto& project = f.document.getProject(); const auto& track = project.tracks.front(); const auto& clip = track.clips.items().front();
        require(notifications == 1 && project.editRevision == before->editRevision + 1 && before->media->assets.empty(), "single immutable transaction");
        require(track.kind == TrackKind::importAudio && track.microphoneIndex == -1 && !track.mute && !track.solo, "independent audio defaults");
        require(clip.timelineStartSample == 12347 && clip.sourceIn == 0 && clip.lengthSamples == 48001, "playhead and rescaled length");
        require(clip.linkGroupId.isEmpty() && clip.takeStackId.isEmpty() && clip.versionId.isEmpty() && project.media->takes.empty(), "outside take/link/version");
        require(commitImportedAudio(f.document, *p, f.control).failed(), "duplicate commit rejected");
    });
    suite.test("new project starts at zero; each file gets its own track without microphone limit", []
    {
        Fixture f;
        for (int i = 0; i < 9; ++i) { auto p = f.prepare(); require(commitImportedAudio(f.document, *p, f.control).wasOk(), "independent import"); }
        require(f.document.getProject().tracks.size() == 9 && f.document.getProject().media->takes.empty(), "imports do not occupy microphone lanes");
        for (const auto& t : f.document.getProject().tracks) require(t.clips.items().front().timelineStartSample == 0, "default zero, not append");
        require(f.document.getProject().validate().wasOk(), "model invariants");
    });
    suite.test("mute/solo, undo/redo, checkpoint and sidecar survive without source mutation", []
    {
        Fixture f; auto p = f.prepare(); require(commitImportedAudio(f.document, *p, f.control).wasOk(), "commit");
        const auto id = p->asset().assetId; const auto original = p->originalFile(); const auto hash = p->info().contentHash;
        require(f.document.performEdit("mute/solo", [](EditState& edit) { edit.tracks.front().mute = true; edit.tracks.front().solo = true; }).wasOk(), "flags edit");
        require(f.document.getProject().tracks.front().mute && f.document.getProject().tracks.front().solo, "same model flags as mic");
        require(f.document.undo().wasOk() && !f.document.getProject().tracks.front().mute, "undo flags");
        require(f.document.undo().wasOk() && f.document.getProject().tracks.empty(), "undo import placement");
        require(f.document.getProject().media->findAsset(id) && original.existsAsFile(), "append-only registry survives undo");
        require(f.document.redo().wasOk() && f.document.redo().wasOk(), "redo import and flags");
        require(f.document.saveCheckpoint(f.directory.getChildFile("project.recorder")).wasOk(), "save");
        RecorderDocument reopened; require(reopened.openCheckpoint(f.directory.getChildFile("project.recorder")).wasOk(), "reopen");
        const auto* asset = reopened.getProject().media->findAsset(id); require(asset != nullptr, "asset restored");
        require(AudioImport::loadInfo(f.directory, *asset).decodedSamples == 44101, "sidecar sample count");
        auto altered = *asset; ++altered.logicalLength;
        rejects([&] { AudioImport::loadInfo(f.directory, altered); });
        require(reopened.getProject().tracks.front().mute && reopened.getProject().tracks.front().solo, "flags serialized");
        require(AudioImport::hashFile(original, f.control) == hash && f.document.setTimebase(44100, {}).failed(), "original and Fs immutable");
    });
    suite.test("cancel, changed project, bad placement and revision overflow never register half an asset", []
    {
        Fixture f; auto p = f.prepare(); f.control.cancelled.store(true);
        require(commitImportedAudio(f.document, *p, f.control).failed() && f.document.getProject().media->assets.empty(), "cancel at commit");
        f.control.cancelled.store(false); f.document.newProject("changed");
        require(commitImportedAudio(f.document, *p, f.control).failed() && f.document.getProject().media->assets.empty(), "project identity guard"); p.reset();
        p = f.prepare(); require(f.document.saveCheckpoint(f.directory.getChildFile("relocated/project.recorder")).wasOk(), "relocated checkpoint");
        require(commitImportedAudio(f.document, *p, f.control).failed() && f.document.getProject().media->assets.empty(), "project folder guard"); p.reset();
        f.document.newProject("overflow");
        auto maxProject = *f.document.snapshot(); maxProject.editRevision = (std::numeric_limits<Sample>::max)();
        require(f.document.adopt(maxProject, {}, {}).wasOk(), "overflow project fixture"); p = f.prepare(); const auto before = f.document.snapshot();
        require(commitImportedAudio(f.document, *p, f.control).failed() && f.document.snapshot() == before, "publish failure rolls back registry and clips");
        rejects([&] { f.prepare(-1); }); rejects([&] { f.prepare((std::numeric_limits<Sample>::max)()); });
    });
    return suite.result("ImportedClipTests");
}
