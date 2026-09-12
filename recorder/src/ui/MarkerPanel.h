#pragma once
#include "RecorderLookAndFeel.h"
#include "TimelineView.logic.h"

namespace gocue::recorder
{
class MarkerPanel : public juce::Component, private juce::ListBoxModel
{
public:
    explicit MarkerPanel(TimelineEditController&);
    void refresh();
    void resized() override;
    std::function<void(juce::Result)> onEdit;
    std::function<void()> onAddRequested; // set by the timeline view: routes "마커 추가" to the host's name dialog
private:
    int getNumRows() override { return int(markers.size()); }
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override;
    void selectedRowsChanged(int) override;
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override;
    void apply();
    TimelineEditController& edits;
    std::vector<Marker> markers;
    Id selected;
    juce::Viewport viewport;
    juce::Component content;
    juce::ListBox list;
    juce::TextEditor name, position, colour;
    juce::TextButton add {ko("마커 추가")}, change {ko("이름 변경")}, remove {ko("삭제")};
};
}
