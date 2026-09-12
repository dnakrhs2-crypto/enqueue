#pragma once
#include "RecorderLookAndFeel.h"
#include "TimelineView.logic.h"

namespace gocue::recorder
{
class ClipInspector : public juce::Component
{
public:
    explicit ClipInspector(TimelineEditController&);
    void refresh();
    void resized() override;
    std::function<void(juce::Result)> onEdit;
private:
    void commit(unsigned field);
    TimelineEditController& edits;
    juce::Viewport viewport;
    juce::Component content;
    juce::Label title, units, link, versions;
    std::array<juce::Label, 5> labels;
    std::array<juce::TextEditor, 5> inputs;
    Id displayedClip;
};
}
