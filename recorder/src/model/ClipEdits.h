#pragma once
#include "RecorderModel.h"
#include <optional>

namespace gocue::recorder
{
struct ClipEditResult
{
    juce::Result status;
    RecorderProject project;
    // Absent for a caller-supplied metadata edit; otherwise the surviving edit targets.
    std::optional<std::vector<Id>> selection;
    ClipEditResult(RecorderProject p) : status(juce::Result::ok()), project(std::move(p)) {}
};

// Value transformations only: no UI, media I/O, revision changes or registry mutations.
// Errors return the original project. IDs are deterministic within the input project's
// namespace; repeating the same operation on the same input produces the same result.
class ClipEdits
{
public:
    enum class Placement { before, after };
    static ClipEditResult split(const RecorderProject&, const std::vector<Id>& clipIds, Sample timelineSample);
    // The requested edge is absolute, relative to clipIds.front(). All linked edges
    // move by the same delta. Requests outside the common handles are rejected, not clamped.
    static ClipEditResult trimIn(const RecorderProject&, const std::vector<Id>& clipIds, Sample timelineSample, bool frameSnap = true);
    static ClipEditResult trimOut(const RecorderProject&, const std::vector<Id>& clipIds, Sample timelineSample, bool frameSnap = true);
    static ClipEditResult remove(const RecorderProject&, const std::vector<Id>& clipIds);
    static ClipEditResult remove(const RecorderProject&, const std::vector<Id>& clipIds, SampleRange);
    static ClipEditResult remove(const RecorderProject&, SampleRange); // all active targets, preserves time
    static ClipEditResult rippleDeleteAll(const RecorderProject&, SampleRange);
    // Audio tracks only; edits active clips, preserves previous versions and global markers.
    static ClipEditResult rippleDeleteTracks(const RecorderProject&, SampleRange, const std::vector<Id>& trackIds);
    // Alt drag / explicit sample input may opt out; the default keeps round-13 snapping.
    static ClipEditResult move(const RecorderProject&, const std::vector<Id>& clipIds, Sample deltaSamples, bool frameSnap = true);
    // Complete, symmetric bundles on the same lanes. Unrelated markers keep their positions.
    static ClipEditResult reorder(const RecorderProject&, const std::vector<Id>& bundle,
                                  Placement, const std::vector<Id>& target);
    // link merges the complete existing groups. unlink detaches just the explicit IDs.
    static ClipEditResult link(const RecorderProject&, const std::vector<Id>& clipIds);
    static ClipEditResult unlink(const RecorderProject&, const std::vector<Id>& clipIds);
};
}
