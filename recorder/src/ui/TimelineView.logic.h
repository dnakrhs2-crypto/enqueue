#pragma once
#include "app/RecorderDocument.h"

namespace gocue::recorder
{
// Shared by mouse/keyboard/widgets and the headless probe. No JUCE GUI or devices.
enum class TimelineAction { split, trimIn, trimOut, remove, rippleAll, rippleAudio,
    move, earlier, later, unlink, link, undo, redo, addMarker, editMarker, deleteMarker, mute, solo, seek, hideTrack, showTrack };
enum class RippleChoice { cancel, expand, unlink };
struct RipplePrompt
{
    RecorderDocument::Snapshot base;
    SampleRange range;
    std::vector<Id> tracks, expandedTracks, detachClips;
    bool conflict = false, requiresAllTracks = false;
};
class TimelineEditController
{
public:
    explicit TimelineEditController(RecorderDocument& d) : document(d) {}
    RecorderDocument& document;
    void setLocked(bool value) { locked = value; if (isLocked()) cancelDrag(); }
    bool isLocked() const { return locked || document.isRecordingStructureLocked(); }
    bool enabled(TimelineAction) const;
    static juce::String text(TimelineAction);
    juce::String historyText(bool redo) const;
    juce::String editName(TimelineAction) const;
    void clickClip(const Id&, bool shift = false, bool control = false);
    void focusSelection(const Id& id) { if (document.getProject().findClip(id)) focus = id; }
    void clearSelection();
    void reconcileSelection();
    const Id& focusedClip() const { return focus; }
    const std::vector<Id>& explicitSelection() const { return explicitClips; }
    std::vector<Id> targets() const;
    void selectTrack(const Id&, bool additive);
    const std::vector<Id>& selectedTracks() const { return trackIds; }
    void setRange(Sample a, Sample b);
    void clearRange() { range.reset(); }
    std::optional<SampleRange> selectedRange() const { return range; }
    Sample playhead() const { return cursor; }
    void followPlayhead(Sample at) { cursor = at; }
    juce::Result seek(Sample);
    std::function<void(Sample, bool)> onSeek;
    juce::Result execute(TimelineAction, Sample value = 0, bool exact = false, const juce::String& mergeKey = {});
    ClipEditResult preview(TimelineAction, Sample value = 0, bool exact = false) const;
    RipplePrompt ripplePrompt() const;
    juce::Result resolveRipple(const RipplePrompt&, RippleChoice);
    juce::Result setTrackListening(const Id&, bool solo);
    juce::Result setTrackHidden(const Id&, bool hidden);
    juce::Result setTracksHidden(const std::vector<Id>&, bool hidden); // one journaled edit (one undo step) for several tracks
    juce::Result addMarker(const juce::String& name = {}, const juce::String& colour = "#4c8dff");
    juce::Result editMarker(const Id&, Sample, const juce::String& name, const juce::String& colour);
    juce::Result deleteMarker(const Id&);
    bool beginDrag(TimelineAction);
    const ClipEditResult* dragTo(Sample absoluteEdgeOrStart, bool exact);
    std::optional<Sample> dragNeighbourGuide() const;
    juce::Result commitDrag();
    void cancelDrag();
    const ClipEditResult* dragPreview() const { return dragResult.get(); }
    TimelineAction dragAction() const { return dragKind; }
    Sample dragValue() const { return dragAt; }
    const std::vector<Id>& dragTargets() const { return dragIds; }
    static bool parseSample(const juce::String&, Sample&);
    static bool parseTimecode(const juce::String&, unsigned Fs, Sample&);
    static std::vector<Id> expandLinks(const RecorderProject&, const std::vector<Id>&);
private:
    ClipEditResult apply(const RecorderProject&, TimelineAction, const std::vector<Id>&, Sample, bool) const;
    std::vector<Id> neighbour(const RecorderProject&, const std::vector<Id>&, bool later) const;
    bool locked = false;
    Id focus, anchor, primaryTrack;
    std::vector<Id> explicitClips, publishedSelection, trackIds;
    Sample cursor = 0;
    std::optional<SampleRange> range;
    RecorderDocument::Snapshot dragBase;
    std::vector<Id> dragIds, dragSelection;
    std::unique_ptr<ClipEditResult> dragResult;
    TimelineAction dragKind = TimelineAction::move;
    Sample dragAt = 0;
    bool dragExact = false;
};
}
