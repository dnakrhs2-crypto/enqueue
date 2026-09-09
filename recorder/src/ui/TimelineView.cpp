#include "TimelineView.h"
#include <algorithm>
#include <cmath>

namespace gocue::recorder
{
struct TimelineView::Header : juce::Component
{
    juce::Label name; juce::TextButton mute {ko("음소거")}, solo {ko("솔로")};
    Header() { addAndMakeVisible(name); addAndMakeVisible(mute); addAndMakeVisible(solo); name.setFont(juce::Font(juce::FontOptions(17, juce::Font::bold))); }
    void resized() override { name.setBounds(8, 2, getWidth() - 16, 28); mute.setBounds(8, 34, 84, 28); solo.setBounds(98, 34, 70, 28); }
};
TimelineView::TimelineView(RecorderDocument& d) : rows(*this), document(d)
{
    addAndMakeVisible(transport); addAndMakeVisible(viewport); addAndMakeVisible(horizontal); addAndMakeVisible(selectionInfo);
    selectionInfo.setFont(juce::Font(juce::FontOptions(15))); selectionInfo.setColour(juce::Label::textColourId, Palette::dimText);
    viewport.setViewedComponent(&rows, false); viewport.setScrollBarsShown(true, false); horizontal.addListener(this); horizontal.setAutoHide(false);
    transport.zoomIn.onClick = [this] { zoom(.5); }; transport.zoomOut.onClick = [this] { zoom(2); }; transport.fit.onClick = [this] { zoomToFit(); };
    refresh(false, 0, {});
}
TimelineView::~TimelineView() { horizontal.removeListener(this); }
void TimelineView::clearCaches() { peaks.clear(); thumbnails.clear(); shown.reset(); lastClipPaintQpc = 0; lastPaintedTake.clear(); viewStart = 0; }
void TimelineView::setPeaks(const Id& asset, std::shared_ptr<PeakCache> cache, unsigned channel) { peaks[asset] = {std::move(cache), {}, channel}; lastPeakRefresh = 0; }
void TimelineView::setLoadedPeaks(const Id& asset, PeakSnapshot data, unsigned channel)
{ if (!peaks.count(asset)) peaks[asset] = {nullptr, std::make_shared<const PeakSnapshot>(std::move(data)), channel}; rows.repaint(); }
void TimelineView::setThumbnails(const Id& asset, std::vector<ThumbnailFrame> frames)
{
    std::vector<Thumb> converted;
    for (const auto& frame : frames)
    {
        juce::Image image(juce::Image::RGB, frame.width, frame.height, false); juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x)
        { const auto* p = frame.rgb.data() + (std::size_t(y) * frame.width + x) * 3; bitmap.setPixelColour(x, y, juce::Colour(p[0], p[1], p[2])); }
        converted.push_back({frame.sample, std::move(image)});
    }
    thumbnails[asset] = std::move(converted); rows.repaint();
}
void TimelineView::rebuildHeaders()
{
    headers.clear(); tracks.clear(); const auto& p = document.getProject();
    for (auto kind : {TrackKind::cam1, TrackKind::cam2})
    { auto it = std::find_if(p.tracks.begin(), p.tracks.end(), [kind](const auto& t) { return t.kind == kind; }); if (it != p.tracks.end()) tracks.push_back(*it); else { Track t; t.kind = kind; t.name = ko(kind == TrackKind::cam1 ? "캠1" : "캠2"); tracks.push_back(t); } }
    for (const auto& t : p.tracks) if (t.kind == TrackKind::mic || t.kind == TrackKind::importAudio) tracks.push_back(t);
    for (const auto& track : tracks)
    {
        auto h = std::make_unique<Header>(); h->name.setText(track.name, juce::dontSendNotification); h->name.setTooltip(track.name);
        const bool audioTrack = track.kind == TrackKind::mic || track.kind == TrackKind::importAudio;
        h->mute.setVisible(audioTrack); h->solo.setVisible(audioTrack); h->mute.setEnabled(!locked); h->solo.setEnabled(!locked);
        h->mute.setToggleState(track.mute, juce::dontSendNotification); h->solo.setToggleState(track.solo, juce::dontSendNotification);
        const auto change = [this, id = track.trackId](bool solo)
        {
            if (locked) return;
            document.performEdit(solo ? ko("솔로") : ko("음소거"), [id, solo](EditState& e) { for (auto& t : e.tracks) if (t.trackId == id) { if (solo) t.solo = !t.solo; else t.mute = !t.mute; } });
            if (onListeningChanged) onListeningChanged();
        };
        h->mute.onClick = [change] { change(false); }; h->solo.onClick = [change] { change(true); };
        rows.addAndMakeVisible(*h); headers.push_back(std::move(h));
    }
    resized();
}
void TimelineView::refresh(bool isLocked, Sample at, const juce::String& status)
{
    const auto snapshot = document.snapshot(); const auto changed = shown != snapshot || locked != isLocked;
    shown = snapshot; locked = isLocked; playhead = at; latestStatus = status;
    if (changed) { rebuildHeaders(); updateRange(); }
    const auto now = juce::Time::getMillisecondCounter();
    if (!lastPeakRefresh || now - lastPeakRefresh >= 100)
    {
        std::map<PeakCache*, std::shared_ptr<const PeakSnapshot>> copies;
        for (auto& pair : peaks) if (pair.second.live)
        {
            auto& entry = pair.second; auto& copy = copies[entry.live.get()]; if (!copy) copy = std::make_shared<const PeakSnapshot>(entry.live->snapshot());
            entry.data = copy; if (copy->complete) entry.live.reset();
        }
        lastPeakRefresh = now;
    }
    juce::String selectionText = ko("클립 클릭 · 연결된 클립 함께 선택");
    if (!document.getSelection().empty()) if (const auto* c = snapshot->findClip(document.getSelection().front()))
        selectionText = ko("시작 ") + formatRecorderTime(c->timelineStartSample, snapshot->Fs) + ko(" · 끝 ") + formatRecorderTime(c->timelineEnd(), snapshot->Fs)
            + ko(" · 원본 시작 ") + formatRecorderTime(c->sourceIn, snapshot->Fs) + ko(" · 원본 끝 ") + formatRecorderTime(c->sourceIn + c->lengthSamples, snapshot->Fs)
            + (c->linkGroupId.isEmpty() ? juce::String() : ko(" · 링크"));
    selectionInfo.setText(selectionText, juce::dontSendNotification); rows.repaint();
}
double TimelineView::xFor(Sample s) const { return headerWidth + (double(s) / document.getProject().Fs - viewStart) / viewSeconds * juce::jmax(1, rows.getWidth() - headerWidth); }
Sample TimelineView::sampleFor(double x) const { return Sample(std::llround(juce::jmax(0.0, viewStart + (x - headerWidth) * viewSeconds / juce::jmax(1, rows.getWidth() - headerWidth)) * document.getProject().Fs)); }
void TimelineView::updateRange()
{
    const auto duration = juce::jmax(viewSeconds, double(document.getProject().activeTimelineEnd()) / document.getProject().Fs + 2);
    viewStart = juce::jlimit(0.0, juce::jmax(0.0, duration - viewSeconds), viewStart);
    horizontal.setRangeLimits(0, duration, juce::dontSendNotification); horizontal.setCurrentRange(viewStart, viewSeconds, juce::dontSendNotification);
}
void TimelineView::zoom(double factor)
{
    const auto anchor = double(playhead) / document.getProject().Fs; const auto fraction = juce::jlimit(0.0, 1.0, (anchor - viewStart) / viewSeconds);
    viewSeconds = juce::jlimit(.1, 10800.0, viewSeconds * factor); viewStart = juce::jmax(0.0, anchor - fraction * viewSeconds); updateRange(); rows.repaint();
}
void TimelineView::zoomToFit() { viewStart = 0; viewSeconds = juce::jmax(5.0, double(document.getProject().activeTimelineEnd()) / document.getProject().Fs + 1); updateRange(); rows.repaint(); }
void TimelineView::reveal(Sample at) { viewStart = juce::jmax(0.0, double(at) / document.getProject().Fs - viewSeconds * .05); updateRange(); rows.repaint(); }
void TimelineView::scrollBarMoved(juce::ScrollBar*, double start) { viewStart = start; rows.repaint(); }
void TimelineView::resized()
{
    auto a = getLocalBounds(); transport.setBounds(a.removeFromTop(38)); selectionInfo.setBounds(a.removeFromBottom(24)); horizontal.setBounds(a.removeFromBottom(14).withTrimmedLeft(headerWidth)); viewport.setBounds(a);
    rows.setSize(juce::jmax(1, a.getWidth() - 16), juce::jmax(a.getHeight(), rulerHeight + int(tracks.size()) * rowHeight));
    for (unsigned i = 0; i < headers.size(); ++i) headers[i]->setBounds(0, rulerHeight + int(i) * rowHeight, headerWidth - 4, rowHeight - 2);
}
void TimelineView::drawWave(juce::Graphics& g, const Clip& clip, juce::Rectangle<float> box)
{
    const auto found = peaks.find(clip.assetId); if (found == peaks.end() || !found->second.data) return;
    const auto& s = *found->second.data; const auto ch = found->second.channel; if (ch >= s.channels || s.bins.empty()) return;
    const auto left = juce::jmax(headerWidth, int(box.getX())), right = juce::jmin(rows.getWidth(), int(box.getRight()));
    g.setColour(Palette::meterGreen); const auto middle = box.getCentreY();
    for (int x = left; x < right; ++x)
    {
        const auto first = juce::jmax(Sample{0}, clip.sourceIn + sampleFor(x) - clip.timelineStartSample);
        const auto last = juce::jmax(first + 1, clip.sourceIn + sampleFor(x + 1) - clip.timelineStartSample);
        const auto a = std::uint64_t(first) / s.samplesPerBin, b = juce::jmin<std::uint64_t>(s.bins.size(), (std::uint64_t(last) + s.samplesPerBin - 1) / s.samplesPerBin);
        if (a >= b) continue; auto low = s.bins[std::size_t(a)][ch].minimum, high = s.bins[std::size_t(a)][ch].maximum;
        for (auto i = a + 1; i < b; ++i) { low = juce::jmin(low, s.bins[std::size_t(i)][ch].minimum); high = juce::jmax(high, s.bins[std::size_t(i)][ch].maximum); }
        g.drawVerticalLine(x, middle - high * box.getHeight() * .47f, juce::jmax(middle - high * box.getHeight() * .47f + 1, middle - low * box.getHeight() * .47f));
    }
}
void TimelineView::Rows::paint(juce::Graphics& g)
{
    auto& v = view; const auto& p = v.document.getProject(); g.fillAll(Palette::card);
    const auto clipBounds = g.getClipBounds(); const auto step = v.viewSeconds <= 2 ? .1 : v.viewSeconds <= 10 ? 1.0 : v.viewSeconds <= 60 ? 5.0 : v.viewSeconds <= 300 ? 30.0 : 60.0;
    g.setFont(juce::Font(juce::FontOptions(14))); g.setColour(Palette::dimText);
    for (double t = std::ceil(v.viewStart / step) * step; t <= v.viewStart + v.viewSeconds; t += step)
    { const int x = int(v.xFor(Sample(std::llround(t * p.Fs)))); g.drawVerticalLine(x, 22, float(getHeight())); g.drawText(formatRecorderTime(Sample(t * p.Fs), p.Fs).substring(3, 8), x + 3, 0, 64, 22, juce::Justification::centredLeft); }
    for (unsigned row = 0; row < v.tracks.size(); ++row)
    {
        const auto y = rulerHeight + int(row) * rowHeight; if (y > clipBounds.getBottom() || y + rowHeight < clipBounds.getY()) continue;
        g.setColour(Palette::line); g.drawHorizontalLine(y + rowHeight - 1, 0, float(getWidth()));
        for (const auto& c : v.tracks[row].clips.items()) if (p.isActive(c))
        {
            const auto left = float(v.xFor(c.timelineStartSample)), right = float(v.xFor(c.timelineEnd())); if (right <= headerWidth || left >= getWidth()) continue;
            auto box = juce::Rectangle<float>(left, float(y + 3), juce::jmax(2.0f, right - left), float(rowHeight - 6));
            const juce::Graphics::ScopedSaveState save(g); g.reduceClipRegion(headerWidth, y, getWidth() - headerWidth, rowHeight);
            const bool selected = std::find(v.document.getSelection().begin(), v.document.getSelection().end(), c.clipId) != v.document.getSelection().end();
            g.setColour(Palette::card2); g.fillRoundedRectangle(box, 5);
            const Take* take = nullptr;
            for (const auto& t : p.media->takes) if (t.cam1AssetId == c.assetId || t.cam2AssetId == c.assetId || std::find(t.microphoneAssetIds.begin(), t.microphoneAssetIds.end(), c.assetId) != t.microphoneAssetIds.end()) { take = &t; break; }
            auto state = take ? take->state == TakeState::complete ? ko("완료") : take->state == TakeState::partial ? ko("확인 필요") : ko("마무리 중") : juce::String();
            if (take && !p.media->takes.empty() && take->takeId == p.media->takes.back().takeId && v.latestStatus.isNotEmpty()) state = v.latestStatus;
            const auto imageBox = box.withTrimmedTop(24).reduced(2);
            const auto thumbs = v.thumbnails.find(c.assetId);
            if (thumbs != v.thumbnails.end() && !thumbs->second.empty())
            {
                for (float x = juce::jmax(float(headerWidth), imageBox.getX()); x < juce::jmin(float(getWidth()), imageBox.getRight()); x += 72)
                {
                    const auto sample = c.sourceIn + v.sampleFor(x) - c.timelineStartSample; const auto& images = thumbs->second;
                    auto it = std::upper_bound(images.begin(), images.end(), sample, [](Sample s, const Thumb& t) { return s < t.sample; }); if (it != images.begin()) --it;
                    g.drawImage(it->image, juce::Rectangle<float>(x, imageBox.getY(), 70.0f, imageBox.getHeight()), juce::RectanglePlacement::stretchToFit);
                }
            }
            else v.drawWave(g, c, imageBox);
            g.setColour(Palette::text); g.setFont(juce::Font(juce::FontOptions(14)));
            auto labelBounds = box.withHeight(24).withTrimmedLeft(c.linkGroupId.isEmpty() ? 6 : 28);
            g.drawText((take ? take->name : ko("클립")) + " · " + state, labelBounds, juce::Justification::centredLeft, true);
            if (c.linkGroupId.isNotEmpty()) { g.setColour(Palette::accent); g.drawRoundedRectangle(box.getX() + 6, box.getY() + 8, 12, 7, 3, 1.5f); g.drawRoundedRectangle(box.getX() + 12, box.getY() + 10, 12, 7, 3, 1.5f); }
            g.setColour(selected ? Palette::accent : Palette::line); g.drawRoundedRectangle(box, 5, selected ? 2.0f : 1.0f);
            if (take && !p.media->takes.empty() && take->takeId == p.media->takes.back().takeId
                && (v.lastPaintedTake != take->takeId || !v.lastClipPaintQpc)) { v.lastPaintedTake = take->takeId; v.lastClipPaintQpc = qpcNow(); }
        }
    }
    const auto x = int(v.xFor(v.playhead)); if (x >= headerWidth && x < getWidth()) { g.setColour(Palette::danger); g.drawVerticalLine(x, 0, float(getHeight())); g.fillEllipse(float(x - 4), 0, 8, 8); }
}
void TimelineView::Rows::mouseDown(const juce::MouseEvent& e)
{
    auto& v = view; if (v.locked || e.x < headerWidth) return;
    if (e.y < rulerHeight) { dragging = true; v.playhead = juce::jmin(v.sampleFor(e.x), v.document.getProject().activeTimelineEnd()); if (v.onScrub) v.onScrub(v.playhead, false); repaint(); return; }
    const auto row = (e.y - rulerHeight) / rowHeight; if (row < 0 || row >= int(v.tracks.size())) return;
    const auto at = v.sampleFor(e.x); std::vector<Id> selected;
    for (const auto& c : v.tracks[std::size_t(row)].clips.items()) if (v.document.getProject().isActive(c) && at >= c.timelineStartSample && at < c.timelineEnd())
    {
        selected.push_back(c.clipId); for (const auto& group : v.document.getProject().linkGroups) if (group.linkGroupId == c.linkGroupId) selected = group.clipIds; break;
    }
    v.document.setSelection(std::move(selected)); repaint();
}
void TimelineView::Rows::mouseDrag(const juce::MouseEvent& e)
{ if (dragging && !view.locked) { view.playhead = juce::jmin(view.sampleFor(e.x), view.document.getProject().activeTimelineEnd()); if (view.onScrub) view.onScrub(view.playhead, false); repaint(); } }
void TimelineView::Rows::mouseUp(const juce::MouseEvent& e)
{ if (dragging && !view.locked) { dragging = false; view.playhead = juce::jmin(view.sampleFor(e.x), view.document.getProject().activeTimelineEnd()); if (view.onScrub) view.onScrub(view.playhead, true); repaint(); } }
void TimelineView::Rows::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{ if (e.mods.isCtrlDown()) view.zoom(wheel.deltaY > 0 ? .8 : 1.25); else if (e.mods.isShiftDown() || std::abs(wheel.deltaX) > .001f) { view.viewStart += -(wheel.deltaX ? wheel.deltaX : wheel.deltaY) * view.viewSeconds; view.updateRange(); repaint(); } else juce::Component::mouseWheelMove(e, wheel); }
}
