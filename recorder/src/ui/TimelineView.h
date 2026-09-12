#pragma once
#include "RecorderTransportBar.h"
#include "TimelineView.logic.h"
#include "TimelineView.scale.h"
#include "TimelineInteraction.h"
#include "ShortcutKeys.h"
#include "ClipInspector.h"
#include "TrackHeader.h"
#include "MarkerPanel.h"
#include "media/PeakCache.h"
#include "media/ThumbnailCache.h"
#include <map>

namespace gocue::recorder
{
struct RecordingPreviewTargets
{
    std::array<bool, 2> cameras{};
    std::vector<unsigned> microphones; // Armed logical slots 1..8, as returned by the audio engine.
};
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
    void zoomAt(double factor, double rowX);
    void zoomToFit();
    void reveal(Sample);
    void setRecordingPreview(bool active, Sample placement, Sample elapsed, const RecordingPreviewTargets&, const UserSettings&);
    void setShortcuts(const RecorderShortcuts& value) { if (shortcuts.keys != value.keys) { shortcuts = value; updateControls(); } }
    std::function<bool(const juce::KeyPress&, juce::Component*)> onGlobalKey;
    // Host-owned marker entry (name dialog at the captured time). Unset: the marker is added immediately at the playhead.
    std::function<void()> onAddMarkerRequested;
    void revealTrack(unsigned row) { viewport.setViewPosition(0, rulerHeight + int(row) * rowHeight); }
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
    std::size_t lastPaintVisitedClips = 0, lastPaintWaveColumns = 0;
private:
    friend struct StabilityTestAccess;
    friend struct TimelineUxTestAccess;
    struct PeakDisplay { std::shared_ptr<PeakCache> live; std::shared_ptr<const PeakSnapshot> data; unsigned channel = 0; };
    struct Thumb { Sample sample; juce::Image image; };
    class Rows : public juce::Component, private juce::Timer
    {
    public:
        explicit Rows(TimelineView& v) : view(v) {}
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void cancelGesture(bool restore = true);
        TimelineView& view;
        enum class Drag { none, scrub, range, clip } dragging = Drag::none;
        Sample downSample = 0, downEdge = 0;
        Id downClip;
        RecorderDocument::Snapshot downSnapshot;
        std::vector<Id> downSelection, downExplicitSelection, downTargets;
        Sample downRevision = 0, downStart = 0, downEnd = 0;
        bool moved = false;
        bool collapseSelection = false;
        TimelineAction pendingAction = TimelineAction::move;
        TimelineSnapIndex snapIndex;
        std::optional<Sample> snapGuide;
        juce::Point<float> lastPointer;
        juce::ModifierKeys lastModifiers;
        Sample previousPlayhead = 0;
        double previousViewStart = 0;
        std::optional<SampleRange> previousRange;
        int downRow = -1;
    private:
        bool pendingClipUnchanged() const;
        void updateDrag();
        void timerCallback() override;
        std::uint32_t lastAutoScroll = 0;
    } rows;
    void rebuildHeaders();
    void rebuildPreview();
    void scrollBarMoved(juce::ScrollBar*, double) override;
    void updateRange();
    void finish(const juce::Result&, bool playback = true);
    void showEditMenu(bool atMouse = false);
    juce::PopupMenu createEditMenu() const;
    bool isRecordingTrack(const Track&) const;
    void showRipplePrompt();
    void updateControls();
    void setRangeFromInputs();
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    void drawWave(juce::Graphics&, const Clip&, juce::Rectangle<float>);
    juce::Image thumbnailFor(const Id&, Sample, bool priority = false);
    void drawScrubPreview(juce::Graphics&);
    double xFor(Sample) const;
    Sample sampleFor(double x) const;
    static constexpr int headerWidth = TimelineLayout::headerWidth, rulerHeight = TimelineLayout::rulerHeight, rowHeight = TimelineLayout::rowHeight;
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
    TimelineVisibleIndex visibleIndex, previewIndex;
    std::map<Id, const Take*> takeForAsset;
    std::map<Id, const MediaAsset*> assetById;
    std::vector<std::unique_ptr<TrackHeader>> headers;
    std::map<Id, PeakDisplay> peaks;
    std::map<Id, std::vector<Thumb>> thumbnails;
    struct Converted { std::shared_ptr<const ThumbnailFrame> source; juce::Image image; std::uint64_t used; };
    std::map<const ThumbnailFrame*, Converted> convertedThumbnails;
    ThumbnailCache progressiveThumbnails;
    std::uint64_t thumbnailAccess = 0;
    double viewStart = 0, viewSeconds = 20;
    Sample playhead = 0;
    bool locked = false;
    RecorderShortcuts shortcuts;
    struct RecordingPreview
    {
        bool active = false, follow = true;
        Sample start = 0, length = 0;
        std::array<bool, 2> cameras{};
        std::array<bool, 8> microphones{};
        std::array<juce::String, 8> microphoneLabels;
    } recordingPreview;
    juce::String latestStatus;
    juce::String editStatus;
    std::unique_ptr<ClipEditResult> orderPreview;
    std::uint32_t lastPeakRefresh = 0;
};
std::unique_ptr<juce::DocumentWindow> createTimelineAutomationWindow(const juce::File&, std::function<void(int)>);
}
