#pragma once
#include "model/RecorderModel.h"
#include <map>

namespace gocue::recorder
{
// Shared by TimelineView paint/hit testing and the device-free scale probe.
// Rebuilt only when the immutable document snapshot changes, never per paint.
class TimelineVisibleIndex
{
public:
    using Clips = std::vector<const Clip*>;
    struct Range
    {
        Clips::const_iterator first, last;
        std::size_t comparisons = 0;
        auto begin() const { return first; }
        auto end() const { return last; }
        std::size_t size() const { return std::size_t(last - first); }
    };
    void rebuild(const RecorderProject&, const std::vector<Track>& displayedTracks);
    Range visible(std::size_t row, Sample begin, Sample end) const;
    const Clip* find(const Id& id) const { const auto it = byId.find(id); return it == byId.end() ? nullptr : it->second; }
    std::size_t size() const noexcept { return clipCount; }
    Sample timelineEnd() const noexcept { return lastSample; }
private:
    struct Row { SharedList<Clip> owner; Clips clips; std::vector<Sample> maximumEnds; };
    std::vector<Row> rows;
    std::map<Id, const Clip*> byId;
    std::size_t clipCount = 0;
    Sample lastSample = 0;
};
struct TimelineLayout
{
    struct Box { int x = 0, y = 0, width = 0, height = 0; };
    static constexpr int headerWidth = 210, rulerHeight = 30, rowHeight = 72;
    // JUCE uses logical coordinates; DPI is applied once at the HWND boundary.
    static std::array<Box, 2> cameras(int logicalWidth, int logicalHeight);
    static Box physical(Box, double scale);
    static std::pair<int, int> visibleRows(int top, int bottom, int count);
    static std::pair<int, int> waveColumns(int left, int right, int clipLeft, int clipRight);
};
// Metadata only; queries the same production interval/layout helpers. No latency
// claim about GPU, OS window painting or DAC is made from this report.
juce::var timelineScaleReport(std::size_t clipCount);
}
