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
    // Lands a finished temporary at target: a plain move when nothing is there, otherwise a swap that keeps the previous file
    // as a backup until the new one is in place. `swap` is Windows ReplaceFileW by default; tests inject its failure shapes.
    using SwapFunction = std::function<bool(const juce::File& target, const juce::File& temporary, const juce::File& backup)>;
    static juce::Result landExport(const juce::File& target, const juce::File& temporary, const SwapFunction& swap = {});
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
