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
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    std::function<void(juce::Result)> onEdit;
private:
    friend struct TimelineUxTestAccess;
    juce::PopupMenu createContextMenu() const;
    void handleContextMenuResult(int result, const RecorderDocument::Snapshot& base);
    TimelineEditController& edits;
    Id id;
    juce::Label name;
    juce::TextButton mute {ko("음소거")}, solo {ko("솔로")};
};
}
