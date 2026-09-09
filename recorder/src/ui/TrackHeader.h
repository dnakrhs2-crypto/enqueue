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
    void mouseDown(const juce::MouseEvent&) override;
    std::function<void(juce::Result)> onEdit;
    std::function<void()> onSelection;
private:
    TimelineEditController& edits;
    Id id;
    juce::Label name;
    juce::TextButton target {ko("대상")}, mute {ko("음소거")}, solo {ko("솔로")};
};
}
