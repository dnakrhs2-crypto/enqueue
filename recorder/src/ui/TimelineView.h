#pragma once
#include "RecorderTransportBar.h"
#include "media/PeakCache.h"
#include "media/ThumbnailCache.h"
#include <map>

namespace gocue::recorder
{
class TimelineView : public juce::Component, private juce::ScrollBar::Listener
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
    RecorderTransportBar transport;
    std::function<void(Sample, bool)> onScrub;
    std::function<void()> onListeningChanged;
    std::int64_t lastClipPaintQpc = 0;
    Id lastPaintedTake;
private:
    struct PeakDisplay { std::shared_ptr<PeakCache> live; std::shared_ptr<const PeakSnapshot> data; unsigned channel = 0; };
    struct Thumb { Sample sample; juce::Image image; };
    struct Header;
    class Rows : public juce::Component
    {
    public:
        explicit Rows(TimelineView& v) : view(v) {}
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        TimelineView& view;
        bool dragging = false;
    } rows;
    void rebuildHeaders();
    void scrollBarMoved(juce::ScrollBar*, double) override;
    void updateRange();
    void drawWave(juce::Graphics&, const Clip&, juce::Rectangle<float>);
    double xFor(Sample) const;
    Sample sampleFor(double x) const;
    static constexpr int headerWidth = 210, rulerHeight = 30, rowHeight = 72;
    RecorderDocument& document;
    RecorderDocument::Snapshot shown;
    juce::Viewport viewport;
    juce::ScrollBar horizontal {false};
    juce::Label selectionInfo;
    std::vector<Track> tracks;
    std::vector<std::unique_ptr<Header>> headers;
    std::map<Id, PeakDisplay> peaks;
    std::map<Id, std::vector<Thumb>> thumbnails;
    double viewStart = 0, viewSeconds = 20;
    Sample playhead = 0;
    bool locked = false;
    juce::String latestStatus;
    std::uint32_t lastPeakRefresh = 0;
};
}
