#pragma once
#include "TimelineView.logic.h"

namespace gocue::recorder
{
RecorderProject makeTimelineUiFixture();
// Each step uses exactly the controller entry points used by the TimelineView widgets.
class IndependentAudioCutsScenario
{
public:
    explicit IndependentAudioCutsScenario(TimelineEditController&);
    bool advance(); // true when finished (including a failed assertion)
    juce::var report() const;
    std::function<juce::Result(TimelineAction, Sample, bool)> dispatch;
private:
    void verifyOtherTracks() const;
    TimelineEditController& edits;
    RecorderProject original;
    Id micFirst, micSecond, survivor;
    int step = 0;
    juce::Array<juce::var> steps;
    juce::String error, reorderedHash;
};
}
