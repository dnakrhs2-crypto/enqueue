#pragma once
#include "ClipEdits.h"

namespace gocue::recorder
{
struct TakeVersionImpact
{
    juce::Result status = juce::Result::ok();
    SampleRange range;
    std::vector<Id> removed, restored;
};

// Pure metadata operations. Media must already be registered in the input copy.
// Versions are newest first; an absent clip in the active version is an explicit
// empty lane/tail, never a request to fall through to an older version.
class TakeStackEdits
{
public:
    static ClipEditResult addTake(const RecorderProject&, const Id& takeId,
                                  SampleRange recordingRange, const std::vector<int>& microphoneLanes,
                                  const Id& retakeStack = {});
    static TakeVersionImpact impact(const RecorderProject&, const Id& stackId, const Id& versionId);
    static ClipEditResult useVersion(const RecorderProject&, const Id& stackId, const Id& versionId);
    static const TakeStack* find(const RecorderProject&, const Id& stackId);
    // Split/ripple reuse round 13. Trim targets the absolute stack edge, including
    // when the active version has a blank tail or its clips were already split.
    static ClipEditResult split(const RecorderProject&, const Id& stackId, Sample);
    static ClipEditResult trimIn(const RecorderProject&, const Id& stackId, Sample);
    static ClipEditResult trimOut(const RecorderProject&, const Id& stackId, Sample);
    static ClipEditResult rippleDeleteAll(const RecorderProject&, SampleRange);
};
}
