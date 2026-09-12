#pragma once
#include "RecorderLookAndFeel.h"
#include "TimelineView.logic.h"

namespace gocue::recorder
{
class TrackHeader : public juce::Component
{
public:
    TrackHeader(TimelineEditController&, Id);
    void refresh(const Track&);
    void resized() override;
    std::function<void(juce::Result)> onEdit;
private:
    TimelineEditController& edits;
    Id id;
    juce::Label name;
    juce::TextButton mute {ko("음소거")}, solo {ko("솔로")};
};
}
