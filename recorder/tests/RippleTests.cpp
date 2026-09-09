#include "TestSupport.h"
#include "ui/TimelineView.automation.h"
#include <algorithm>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
constexpr Sample S = 48000;
juce::String hash(const RecorderProject& p) { return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)); }
void adopt(RecorderDocument& d, const RecorderProject& p) { require(d.adopt(p, {}, {}).wasOk(), "Fixture adoption failed"); }
}
int runRippleTests()
{
    Suite suite;
    suite.test("UI click shift ctrl expand links without saved history", []
    {
        RecorderDocument d; auto p = makeTimelineUiFixture(); adopt(d, p); TimelineEditController ui(d);
        const auto a = p.tracks[3].clips.items()[0].clipId, b = p.tracks[3].clips.items()[1].clipId;
        ui.clickClip(a); require(ui.targets().size() == 4 && ui.explicitSelection() == std::vector<Id>{a}, "Explicit/linked selection confused");
        ui.clickClip(b, false, true); require(ui.targets().size() == 8 && ui.explicitSelection().size() == 2, "Ctrl add failed");
        ui.clickClip(b, false, true); require(ui.targets().size() == 4, "Ctrl toggle failed");
        ui.clickClip(a); ui.clickClip(b, true); require(ui.targets().size() == 8, "Shift chronological range failed");
        ui.seek(123); ui.setRange(19, 71); ui.clearSelection(); require(d.getHistory().undoDepth() == 0 && !d.isDirty() && hash(d.getProject()) == hash(p), "Selection/seek created persistence work");
    });
    suite.test("UI split targets match model and unlink detaches explicit microphone only", []
    {
        RecorderDocument d; auto p = makeTimelineUiFixture(); adopt(d, p); TimelineEditController ui(d);
        const auto mic = p.tracks[3].clips.items()[0].clipId; ui.clickClip(mic); ui.followPlayhead(12017);
        const auto expected = ClipEdits::split(p, ui.targets(), 12017); require(ui.execute(TimelineAction::split).wasOk(), "Linked split failed");
        require(hash(d.getProject()) == hash(expected.project), "UI split diverged from model target set");
        require(ui.execute(TimelineAction::undo).wasOk(), "Undo failed"); ui.clickClip(mic);
        require(ui.execute(TimelineAction::unlink).wasOk(), "Unlink failed");
        require(d.getProject().findClip(mic)->linkGroupId.isEmpty() && d.getProject().linkGroups[0].clipIds.size() == 3, "Unlink silently detached others");
        ui.followPlayhead(12017); require(ui.execute(TimelineAction::split).wasOk(), "Independent split failed");
        require(d.getProject().tracks[0].clips.items().size() == 2 && d.getProject().tracks[3].clips.items()[0].timelineEnd() == 12017, "Audio split touched camera or snapped");
    });
    suite.test("editing a linked group retains directly clicked unlink intent", []
    {
        RecorderDocument d; const auto p = makeTimelineUiFixture(); adopt(d, p); TimelineEditController ui(d);
        const auto mic = p.tracks[3].clips.items()[0].clipId; ui.clickClip(mic);
        require(ui.execute(TimelineAction::trimIn, S, true).wasOk(), "Linked trim");
        require(ui.targets().size() == 4 && ui.explicitSelection() == std::vector<Id>{mic}, "Model output became explicit clicks");
        require(ui.execute(TimelineAction::unlink).wasOk() && d.getProject().linkGroups[0].clipIds.size() == 3, "Post-trim unlink detached the whole group");
    });
    suite.test("selected interval delete preserves empty time and uses selected links", []
    {
        RecorderDocument d; const auto p = makeTimelineUiFixture(); adopt(d, p); TimelineEditController ui(d);
        ui.clickClip(p.tracks[0].clips.items()[0].clipId); ui.setRange(2*S, 3*S);
        auto expected = ClipEdits::remove(p, ui.targets(), {2*S, S});
        require(ui.execute(TimelineAction::remove).wasOk() && hash(d.getProject()) == hash(expected.project), "Selected range delete mismatch");
        require(d.getProject().activeTimelineEnd() == 18*S, "Plain delete pulled timeline");
        ui.execute(TimelineAction::undo); ui.clearSelection(); ui.setRange(2*S, 3*S); expected = ClipEdits::remove(p, SampleRange{2*S, S});
        require(ui.execute(TimelineAction::remove).wasOk() && hash(d.getProject()) == hash(expected.project), "Unselected range delete mismatch");
    });
    suite.test("external camera link prompts then cancel or explicit global expansion", []
    {
        RecorderDocument d; const auto p = makeTimelineUiFixture(); adopt(d, p); TimelineEditController ui(d);
        ui.selectTrack(p.tracks[3].trackId, false); ui.setRange(2*S, 3*S); const auto prompt = ui.ripplePrompt();
        require(prompt.conflict && prompt.requiresAllTracks && prompt.expandedTracks.size() == 4, "Missing camera link prompt");
        require(ui.execute(TimelineAction::rippleAudio).failed() && hash(d.getProject()) == hash(p), "Direct path silently broke link");
        require(ui.resolveRipple(prompt, RippleChoice::cancel).wasOk() && d.getHistory().undoDepth() == 0, "Cancel mutated");
        const auto expected = ClipEdits::rippleDeleteAll(p, {2*S, S});
        require(ui.resolveRipple(prompt, RippleChoice::expand).wasOk() && hash(d.getProject()) == hash(expected.project), "Global expansion mismatch");
        require(d.getHistory().undoDepth() == 1, "Expansion not one transaction");
    });
    suite.test("explicit unlink plus local ripple is one undo and leaves global markers", []
    {
        RecorderDocument d; auto p = makeTimelineUiFixture(); Marker m; m.sample = 4*S; p.markers.push_back(m); adopt(d, p); TimelineEditController ui(d);
        ui.selectTrack(p.tracks[3].trackId, false); ui.setRange(2*S, 3*S); const auto prompt = ui.ripplePrompt();
        auto expected = ClipEdits::unlink(p, prompt.detachClips); expected = ClipEdits::rippleDeleteTracks(expected.project, {2*S,S}, prompt.tracks);
        require(ui.resolveRipple(prompt, RippleChoice::unlink).wasOk() && hash(d.getProject()) == hash(expected.project), "Unlink prompt mapping mismatch");
        require(d.getProject().markers[0].sample == 4*S && d.getHistory().undoDepth() == 1, "Local ripple touched marker or split history");
        require(ui.execute(TimelineAction::undo).wasOk() && hash(d.getProject()) == hash(p), "Unlink and ripple did not restore together");
    });
    suite.test("all-audio links expand only audio lanes and stale dialogs refuse", []
    {
        RecorderDocument d; auto p = makeTimelineUiFixture(); std::vector<Id> cameras;
        for (unsigned i = 0; i < 2; ++i) for (const auto& c : p.tracks[i].clips.items()) cameras.push_back(c.clipId);
        p = ClipEdits::unlink(p, cameras).project; adopt(d, p); TimelineEditController ui(d);
        ui.selectTrack(p.tracks[3].trackId, false); ui.setRange(2*S, 3*S); auto prompt = ui.ripplePrompt();
        require(prompt.conflict && !prompt.requiresAllTracks && prompt.expandedTracks.size() == 2, "Audio-only expansion wrong");
        const auto expected = ClipEdits::rippleDeleteTracks(p, {2*S,S}, prompt.expandedTracks);
        require(ui.resolveRipple(prompt, RippleChoice::expand).wasOk() && hash(d.getProject()) == hash(expected.project), "Audio expansion mismatch");
        ui.execute(TimelineAction::undo); ui.setRange(2*S, 3*S); prompt = ui.ripplePrompt(); ui.addMarker(); const auto after = hash(d.getProject());
        require(ui.resolveRipple(prompt, RippleChoice::unlink).failed() && hash(d.getProject()) == after, "Stale dialog applied to changed project");
    });
    return suite.result("RippleTests");
}
