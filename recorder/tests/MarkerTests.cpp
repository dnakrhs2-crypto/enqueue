#include "TestSupport.h"
#include "ui/TimelineView.automation.h"
#include "model/MarkerExport.h"
#include <limits>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
constexpr Sample S = 48000;
juce::String hash(const RecorderProject& p) { return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)); }
}
int runMarkerTests()
{
    Suite suite;
    suite.test("marker export empty and shuffled subsecond chapters use CRLF without mutating the project", []
    {
        RecorderProject p; p.Fs = 48000;
        require(markerExportText(p).isEmpty(), "Empty project exported text");
        Marker late; late.sample = 3910 * S + S - 1; late.name = juce::String::fromUTF8("마무리");
        Marker first; first.sample = 2 * S + 1000; first.name = "Zulu";
        Marker second; second.sample = 2 * S + 36000; second.name = "Alpha";
        p.markers = {late, second, first}; const auto before = RecorderSerializer::toJson(p);
        require(markerExportText(p) == juce::String::fromUTF8("00:02 Zulu\r\n00:02 Alpha\r\n1:05:10 마무리\r\n"), "Chapter order, flooring, hours or CRLF changed");
        require(RecorderSerializer::toJson(p) == before, "Export reordered or edited the source project");
    });
    suite.test("marker export ties sort by name and embedded newlines become spaces", []
    {
        RecorderProject p; p.Fs = 48000;
        Marker b; b.sample = 42 * S; b.name = juce::String::fromUTF8("나\r줄\n끝\v다음\f장\u0085NEL\u2028LS\u2029PS");
        Marker a; a.sample = b.sample; a.name = juce::String::fromUTF8("가\r\n줄");
        Marker spaces; spaces.sample = 0; spaces.name = "  unchanged  "; p.markers = {b, a, spaces};
        require(markerExportText(p) == juce::String::fromUTF8("00:00   unchanged  \r\n00:42 가 줄\r\n00:42 나 줄 끝 다음 장 NEL LS PS\r\n"), "Tie order, whitespace preservation or newline sanitising changed");
    });
    suite.test("marker timecode accepts minutes hours and fractional seconds at the project rate", []
    {
        Sample at = -1, samples = -1;
        require(TimelineEditController::parseTimecode("00:01.500", 60000, at) && at == 90000
            && TimelineEditController::parseSample("90000", samples) && samples == at, "Time and sample positions disagree");
        require(TimelineEditController::parseTimecode(" 00:42 ", 48000, at) && at == 42 * S, "Minute timecode failed");
        require(TimelineEditController::parseTimecode("1:05:10.123", 48000, at) && at == 3910 * S + 5904, "Hour timecode failed");
        require(TimelineEditController::parseTimecode("1:05:10", 48000, at) && at == 3910 * S, "Chapter timecode failed");
        require(TimelineEditController::parseTimecode("1:05", 48000, at) && at == 65 * S, "Single-digit minutes must be accepted");
        require(TimelineEditController::parseTimecode("1:5", 48000, at) && at == 65 * S, "Single-digit seconds must be accepted");
        require(TimelineEditController::parseTimecode("1:5:7.25", 48000, at) && at == 3907 * S + 12000, "Single-digit hour fields must be accepted");
        require(TimelineEditController::parseTimecode("00:00.005", 44100, at) && at == 221, "Fraction must round to the nearest sample");
        require(TimelineEditController::parseTimecode("00:01.5", 60000, at) && at == 90000, "Short fraction must be decimal seconds");
        require(TimelineEditController::parseTimecode("2562047788015215:30:07", 1, at) && at == (std::numeric_limits<Sample>::max)(), "Largest representable sample rejected");
    });
    suite.test("marker timecode rejects malformed out of range and overflowing input without changing the output", []
    {
        for (const auto* text : {"", "90000", "-00:01", "00:60", "60:00", "1:60:00", "000:01", "00:001", "00:01.", "00:01.1234", "00:01.2x", "00:01.2.3",
                                 "1::02", ":01", "00:01:", "00: 01", "00:01 extra", "9223372036854775807:00:00", "2562047788015215:30:08"})
        {
            Sample at = 123;
            require(!TimelineEditController::parseTimecode(text, 48000, at) && at == 123, text);
        }
        Sample at = 123;
        require(!TimelineEditController::parseTimecode("00:01", 0, at) && at == 123, "Zero rate accepted");
        require(!TimelineEditController::parseTimecode("2562047788015215:30:07.999", 1, at) && at == 123, "Fraction overflow accepted");
    });
    suite.test("marker add rename colour position delete undo and jump mapping", []
    {
        RecorderDocument d; const auto p = makeTimelineUiFixture(); require(d.adopt(p, {}, {}).wasOk(), "Adopt"); TimelineEditController ui(d); Sample sought = -1;
        ui.onSeek = [&](Sample at, bool released) { require(released, "Marker jump should release seek"); sought = at; };
        ui.followPlayhead(123); require(ui.addMarker().wasOk(), "Add marker"); const auto id = d.getProject().markers.front().markerId;
        require(ui.editMarker(id, 456, juce::String::fromUTF8("다시 시작"), "#ff0077").wasOk(), "Edit marker");
        require(d.getProject().markers[0].name == juce::String::fromUTF8("다시 시작") && d.getProject().markers[0].colour == "#ff0077", "Marker fields missing");
        const auto depth = d.getHistory().undoDepth(); require(ui.seek(d.getProject().markers[0].sample).wasOk() && sought == 456 && d.getHistory().undoDepth() == depth, "Jump saved history");
        require(ui.editMarker(id, -1, "bad", "#ff0077").failed() && ui.editMarker(id, 456, "bad", "#xyzxyz").failed(), "Invalid marker accepted");
        require(ui.deleteMarker(id).wasOk() && d.getProject().markers.empty(), "Delete marker");
        ui.execute(TimelineAction::undo); require(d.getProject().markers[0].sample == 456, "Delete undo");
        ui.execute(TimelineAction::undo); require(d.getProject().markers[0].sample == 123, "Rename undo");
        ui.execute(TimelineAction::undo); require(hash(d.getProject()) == hash(p), "Add undo");
        ui.execute(TimelineAction::redo); require(d.getProject().markers[0].markerId == id, "Redo marker identity");
    });
    suite.test("global ripple transforms marker half-open boundaries and all take versions", []
    {
        RecorderDocument d; auto p = makeTimelineUiFixture();
        TakeStack stack; stack.spanSamples = 10*S; TakeVersion active, previous; LinkGroup old;
        for (auto& t : p.tracks)
        {
            auto& c = t.clips.edit()[0]; c.takeStackId = stack.stackId; c.versionId = active.versionId; active.clipIds.push_back(c.clipId);
            auto prior = c; prior.clipId = newId(); prior.versionId = previous.versionId; prior.linkGroupId = old.linkGroupId;
            previous.clipIds.push_back(prior.clipId); old.clipIds.push_back(prior.clipId); t.clips.edit().push_back(prior);
        }
        stack.versions = {active, previous}; stack.activeVersionId = active.versionId; p.takeStacks.push_back(stack); p.linkGroups.push_back(old);
        for (const auto at : {2*S-1, 2*S, 3*S, 3*S+1}) { Marker m; m.sample = at; p.markers.push_back(m); }
        require(d.adopt(p, {}, {}).wasOk(), "Version fixture adoption"); TimelineEditController ui(d); ui.setRange(2*S, 3*S);
        const auto expected = ClipEdits::rippleDeleteAll(p, {2*S, S});
        require(ui.execute(TimelineAction::rippleAll).wasOk() && hash(d.getProject()) == hash(expected.project), "Global UI/model mismatch");
        const auto& after = d.getProject(); require(after.markers.size() == 3 && after.markers[0].sample == 2*S-1 && after.markers[1].sample == 2*S && after.markers[2].sample == 2*S+1, "Marker boundary transform");
        require(after.takeStacks[0].spanSamples == 9*S && after.takeStacks[0].versions[1].clipIds.size() == 8, "Inactive versions not cut");
        ui.execute(TimelineAction::undo); require(hash(d.getProject()) == hash(p), "Global undo did not restore markers/versions");
    });
    suite.test("recording lock maps all structural controls but permits marker append", []
    {
        RecorderDocument d; const auto p = makeTimelineUiFixture(); require(d.adopt(p, {}, {}).wasOk(), "Adopt"); TimelineEditController ui(d);
        ui.clickClip(p.tracks[3].clips.items()[0].clipId); ui.setRange(S, 2*S); d.setRecordingStructureLock(true);
        for (auto a : {TimelineAction::split, TimelineAction::trimIn, TimelineAction::trimOut, TimelineAction::remove, TimelineAction::move,
                       TimelineAction::earlier, TimelineAction::later, TimelineAction::rippleAll, TimelineAction::rippleAudio, TimelineAction::unlink, TimelineAction::link,
                       TimelineAction::undo, TimelineAction::redo, TimelineAction::editMarker, TimelineAction::deleteMarker, TimelineAction::mute, TimelineAction::solo, TimelineAction::seek})
            require(!ui.enabled(a), "A structural control stayed enabled");
        require(ui.enabled(TimelineAction::addMarker) && !ui.beginDrag(TimelineAction::move) && ui.seek(42).failed(), "Recording action mapping");
        require(ui.execute(TimelineAction::split).failed() && ui.setTrackListening(p.tracks[3].trackId, false).failed(), "Recording command bypass");
        require(d.performEdit("direct-pure", {}, [&](const RecorderProject& before) { return ClipEdits::split(before, {p.tracks[0].clips.items()[0].clipId}, S); }).failed(), "Pure adapter bypasses recording gate");
        require(d.performEdit("direct-state", [](EditState& e) { e.tracks[3].mute = true; }).failed(), "State adapter bypasses recording gate");
        ui.followPlayhead(91); require(ui.addMarker().wasOk() && d.getProject().markers[0].sample == 91, "Recording marker append blocked");
        const auto id = d.getProject().markers[0].markerId; require(ui.editMarker(id, 100, "x", "#ffffff").failed() && ui.deleteMarker(id).failed(), "Existing marker edit allowed during recording");
        require(d.performEdit("mixed-marker-structure", [](EditState& e) { e.markers.emplace_back(); e.tracks[0].clips.edit().clear(); }).failed(), "Marker exception permits structural edits");
        d.setRecordingStructureLock(false); require(ui.execute(TimelineAction::undo).wasOk() && hash(d.getProject()) == hash(p), "Recorded marker undo failed after stop");
        ui.setLocked(true); require(!ui.enabled(TimelineAction::split) && ui.enabled(TimelineAction::addMarker), "UI preparing/live lock mapping");
    });
    return suite.result("MarkerTests");
}
