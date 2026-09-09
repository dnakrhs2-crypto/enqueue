#pragma once
#include "RecorderTransportBar.h"
#include "TimelineView.logic.h"
#include "ClipInspector.h"
#include "TrackHeader.h"
#include "MarkerPanel.h"
#include "media/PeakCache.h"
#include "media/ThumbnailCache.h"
#include <map>

namespace gocue::recorder
{
class TimelineView : public juce::Component, private juce::ScrollBar::Listener, private juce::KeyListener
{
public:
    explicit TimelineView(RecorderDocument&);
    ~TimelineView() override;
    void refresh(bool locked, Sample cursor, const juce::String& latestStatus);
    void setPeaks(const Id&, std::shared_ptr<PeakCache>, unsigned channel);
    void setLoadedPeaks(const Id&, PeakSnapshot, unsigned channel);
    void setThumbnails(const Id&, std::vector<ThumbnailFrame>);
    void clearCaches();
    void zoom(double factor);
    void zoomToFit();
    void reveal(Sample);
    void resized() override;
    void visibilityChanged() override { if (isShowing()) grabKeyboardFocus(); }
    TimelineEditController edits;
    juce::Result invoke(TimelineAction, Sample value = 0, bool exact = false);
    void selectionChanged();
    RecorderTransportBar transport;
    std::function<void(Sample, bool)> onScrub;
    std::function<void()> onListeningChanged;
    std::int64_t lastClipPaintQpc = 0;
    Id lastPaintedTake;
    unsigned rowPaintCount = 0;
private:
    struct PeakDisplay { std::shared_ptr<PeakCache> live; std::shared_ptr<const PeakSnapshot> data; unsigned channel = 0; };
    struct Thumb { Sample sample; juce::Image image; };
    class Rows : public juce::Component
    {
    public:
        explicit Rows(TimelineView& v) : view(v) {}
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        TimelineView& view;
        enum class Drag { none, scrub, range, clip } dragging = Drag::none;
        Sample downSample = 0, downEdge = 0;
        Id downClip;
        bool moved = false;
        bool collapseSelection = false;
    } rows;
    void rebuildHeaders();
    void scrollBarMoved(juce::ScrollBar*, double) override;
    void updateRange();
    void finish(const juce::Result&, bool playback = true);
    void showEditMenu();
    void showRipplePrompt();
    void updateControls();
    void setRangeFromInputs();
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    void drawWave(juce::Graphics&, const Clip&, juce::Rectangle<float>);
    double xFor(Sample) const;
    Sample sampleFor(double x) const;
    static constexpr int headerWidth = 210, rulerHeight = 30, rowHeight = 72;
    RecorderDocument& document;
    RecorderDocument::Snapshot shown;
    juce::Viewport viewport;
    juce::ScrollBar horizontal {false};
    juce::Label selectionInfo;
    juce::Viewport toolbarViewport;
    juce::Component toolbar;
    juce::TextButton menuButton {ko("편집")}, snapButton {ko("스냅 켜짐")};
    std::map<TimelineAction, std::unique_ptr<juce::TextButton>> buttons;
    juce::TextEditor rangeStart, rangeEnd;
    juce::TextButton rangeButton {ko("구간 선택")};
    juce::TabbedComponent sidebar {juce::TabbedButtonBar::TabsAtTop};
    ClipInspector inspector;
    MarkerPanel markerPanel;
    std::vector<Track> tracks;
    std::vector<std::unique_ptr<TrackHeader>> headers;
    std::map<Id, PeakDisplay> peaks;
    std::map<Id, std::vector<Thumb>> thumbnails;
    double viewStart = 0, viewSeconds = 20;
    Sample playhead = 0;
    bool locked = false;
    juce::String latestStatus;
    juce::String editStatus;
    std::unique_ptr<ClipEditResult> orderPreview;
    std::uint32_t lastPeakRefresh = 0;
};
std::unique_ptr<juce::DocumentWindow> createTimelineAutomationWindow(const juce::File&, std::function<void(int)>);
}
