#include "TimelineView.automation.h"
#include <stdexcept>

namespace gocue::recorder
{
namespace
{
Id fixtureId(unsigned n) { return juce::String(n).paddedLeft('0', 32); }
void need(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
juce::String hash(const RecorderProject& p) { return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)); }
constexpr Sample S = 48000;
}
RecorderProject makeTimelineUiFixture()
{
    RecorderProject p; p.projectId = fixtureId(1); p.name = juce::String::fromUTF8("독립 오디오 컷 검증");
    auto registry = std::make_shared<MediaRegistry>(); p.media = registry;
    const TrackKind kinds[] {TrackKind::cam1, TrackKind::cam2, TrackKind::mic, TrackKind::mic};
    const char* names[] {"캠1", "캠2", "마이크 1", "마이크 2"};
    for (unsigned lane = 0; lane < 4; ++lane)
    {
        Track track; track.trackId = fixtureId(10 + lane); track.kind = kinds[lane]; track.name = juce::String::fromUTF8(names[lane]); track.microphoneIndex = lane < 2 ? -1 : int(lane - 2);
        for (unsigned take = 0; take < 2; ++take)
        {
            MediaAsset asset; asset.assetId = fixtureId(100 + take * 10 + lane); asset.kind = lane < 2 ? AssetKind::camera : AssetKind::mic;
            asset.logicalLength = (take == 0 ? 10 : 6) * S; asset.availableRanges = {{0, asset.logicalLength}}; asset.mediaGeneration = 1;
            asset.relativePath = "media/takes/fixture/" + asset.assetId + (lane < 2 ? ".mp4" : ".wav"); asset.contentIdentity = "synthetic-metadata-" + asset.assetId;
            asset.originalFormat.codec = lane < 2 ? "h264" : "pcm_s24le"; asset.originalFormat.sampleRate = 48000;
            asset.originalFormat.channels = lane < 2 ? 0 : 1; asset.originalFormat.bitsPerSample = lane < 2 ? 0 : 24;
            asset.originalFormat.width = lane < 2 ? 1920 : 0; asset.originalFormat.height = lane < 2 ? 1080 : 0;
            if (lane < 2) { asset.sourceUnitsNumerator = 30; asset.sourceUnitsDenominator = S; }
            registry->assets.push_back(asset);
            Clip c; c.clipId = fixtureId(200 + take * 10 + lane); c.assetId = asset.assetId; c.trackId = track.trackId;
            c.timelineStartSample = take == 0 ? 0 : 12 * S; c.lengthSamples = asset.logicalLength; c.linkGroupId = fixtureId(300 + take);
            track.clips.edit().push_back(c);
        }
        p.tracks.push_back(track);
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        LinkGroup group; group.linkGroupId = fixtureId(300 + i);
        Take take; take.takeId = fixtureId(400 + i); take.number = int(i + 1); take.name = juce::String::fromUTF8("테이크 ") + juce::String(i + 1);
        take.state = TakeState::complete; take.logicalLength = (i == 0 ? 10 : 6) * S; take.placementSample = i == 0 ? 0 : 12 * S;
        take.createdAt = "2026-09-09T00:00:00Z"; take.capture.physicalInputs = {0, 1};
        take.cam1AssetId = p.tracks[0].clips.items()[i].assetId; take.cam2AssetId = p.tracks[1].clips.items()[i].assetId;
        for (unsigned lane = 0; lane < 4; ++lane) { group.clipIds.push_back(p.tracks[lane].clips.items()[i].clipId); if (lane >= 2) take.microphoneAssetIds.push_back(p.tracks[lane].clips.items()[i].assetId); }
        p.linkGroups.push_back(group); registry->takes.push_back(take);
    }
    need(p.validate().wasOk(), p.validate().getErrorMessage().toRawUTF8()); return p;
}
IndependentAudioCutsScenario::IndependentAudioCutsScenario(TimelineEditController& c) : edits(c), original(makeTimelineUiFixture())
{
    const auto r = edits.document.adopt(original, {}, {}); need(r.wasOk(), r.getErrorMessage().toRawUTF8());
    micFirst = original.tracks[3].clips.items()[0].clipId; micSecond = original.tracks[3].clips.items()[1].clipId;
    dispatch = [this](TimelineAction a, Sample at, bool exact) { return edits.execute(a, at, exact); };
}
void IndependentAudioCutsScenario::verifyOtherTracks() const
{
    const auto& p = edits.document.getProject(); need(p.validate().wasOk(), "Invalid project after UI command");
    const auto a = RecorderSerializer::editStateToVar(original), b = RecorderSerializer::editStateToVar(p);
    for (int i = 0; i < 3; ++i) need(juce::JSON::toString(a["tracks"][i], true) == juce::JSON::toString(b["tracks"][i], true), "Another track changed");
    need(p.media == original.media, "Media registry changed");
}
bool IndependentAudioCutsScenario::advance()
{
    if (step >= 10 || error.isNotEmpty()) return true;
    auto* row = new juce::DynamicObject(); juce::var entry(row); row->setProperty("step", step);
    const char* names[] {"selection", "unlink-mic2", "trim-in", "trim-out", "split", "delete", "move", "reorder-later", "undo", "redo"}; row->setProperty("action", names[step]);
    try
    {
        const auto run = [&](TimelineAction a, Sample at = 0, bool exact = false) { const auto r = dispatch(a, at, exact); need(r.wasOk(), r.getErrorMessage().toRawUTF8()); };
        switch (step)
        {
            case 0:
                edits.clickClip(micFirst); edits.clickClip(micSecond, false, true); edits.followPlayhead(5 * S);
                need(edits.targets().size() == 8 && edits.explicitSelection().size() == 2, "Selection/link expansion mismatch");
                need(edits.document.getHistory().undoDepth() == 0 && edits.document.getProject().editRevision == 0, "Selection/seek wrote history"); break;
            case 1: run(TimelineAction::unlink); need(edits.document.getProject().findClip(micFirst)->linkGroupId.isEmpty() && edits.document.getProject().findClip(micSecond)->linkGroupId.isEmpty(), "Mic2 still linked"); break;
            case 2: edits.clickClip(micFirst); run(TimelineAction::trimIn, S, true); break;
            case 3: run(TimelineAction::trimOut, 9 * S, true); break;
            case 4: run(TimelineAction::split); break;
            case 5:
            {
                Id left;
                for (const auto& clip : edits.document.getProject().tracks[3].clips.items())
                { if (clip.timelineStartSample == S) left = clip.clipId; if (clip.timelineStartSample == 5 * S) survivor = clip.clipId; }
                need(left.isNotEmpty() && survivor.isNotEmpty(), "Split did not make expected pieces"); edits.clickClip(left); run(TimelineAction::remove); break;
            }
            case 6:
                edits.clickClip(survivor); need(edits.beginDrag(TimelineAction::move), "Drag not started");
                need(edits.dragTo(2 * S + 1, true)->status.wasOk(), "Move preview rejected");
                need(edits.document.getProject().findClip(survivor)->timelineStartSample == 5 * S, "Preview mutated document");
                { const auto r = edits.commitDrag(); need(r.wasOk(), "Move commit failed"); }
                need(edits.document.getProject().findClip(survivor)->timelineStartSample == 2 * S + 1, "Audio sample precision lost"); break;
            case 7: run(TimelineAction::later); reorderedHash = hash(edits.document.getProject()); break;
            case 8: run(TimelineAction::undo); need(edits.document.getProject().findClip(survivor)->timelineStartSample == 2 * S + 1, "Reorder undo mismatch"); break;
            case 9: run(TimelineAction::redo); need(hash(edits.document.getProject()) == reorderedHash, "Redo mismatch"); break;
        }
        verifyOtherTracks(); row->setProperty("status", "PASS"); row->setProperty("otherTracksUnchanged", true);
    }
    catch (const std::exception& e) { error = juce::String::fromUTF8(e.what()); row->setProperty("status", "FAIL"); row->setProperty("reason", error); }
    juce::Array<juce::var> ids; for (const auto& id : edits.targets()) ids.add(id); row->setProperty("selection", ids);
    row->setProperty("revision", juce::int64(edits.document.getProject().editRevision)); row->setProperty("undoDepth", int(edits.document.getHistory().undoDepth()));
    row->setProperty("stateHash", hash(edits.document.getProject())); steps.add(entry); ++step; return step >= 10 || error.isNotEmpty();
}
juce::var IndependentAudioCutsScenario::report() const
{
    auto* r = new juce::DynamicObject(); r->setProperty("schemaVersion", 1); r->setProperty("scenario", "independent-audio-cuts");
    r->setProperty("status", error.isNotEmpty() ? "FAIL" : step >= 10 ? "PASS" : "INCOMPLETE"); r->setProperty("reason", error);
    r->setProperty("source", "synthetic-metadata"); r->setProperty("steps", steps);
    r->setProperty("verification", "Shared TimelineView command/selection/drag controller; other track metadata and source registry checked after every step.");
    r->setProperty("unverified", "OS mouse/keyboard injection, real media playback, camera/ASIO, DPI and optical UI latency."); return juce::var(r);
}
}
