#include "TestSupport.h"
#include "ui/TimelineView.automation.h"
#include <limits>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
constexpr Sample S = 48000;
juce::String hash(const RecorderProject& p) { return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)); }
}
int runReorderTests()
{
    Suite suite;
    suite.test("UI neighbour insertion moves corresponding clips on all lanes", []
    {
        RecorderDocument d; const auto p = makeTimelineUiFixture(); require(d.adopt(p, {}, {}).wasOk(), "Adopt"); TimelineEditController ui(d);
        const auto second = p.tracks[0].clips.items()[1].clipId; ui.clickClip(second);
        const auto expected = ClipEdits::reorder(p, ui.targets(), ClipEdits::Placement::before, {p.tracks[0].clips.items()[0].clipId});
        const auto preview = ui.preview(TimelineAction::earlier); require(preview.status.wasOk() && hash(d.getProject()) == hash(p), "Preview mutated");
        require(ui.execute(TimelineAction::earlier).wasOk() && hash(d.getProject()) == hash(expected.project), "Neighbour model mismatch");
        for (const auto& t : d.getProject().tracks) require(t.clips.items()[0].timelineStartSample == 6*S && t.clips.items()[1].timelineStartSample == 0, "Corresponding lane not moved");
        require(d.getHistory().undoDepth() == 1 && ui.execute(TimelineAction::undo).wasOk() && hash(d.getProject()) == hash(p), "Reorder undo");
    });
    suite.test("asymmetric neighbour is rejected before commit", []
    {
        RecorderDocument d; auto p = makeTimelineUiFixture(); auto& c = p.tracks[3].clips.edit()[1]; c.sourceIn = 1; --c.lengthSamples; ++c.timelineStartSample;
        require(d.adopt(p, {}, {}).wasOk(), "Adopt"); TimelineEditController ui(d); ui.clickClip(p.tracks[0].clips.items()[0].clipId);
        require(ui.preview(TimelineAction::later).status.failed() && ui.execute(TimelineAction::later).failed(), "Asymmetric reorder accepted");
        require(d.getHistory().undoDepth() == 0 && hash(d.getProject()) == hash(p), "Rejected reorder mutated");
    });
    suite.test("drag preview collision and source handles reject atomically", []
    {
        RecorderDocument d; const auto p = makeTimelineUiFixture(); require(d.adopt(p, {}, {}).wasOk(), "Adopt"); TimelineEditController ui(d);
        ui.clickClip(p.tracks[3].clips.items()[0].clipId); ui.execute(TimelineAction::unlink); const auto base = hash(d.getProject()); const auto depth = d.getHistory().undoDepth();
        require(ui.beginDrag(TimelineAction::move), "Begin move"); require(ui.dragTo(5*S, true)->status.failed(), "Overlap did not turn invalid");
        require(hash(d.getProject()) == base && ui.commitDrag().failed() && d.getHistory().undoDepth() == depth, "Invalid drop committed");
        ui.beginDrag(TimelineAction::trimIn); require(ui.dragTo(-1, true)->status.failed() && ui.commitDrag().failed(), "Left source limit accepted");
        ui.beginDrag(TimelineAction::trimOut); require(ui.dragTo(10*S+1, true)->status.failed() && ui.commitDrag().failed(), "Right source limit accepted");
        ui.beginDrag(TimelineAction::trimIn); for (int i = 1; i <= 20; ++i) require(ui.dragTo(i, true)->status.wasOk(), "Trim preview");
        require(hash(d.getProject()) == base && ui.commitDrag().wasOk() && d.getHistory().undoDepth() == depth+1, "Gesture not one commit");
        const auto* edited = d.getProject().findClip(p.tracks[3].clips.items()[0].clipId);
        require(edited->sourceIn == 20 && edited->timelineStartSample == 20 && edited->timelineEnd() == 10*S, "Front trim changed absolute source mapping");
        require(ui.historyText(false) == juce::String::fromUTF8("실행취소: 마이크 2 트림"), "Undo name mismatch");
        ui.execute(TimelineAction::undo); require(hash(d.getProject()) == base, "Trim undo");
    });
    suite.test("video drag snaps; Alt and numeric trim preserve exact sample", []
    {
        RecorderDocument d; const auto p = makeTimelineUiFixture(); require(d.adopt(p, {}, {}).wasOk(), "Adopt"); TimelineEditController ui(d);
        const auto id = p.tracks[0].clips.items()[1].clipId; ui.clickClip(id); ui.beginDrag(TimelineAction::move);
        const auto* snapped = ui.dragTo(12*S+801, false); require(snapped->status.wasOk() && snapped->project.findClip(id)->timelineStartSample == 12*S+1600, "Video snap lost"); ui.cancelDrag();
        ui.beginDrag(TimelineAction::move); require(ui.dragTo(12*S+1, true)->status.wasOk() && ui.commitDrag().wasOk(), "Alt move");
        for (const auto& t : d.getProject().tracks) require(t.clips.items()[1].timelineStartSample == 12*S+1, "Linked exact delta differed");
        require(ui.execute(TimelineAction::trimIn, 12*S+2, true, "numeric:start").wasOk(), "Numeric trim");
        require(d.getProject().findClip(id)->timelineStartSample == 12*S+2 && d.getProject().findClip(id)->sourceIn == 1, "Numeric sample rounded");
        Sample value = 0; require(TimelineEditController::parseSample("9223372036854775807", value) && value == (std::numeric_limits<Sample>::max)(), "64-bit parse");
        for (const auto* bad : {"", "1.5", "-1", "9223372036854775808", "1x"}) require(!TimelineEditController::parseSample(bad, value), "Invalid numeric accepted");
    });
    suite.test("stale drag and cancelled drag cannot publish", []
    {
        RecorderDocument d; auto p = makeTimelineUiFixture(); require(d.adopt(p, {}, {}).wasOk(), "Adopt"); TimelineEditController ui(d); ui.clickClip(p.tracks[0].clips.items()[1].clipId);
        ui.beginDrag(TimelineAction::move); ui.dragTo(14*S, true); ui.addMarker(); const auto base = hash(d.getProject());
        require(ui.commitDrag().failed() && hash(d.getProject()) == base, "Stale drag published");
        ui.beginDrag(TimelineAction::move); ui.dragTo(14*S, true); ui.cancelDrag(); require(ui.commitDrag().wasOk() && hash(d.getProject()) == base, "Escape published");
    });
    suite.test("independent-audio-cuts scenario verifies every other track after each UI operation", []
    {
        RecorderDocument d; TimelineEditController ui(d); IndependentAudioCutsScenario scenario(ui); while (!scenario.advance()) {}
        const auto report = scenario.report(); require(report["status"].toString() == "PASS", report["reason"].toString().toRawUTF8());
        require(report["steps"].size() == 10 && d.getHistory().undoDepth() == 7, "Scenario incomplete");
    });
    return suite.result("ReorderTests");
}
