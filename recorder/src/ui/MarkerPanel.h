#pragma once
#include "RecorderLookAndFeel.h"
#include "TimelineView.logic.h"
#include "MarkerColourSwatches.h"

namespace gocue::recorder
{
class MarkerPanel : public juce::Component, private juce::ListBoxModel
{
public:
    explicit MarkerPanel(TimelineEditController&);
    void refresh();
    void resized() override;
    std::function<void(juce::Result)> onEdit;
    std::function<void(const juce::String&)> onStatusChanged;
    std::function<void()> onAddRequested; // set by the timeline view: routes "마커 추가" to the host's name dialog
private:
    friend struct TimelineUxTestAccess;
    int getNumRows() override { return int(markers.size()); }
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override;
    void selectedRowsChanged(int) override;
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override;
    void apply();
    juce::String rowTimeText(int) const;
    juce::File defaultExportFile() const;
    void chooseExportFile();
    void exportFile(const juce::File&, const RecorderProject&);
    TimelineEditController& edits;
    std::vector<Marker> markers;
    Id selected;
    juce::Viewport viewport;
    juce::Component content;
    juce::ListBox list;
    juce::TextEditor name, position;
    MarkerColourSwatches colours;
    juce::TextButton add {ko("마커 추가")}, change {ko("이름 변경")}, remove {ko("삭제")};
    juce::TextButton exportText {ko("텍스트로 내보내기")};
    std::function<juce::File()> chooseExportFileForTesting; // replace only the native save picker in headless UI tests
    std::unique_ptr<juce::FileChooser> chooser;
};
}
