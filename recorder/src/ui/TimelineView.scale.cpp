#include "TimelineView.scale.h"
#include <algorithm>
#include <cmath>

namespace gocue::recorder
{
void TimelineVisibleIndex::rebuild(const RecorderProject& project, const std::vector<Track>& tracks)
{
    rows.clear(); byId.clear(); clipCount = 0; lastSample = 0;
    std::map<Id, Id> active;
    for (const auto& s : project.takeStacks) active.emplace(s.stackId, s.activeVersionId);
    for (const auto& track : tracks)
    {
        Row row; row.owner = track.clips;
        for (const auto& c : row.owner.items())
        {
            const auto stack = active.find(c.takeStackId);
            if (c.takeStackId.isNotEmpty() && (stack == active.end() || c.versionId != stack->second)) continue;
            row.clips.push_back(&c); byId.emplace(c.clipId, &c); lastSample = (std::max)(lastSample, c.timelineEnd());
        }
        std::sort(row.clips.begin(), row.clips.end(), [](const auto* a, const auto* b) { return a->timelineStartSample < b->timelineStartSample; });
        // Invalid edit previews can overlap. Prefix ends keep an enclosing
        // ghost visible even when a shorter clip ends before the viewport.
        for (const auto* c : row.clips)
            row.maximumEnds.push_back(row.maximumEnds.empty() ? c->timelineEnd() : (std::max)(row.maximumEnds.back(), c->timelineEnd()));
        clipCount += row.clips.size(); rows.push_back(std::move(row));
    }
}
TimelineVisibleIndex::Range TimelineVisibleIndex::visible(std::size_t row, Sample begin, Sample end) const
{
    const auto& indexed = rows.at(row); const auto& clips = indexed.clips; std::size_t comparisons = 0;
    const auto first = std::upper_bound(indexed.maximumEnds.begin(), indexed.maximumEnds.end(), begin, [&](Sample at, Sample last)
        { ++comparisons; return at < last; });
    const auto a = clips.begin() + (first - indexed.maximumEnds.begin());
    const auto b = end <= begin ? a : std::lower_bound(a, clips.end(), end, [&](const Clip* c, Sample at)
        { ++comparisons; return c->timelineStartSample < at; });
    return {a, b, comparisons};
}
std::array<TimelineLayout::Box, 2> TimelineLayout::cameras(int width, int height)
{
    const int cell = (std::max)(0, (width - 12) / 2);
    const int w = (std::min)(cell, (std::max)(0, height) * 16 / 9), h = w * 9 / 16;
    return {{{(cell - w) / 2, (height - h) / 2, w, h},
             {cell + 12 + (cell - w) / 2, (height - h) / 2, w, h}}};
}
TimelineLayout::Box TimelineLayout::physical(Box b, double scale)
{
    if (!std::isfinite(scale) || scale <= 0) throw std::invalid_argument("Invalid DPI scale");
    const auto px = [scale](int n) { return int(std::lround(n * scale)); };
    return {px(b.x), px(b.y), px(b.x + b.width) - px(b.x), px(b.y + b.height) - px(b.y)};
}
std::pair<int, int> TimelineLayout::visibleRows(int top, int bottom, int count)
{
    const auto a = (std::clamp)((top - rulerHeight) / rowHeight, 0, count);
    const auto b = (std::clamp)((bottom - rulerHeight + rowHeight - 1) / rowHeight, a, count);
    return {a, b};
}
std::pair<int, int> TimelineLayout::waveColumns(int left, int right, int clipLeft, int clipRight)
{
    const auto a = (std::max)({headerWidth, left, clipLeft});
    return {a, (std::max)(a, (std::min)(right, clipRight))};
}
juce::var timelineScaleReport(std::size_t count)
{
    if (!count || count > 100000) throw std::invalid_argument("clip-count must be 1..100000");
    RecorderProject project;
    for (unsigned i = 0; i < 4; ++i)
    { Track t; t.kind = i == 0 ? TrackKind::cam1 : i == 1 ? TrackKind::cam2 : TrackKind::mic; t.name = juce::String::fromUTF8("아주 긴 한글 장치 이름 · 스튜디오 마이크 입력과 카메라"); project.tracks.push_back(std::move(t)); }
    for (std::size_t i = 0; i < count; ++i)
    {
        auto& t = project.tracks[i % 4]; Clip c; c.trackId = t.trackId;
        c.timelineStartSample = Sample(i / 4) * 96000; c.lengthSamples = 48000; t.clips.edit().push_back(std::move(c));
    }
    TimelineVisibleIndex index; const auto start = juce::Time::getHighResolutionTicks(); index.rebuild(project, project.tracks);
    std::size_t visited = 0, maxVisible = 0, comparisons = 0;
    for (unsigned query = 0; query < 1000; ++query)
    {
        const auto begin = index.timelineEnd() * query / 1000;
        std::size_t visible = 0;
        for (unsigned row = 0; row < 4; ++row)
        { const auto r = index.visible(row, begin, begin + 480000); visible += r.size(); comparisons += r.comparisons; }
        visited += visible; maxVisible = (std::max)(maxVisible, visible);
    }
    auto report = juce::var(new juce::DynamicObject()); auto* r = report.getDynamicObject();
    r->setProperty("mode", "headless-production-interval-layout"); r->setProperty("clipCount", juce::int64(index.size()));
    r->setProperty("queries", 1000); r->setProperty("visitedClips", juce::int64(visited)); r->setProperty("maxVisibleClips", juce::int64(maxVisible));
    r->setProperty("binarySearchComparisons", juce::int64(comparisons));
    r->setProperty("buildAndQueryMs", 1000.0 * (juce::Time::getHighResolutionTicks() - start) / juce::Time::getHighResolutionTicksPerSecond());
    juce::Array<juce::var> layouts;
    for (const auto dpi : {1.0, 1.5, 2.0})
    {
        const auto cams = TimelineLayout::cameras(960, 250);
        const auto a = TimelineLayout::physical(cams[0], dpi), b = TimelineLayout::physical(cams[1], dpi);
        auto layout = juce::var(new juce::DynamicObject()); auto* l = layout.getDynamicObject();
        l->setProperty("dpiPercent", int(dpi * 100)); l->setProperty("logicalWidth", 960); l->setProperty("logicalHeight", 640);
        l->setProperty("sideBySide", a.x + a.width < b.x && a.y == b.y && a.width == b.width);
        l->setProperty("cameraPixelWidth", a.width); l->setProperty("cameraPixelHeight", a.height);
        l->setProperty("longKoreanName", project.tracks.front().name); l->setProperty("namePolicy", "bounded label with full-name tooltip; real glyph/window capture pending"); layouts.add(layout);
    }
    r->setProperty("layouts", layouts); r->setProperty("realWindowVerified", false);
    r->setProperty("waveformColumnLimit", 960 - 270 - TimelineLayout::headerWidth);
    return report;
}
}
