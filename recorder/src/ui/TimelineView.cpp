#include "TimelineView.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace gocue::recorder
{
namespace
{
Sample previewAdd(Sample a, Sample b)
{
    constexpr auto hi = (std::numeric_limits<Sample>::max)(), lo = (std::numeric_limits<Sample>::min)();
    if (b > 0 && a > hi - b) return hi;
    if (b < 0 && a < lo - b) return lo;
    return a + b;
}
Sample previewSubtract(Sample a, Sample b)
{
    constexpr auto hi = (std::numeric_limits<Sample>::max)(), lo = (std::numeric_limits<Sample>::min)();
    if (b > 0 && a < lo + b) return lo;
    if (b < 0 && a > hi + b) return hi;
    return a - b;
}
Track trackLabel(const Track& track, const RecorderProject& project)
{
    auto display = track;
    if (track.kind == TrackKind::mic)
        for (const auto& clip : track.clips.items()) if (project.isActive(clip))
            if (const auto* asset = project.media->findAsset(clip.assetId); asset && asset->originalFormat.channels == 2)
            { display.name += juce::String::fromUTF8(" · 스테레오"); break; }
    return display;
}
}
TimelineView::TimelineView(RecorderDocument& d) : edits(d), rows(*this), document(d), inspector(edits), markerPanel(edits)
{
    addAndMakeVisible(transport); addAndMakeVisible(viewport); addAndMakeVisible(horizontal); addAndMakeVisible(selectionInfo);
    selectionInfo.setFont(juce::Font(juce::FontOptions(15))); selectionInfo.setColour(juce::Label::textColourId, Palette::dimText);
    viewport.setViewedComponent(&rows, false); viewport.setScrollBarsShown(true, false); viewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never); horizontal.addListener(this); horizontal.setAutoHide(false);
    transport.zoomIn.onClick = [this] { zoom(.5); }; transport.zoomOut.onClick = [this] { zoom(2); }; transport.fit.onClick = [this] { zoomToFit(); };
    setWantsKeyboardFocus(true); rows.setWantsKeyboardFocus(true); addKeyListener(this);
    addAndMakeVisible(toolbarViewport); toolbarViewport.setViewedComponent(&toolbar, false); toolbarViewport.setScrollBarsShown(false, true);
    toolbar.addAndMakeVisible(menuButton); menuButton.setWantsKeyboardFocus(false); menuButton.onClick = [this] { showEditMenu(); };
    for (auto action : {TimelineAction::split, TimelineAction::trimIn, TimelineAction::trimOut, TimelineAction::remove, TimelineAction::rippleAll,
                        TimelineAction::earlier, TimelineAction::later, TimelineAction::unlink, TimelineAction::link, TimelineAction::undo, TimelineAction::redo, TimelineAction::addMarker})
    {
        auto b = std::make_unique<juce::TextButton>(TimelineEditController::text(action)); b->setWantsKeyboardFocus(false);
        b->onClick = [this, action] { if (action == TimelineAction::addMarker && onAddMarkerRequested) onAddMarkerRequested(); else invoke(action, edits.playhead()); };
        if (action == TimelineAction::earlier || action == TimelineAction::later)
            b->onStateChange = [this, action, button = b.get()]
            {
                if (button->isMouseOver() && button->isEnabled())
                {
                    orderPreview = std::make_unique<ClipEditResult>(edits.preview(action));
                    if (orderPreview->status.failed()) { editStatus = orderPreview->status.getErrorMessage(); selectionInfo.setColour(juce::Label::textColourId, Palette::danger); selectionInfo.setText(editStatus, juce::dontSendNotification); }
                }
                else orderPreview.reset();
                rebuildPreview();
                rows.repaint();
            };
        toolbar.addAndMakeVisible(*b); buttons.emplace(action, std::move(b));
    }
    for (auto* field : {&rangeStart, &rangeEnd}) { toolbar.addAndMakeVisible(field); field->setInputRestrictions(19, "0123456789"); field->onReturnKey = [this] { setRangeFromInputs(); }; }
    rangeStart.setTextToShowWhenEmpty(ko("구간 시작 · 샘플"), Palette::dimText); rangeEnd.setTextToShowWhenEmpty(ko("구간 끝 · 샘플"), Palette::dimText);
    rangeStart.setTooltip(ko("선택 구간 시작 · 정수 샘플")); rangeEnd.setTooltip(ko("선택 구간 끝 · 정수 샘플"));
    toolbar.addAndMakeVisible(rangeButton); rangeButton.setWantsKeyboardFocus(false); rangeButton.onClick = [this] { setRangeFromInputs(); };
    addAndMakeVisible(snapButton); snapButton.setClickingTogglesState(true); snapButton.setToggleState(true, juce::dontSendNotification); snapButton.setWantsKeyboardFocus(false);
    snapButton.setTooltip(ko("클립 경계·마커·재생헤드에 붙이기 · 영상은 프레임 · Alt 드래그로 해제"));
    snapButton.onClick = [this] { snapButton.setButtonText(snapButton.getToggleState() ? ko("스냅 켜짐") : ko("스냅 꺼짐")); };
    addAndMakeVisible(sidebar); sidebar.addTab(ko("클립 속성"), Palette::card, &inspector, false); sidebar.addTab(ko("마커"), Palette::card, &markerPanel, false); sidebar.setTabBarDepth(28);
    inspector.onEdit = [this](const juce::Result& r) { finish(r); }; markerPanel.onEdit = [this](const juce::Result& r) { finish(r, false); };
    markerPanel.onAddRequested = [this] { if (onAddMarkerRequested) onAddMarkerRequested(); else finish(edits.addMarker(), false); };
    edits.onSeek = [this](Sample at, bool released) { playhead = at; if (onScrub) onScrub(at, released); reveal(at); };
    refresh(false, 0, {});
}
TimelineView::~TimelineView() { removeKeyListener(this); horizontal.removeListener(this); sidebar.clearTabs(); }
void TimelineView::clearCaches() { rows.cancelGesture(false); edits.clearRange(); orderPreview.reset(); rebuildPreview(); peaks.clear(); thumbnails.clear(); progressiveThumbnails.invalidate(); convertedThumbnails.clear(); shown.reset(); lastClipPaintQpc = 0; lastPaintedTake.clear(); viewStart = 0; editStatus.clear(); }
void TimelineView::setPeaks(const Id& asset, std::shared_ptr<PeakCache> cache, unsigned channel) { peaks[asset] = {std::move(cache), {}, channel}; lastPeakRefresh = 0; }
void TimelineView::setLoadedPeaks(const Id& asset, PeakSnapshot data, unsigned channel)
{ if (!peaks.count(asset)) peaks[asset] = {nullptr, std::make_shared<const PeakSnapshot>(std::move(data)), channel}; rows.repaint(); }
void TimelineView::setThumbnails(const Id& asset, std::vector<ThumbnailFrame> frames)
{
    std::vector<Thumb> converted; std::size_t imageBytes = 0;
    for (const auto& frame : frames)
    {
        if (converted.size() >= 32) break;
        if (frame.width <= 0 || frame.height <= 0 || frame.width > 640 || frame.height > 360
            || frame.rgb.size() != std::size_t(frame.width) * frame.height * 3) continue;
        const auto bytes = std::size_t(frame.width) * frame.height * 4;
        if (imageBytes + bytes > ThumbnailCache::maximumBytes) break;
        imageBytes += bytes;
        juce::Image image(juce::Image::RGB, frame.width, frame.height, false); juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x)
        { const auto* p = frame.rgb.data() + (std::size_t(y) * frame.width + x) * 3; bitmap.setPixelColour(x, y, juce::Colour(p[0], p[1], p[2])); }
        converted.push_back({frame.sample, std::move(image)});
    }
    if (converted.empty()) return;
    std::sort(converted.begin(), converted.end(), [](const auto& a, const auto& b) { return a.sample < b.sample; });
    thumbnails[asset] = std::move(converted);
    const auto bytes = [&]
    { std::size_t n = 0; for (const auto& group : thumbnails) for (const auto& t : group.second) n += std::size_t(t.image.getWidth()) * t.image.getHeight() * 4; return n; };
    while (bytes() > ThumbnailCache::maximumBytes && thumbnails.size() > 1)
    { auto it = thumbnails.begin(); if (it->first == asset) ++it; thumbnails.erase(it); }
    rows.repaint();
}
juce::Image TimelineView::thumbnailFor(const Id& assetId, Sample sourceSample, bool priority)
{
    const auto asset = assetById.find(assetId);
    if (asset == assetById.end()) return {};
    const auto& a = *asset->second;
    if (a.kind != AssetKind::camera || sourceSample < 0 || sourceSample >= a.logicalLength) return {};
    const auto key = document.getProject().projectId + "/" + assetId + "/" + juce::String(a.mediaGeneration);
    const auto frameSample = ThumbnailCache::frameSample(sourceSample, document.getProject().Fs, a.originalFormat.fps);
    if (a.relativePath.isNotEmpty() && !a.relativePath.containsIgnoreCase(".recording.") && document.getFile() != juce::File())
    {
        progressiveThumbnails.request(key, document.getFile().getParentDirectory().getChildFile(a.relativePath),
            document.getProject().Fs, frameSample, priority);
    }
    if (const auto frame = progressiveThumbnails.at(key, sourceSample))
    {
        auto found = convertedThumbnails.find(frame.get());
        if (found == convertedThumbnails.end())
        {
            if (convertedThumbnails.size() >= 64)
            {
                const auto oldest = std::min_element(convertedThumbnails.begin(), convertedThumbnails.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                convertedThumbnails.erase(oldest);
            }
            juce::Image image(juce::Image::RGB, frame->width, frame->height, false);
            juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < frame->height; ++y) for (int x = 0; x < frame->width; ++x)
            { const auto* pixel = frame->rgb.data() + (std::size_t(y) * frame->width + x) * 3; bitmap.setPixelColour(x, y, juce::Colour(pixel[0], pixel[1], pixel[2])); }
            found = convertedThumbnails.emplace(frame.get(), Converted{frame, std::move(image), 0}).first;
        }
        found->second.used = ++thumbnailAccess; return found->second.image;
    }
    const auto legacy = thumbnails.find(assetId);
    if (legacy == thumbnails.end() || legacy->second.empty()) return {};
    const auto& images = legacy->second;
    auto it = std::upper_bound(images.begin(), images.end(), sourceSample, [](Sample s, const Thumb& t) { return s < t.sample; });
    if (it != images.begin()) --it; return it->sample == frameSample ? it->image : juce::Image{};
}
void TimelineView::rebuildHeaders()
{
    const auto previous = tracks; tracks.clear(); const auto& p = document.getProject();
    for (auto kind : {TrackKind::cam1, TrackKind::cam2})
    { auto it = std::find_if(p.tracks.begin(), p.tracks.end(), [kind](const auto& t) { return t.kind == kind; }); if (it != p.tracks.end()) tracks.push_back(*it); else { Track t; t.trackId = kind == TrackKind::cam1 ? "placeholder-cam1" : "placeholder-cam2"; t.kind = kind; t.name = ko(kind == TrackKind::cam1 ? "캠1" : "캠2"); tracks.push_back(t); } }
    for (const auto& t : p.tracks) if (t.kind == TrackKind::mic || t.kind == TrackKind::importAudio) tracks.push_back(t);
    if (recordingPreview.active) for (unsigned slot = 0; slot < recordingPreview.microphones.size(); ++slot)
        if (recordingPreview.microphones[slot] && std::none_of(tracks.begin(), tracks.end(), [slot](const Track& t)
            { return t.kind == TrackKind::mic && t.microphoneIndex == int(slot); }))
        {
            Track t; t.trackId = "placeholder-mic-" + juce::String(slot + 1); t.kind = TrackKind::mic; t.microphoneIndex = int(slot);
            t.name = recordingPreview.microphoneLabels[slot]; tracks.push_back(std::move(t));
        }
    visibleIndex.rebuild(p, tracks);
    takeForAsset.clear(); assetById.clear();
    for (const auto& asset : p.media->assets) assetById.emplace(asset.assetId, &asset);
    for (const auto& take : p.media->takes)
    {
        takeForAsset.emplace(take.cam1AssetId, &take); takeForAsset.emplace(take.cam2AssetId, &take);
        for (const auto& id : take.microphoneAssetIds) takeForAsset.emplace(id, &take);
    }
    const bool rebuild = previous.size() != tracks.size() || !std::equal(previous.begin(), previous.end(), tracks.begin(), [](const Track& a, const Track& b) { return a.trackId == b.trackId; });
    if (rebuild) headers.clear();
    for (unsigned i = 0; i < tracks.size(); ++i)
    {
        if (rebuild)
        {
            auto h = std::make_unique<TrackHeader>(edits, tracks[i].trackId);
            h->onEdit = [this](const juce::Result& r) { finish(r); }; h->onSelection = [this] { selectionChanged(); };
            rows.addAndMakeVisible(*h); headers.push_back(std::move(h));
        }
        headers[i]->refresh(trackLabel(tracks[i], document.getProject()));
        headers[i]->setEnabled(std::any_of(p.tracks.begin(), p.tracks.end(), [&](const Track& t) { return t.trackId == tracks[i].trackId; }));
    }
    resized();
}
void TimelineView::rebuildPreview()
{
    const auto* preview = edits.dragPreview() ? edits.dragPreview() : orderPreview.get();
    const auto& project = document.getProject();
    if (!preview) { previewIndex.rebuild(project, {}); return; }
    auto ghosts = tracks;
    std::map<Id, std::size_t> rowFor;
    for (std::size_t row = 0; row < ghosts.size(); ++row)
    { rowFor.emplace(ghosts[row].trackId, row); ghosts[row].clips = {}; }
    const auto add = [&](Clip c)
    {
        const auto row = rowFor.find(c.trackId); if (row == rowFor.end()) return;
        // Rejected edits still need a red ghost. Its geometry is display-only:
        // intersect it with the representable timeline before the Clip index
        // calls timelineEnd(). Never change the rejected edit/commit result.
        constexpr auto maximum = (std::numeric_limits<Sample>::max)();
        const auto end = previewAdd(c.timelineStartSample, (std::max)(Sample{1}, c.lengthSamples));
        c.timelineStartSample = std::clamp(c.timelineStartSample, Sample{0}, maximum - 1);
        c.lengthSamples = std::clamp(previewSubtract(end, c.timelineStartSample), Sample{1}, maximum - c.timelineStartSample);
        ghosts[row->second].clips.edit().push_back(std::move(c));
    };
    if (preview->status.wasOk())
    {
        for (const auto& track : preview->project.tracks) for (const auto& c : track.clips.items())
            if (const auto* original = visibleIndex.find(c.clipId); original
                && (c.timelineStartSample != original->timelineStartSample || c.lengthSamples != original->lengthSamples
                    || (edits.dragPreview() && std::find(edits.dragTargets().begin(), edits.dragTargets().end(), c.clipId) != edits.dragTargets().end()))) add(c);
    }
    else
    {
        const bool drag = edits.dragPreview() != nullptr;
        const auto ids = drag ? edits.dragTargets() : edits.targets();
        const auto* reference = ids.empty() ? nullptr : visibleIndex.find(ids.front());
        if (reference) for (const auto& id : ids) if (const auto* original = visibleIndex.find(id))
        {
            auto c = *original;
            if (drag && edits.dragAction() == TimelineAction::move) c.timelineStartSample = previewAdd(c.timelineStartSample, previewSubtract(edits.dragValue(), reference->timelineStartSample));
            else if (drag && edits.dragAction() == TimelineAction::trimIn)
            { const auto delta = previewSubtract(edits.dragValue(), reference->timelineStartSample); c.timelineStartSample = previewAdd(c.timelineStartSample, delta); c.lengthSamples = previewSubtract(c.lengthSamples, delta); }
            else if (drag) c.lengthSamples = previewAdd(c.lengthSamples, previewSubtract(edits.dragValue(), reference->timelineEnd()));
            add(std::move(c));
        }
    }
    previewIndex.rebuild(project, ghosts);
}
void TimelineView::refresh(bool isLocked, Sample at, const juce::String& status)
{
    if ((isLocked || document.isRecordingStructureLocked()) && rows.dragging != Rows::Drag::none) rows.cancelGesture(false);
    if (rows.dragging == Rows::Drag::scrub) at = playhead;
    edits.setLocked(isLocked); edits.reconcileSelection(); edits.followPlayhead(at);
    const auto snapshot = document.snapshot(); const auto changed = shown != snapshot || locked != edits.isLocked();
    shown = snapshot; locked = edits.isLocked(); playhead = at; latestStatus = status;
    progressiveThumbnails.setRecording(locked);
    if (changed) { orderPreview.reset(); rebuildHeaders(); rebuildPreview(); updateRange(); }
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
    if (changed) updateControls(); rows.repaint();
}
void TimelineView::updateControls()
{
    for (auto& item : buttons)
    {
        auto& b = *item.second; const auto a = item.first; b.setEnabled(edits.enabled(a));
        juce::String hint = TimelineEditController::text(a);
        if (a == TimelineAction::undo || a == TimelineAction::redo) { hint = edits.historyText(a == TimelineAction::redo); b.setButtonText(hint); hint += a == TimelineAction::redo ? " · Ctrl+Shift+Z" : " · Ctrl+Z"; }
        if (a == TimelineAction::split) hint += " · " + shortcuts[RecorderCommand::split];
        if (a == TimelineAction::remove) hint += " · Delete";
        if (a == TimelineAction::addMarker) hint += " · " + shortcuts[RecorderCommand::marker];
        if (a == TimelineAction::unlink) hint += ko(" · 직접 클릭한 클립만 연결에서 분리");
        if (a == TimelineAction::earlier || a == TimelineAction::later) hint += ko(" · 인접한 묶음에 삽입 · 가리키면 이동 대상 표시");
        if (edits.isLocked() && a != TimelineAction::addMarker) hint = ko("녹화 중에는 구조 편집을 사용할 수 없습니다.");
        b.setTooltip(hint);
    }
    for (unsigned i = 0; i < headers.size(); ++i) headers[i]->refresh(trackLabel(tracks[i], document.getProject()));
    inspector.refresh(); markerPanel.refresh();
    juce::String info = editStatus.isNotEmpty() ? editStatus : ko("Shift/Ctrl 다중 선택 · 빈 곳 드래그/Shift+눈금 드래그로 구간 선택");
    if (const auto r = edits.selectedRange()) info = ko("선택 구간 [") + juce::String(r->start) + ", " + juce::String(r->start + r->length) + ko(") 샘플 · ") + info;
    if (edits.isLocked()) info = ko("녹화 중 · 구조 편집·스크럽 잠금 · 프리뷰와 마커 추가 가능");
    selectionInfo.setText(info, juce::dontSendNotification); selectionInfo.setTooltip(info);
}
void TimelineView::selectionChanged() { orderPreview.reset(); rebuildPreview(); editStatus.clear(); updateControls(); rows.repaint(); }
void TimelineView::finish(const juce::Result& r, bool playback)
{
    editStatus = r.failed() ? r.getErrorMessage() : juce::String();
    selectionInfo.setColour(juce::Label::textColourId, r.failed() ? Palette::danger : Palette::dimText);
    if (r.wasOk() && playback && onListeningChanged) onListeningChanged();
    refresh(edits.isLocked(), edits.playhead(), latestStatus);
    updateControls(); // Failed/no-op edits can change status without a new snapshot.
}
juce::Result TimelineView::invoke(TimelineAction a, Sample value, bool exact)
{
    const auto revision = document.getProject().editRevision;
    const auto r = edits.execute(a, value, exact); finish(r, a != TimelineAction::addMarker && revision != document.getProject().editRevision); return r;
}
void TimelineView::setRangeFromInputs()
{
    Sample a = 0, b = 0;
    if (!TimelineEditController::parseSample(rangeStart.getText(), a) || !TimelineEditController::parseSample(rangeEnd.getText(), b) || a == b)
    { finish(juce::Result::fail(ko("서로 다른 시작·끝 샘플을 입력하세요.")), false); return; }
    edits.setRange(a, b); selectionChanged();
}
void TimelineView::showRipplePrompt()
{
    const auto prompt = edits.ripplePrompt();
    if (!prompt.conflict) { invoke(TimelineAction::rippleAudio); return; }
    juce::String names;
    for (const auto& t : document.getProject().tracks) if (std::find(prompt.expandedTracks.begin(), prompt.expandedTracks.end(), t.trackId) != prompt.expandedTracks.end()) names += (names.isEmpty() ? "" : ", ") + t.name;
    const auto body = ko("대상 밖 트랙과 연결되어 있습니다: ") + names + "\n"
        + (prompt.requiresAllTracks ? ko("대상 확대: 전 트랙·마커·모든 테이크 버전에 같은 구간을 적용합니다.") : ko("대상 확대: 표시한 오디오 트랙을 함께 당깁니다. 마커는 유지합니다."))
        + ko("\n링크 해제: 선택한 오디오 트랙의 영향받는 클립을 분리한 뒤 당깁니다. 한 번에 실행취소할 수 있습니다.");
    juce::Component::SafePointer<TimelineView> safe(this);
    juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon).withTitle(ko("링크 충돌"))
        .withMessage(body).withButton(ko("대상 확대")).withButton(ko("링크 해제")).withButton(ko("취소")).withAssociatedComponent(this),
        [safe, prompt](int result) { if (safe) safe->finish(safe->edits.resolveRipple(prompt, result == 1 ? RippleChoice::expand : result == 2 ? RippleChoice::unlink : RippleChoice::cancel), result == 1 || result == 2); });
}
juce::PopupMenu TimelineView::createEditMenu() const
{
    juce::PopupMenu menu;
    for (auto a : {TimelineAction::split, TimelineAction::trimIn, TimelineAction::trimOut, TimelineAction::remove, TimelineAction::rippleAll,
                   TimelineAction::rippleAudio, TimelineAction::earlier, TimelineAction::later, TimelineAction::unlink, TimelineAction::link,
                   TimelineAction::undo, TimelineAction::redo, TimelineAction::addMarker})
    {
        auto label = a == TimelineAction::undo || a == TimelineAction::redo ? edits.historyText(a == TimelineAction::redo) : TimelineEditController::text(a);
        if (a == TimelineAction::rippleAll) label += ko(" · 전 트랙");
        if (a == TimelineAction::rippleAudio) label += ko(" · 선택 오디오 트랙");
        if (a == TimelineAction::split) label += " (" + shortcuts[RecorderCommand::split] + ")";
        if (a == TimelineAction::remove) label += " (Delete)";
        if (a == TimelineAction::addMarker) label += " (" + shortcuts[RecorderCommand::marker] + ")";
        if (a == TimelineAction::undo) label += " (Ctrl+Z)";
        if (a == TimelineAction::redo) label += " (Ctrl+Shift+Z)";
        menu.addItem(int(a) + 1, label, edits.enabled(a));
    }
    return menu;
}
void TimelineView::showEditMenu(bool atMouse)
{
    auto menu = createEditMenu();
    juce::Component::SafePointer<TimelineView> safe(this);
    const auto base = document.snapshot(); const auto selection = document.getSelection();
    auto options = juce::PopupMenu::Options().withTargetComponent(&menuButton);
    if (atMouse) // a right-click opens where the mouse is, not next to the toolbar button
    { const auto p = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt(); options = juce::PopupMenu::Options().withTargetScreenArea({p.x, p.y, 1, 1}); }
    menu.showMenuAsync(options, [safe, base, selection](int result)
    {
        if (!safe || result == 0) return;
        if (safe->document.snapshot() != base || safe->document.getSelection() != selection) { safe->finish(juce::Result::fail(ko("편집 대상이 바뀌었습니다. 메뉴를 다시 여세요.")), false); return; }
        const auto a = TimelineAction(result - 1);
        if (a == TimelineAction::rippleAudio) safe->showRipplePrompt();
        else if (a == TimelineAction::addMarker && safe->onAddMarkerRequested) safe->onAddMarkerRequested();
        else safe->invoke(a, safe->edits.playhead());
    });
}
bool TimelineView::keyPressed(const juce::KeyPress& key, juce::Component* origin)
{
    if (onGlobalKey && onGlobalKey(key, origin)) return true;
    if (!isShowing() || dynamic_cast<juce::TextEditor*>(origin) || (origin && origin->findParentComponentOfClass<juce::TextEditor>())) return false;
    const auto code = key.getKeyCode();
    if (key.getModifiers().isCtrlDown() && (code == 'Z' || code == 'z')) { invoke(key.getModifiers().isShiftDown() ? TimelineAction::redo : TimelineAction::undo); return true; }
    if (key.getModifiers().isCtrlDown() || key.getModifiers().isAltDown()) return false;
    if (!onGlobalKey) if (const auto command = shortcutCommand(shortcuts, key, origin))
    { if (*command == RecorderCommand::split) { invoke(TimelineAction::split); return true; } if (*command == RecorderCommand::marker) { if (onAddMarkerRequested) onAddMarkerRequested(); else invoke(TimelineAction::addMarker); return true; } }
    if (code == juce::KeyPress::deleteKey) { invoke(TimelineAction::remove); return true; }
    if (code == juce::KeyPress::escapeKey) { rows.cancelGesture(); selectionChanged(); return true; }
    return false;
}
double TimelineView::xFor(Sample s) const { return headerWidth + (double(s) / document.getProject().Fs - viewStart) / viewSeconds * juce::jmax(1, rows.getWidth() - headerWidth); }
Sample TimelineView::sampleFor(double x) const
{
    const auto sample = (viewStart + (x - headerWidth) * viewSeconds / juce::jmax(1, rows.getWidth() - headerWidth)) * document.getProject().Fs;
    // Hit/paint queries add one for their half-open end. Reserve that sample,
    // and reject the rounded 2^63 boundary before calling llround.
    return TimelineSamples::roundNonnegative(sample, (std::numeric_limits<Sample>::max)() - 1);
}
void TimelineView::updateRange()
{
    auto end = juce::jmax(playhead, visibleIndex.timelineEnd());
    for (const auto& m : document.getProject().markers) end = juce::jmax(end, m.sample);
    const auto duration = std::max({viewSeconds, double(end) / document.getProject().Fs + 2,
                                   rows.dragging != Rows::Drag::none || recordingPreview.active ? viewStart + viewSeconds : 0.0});
    viewStart = juce::jlimit(0.0, juce::jmax(0.0, duration - viewSeconds), viewStart);
    horizontal.setRangeLimits(0, duration, juce::dontSendNotification); horizontal.setCurrentRange(viewStart, viewSeconds, juce::dontSendNotification);
}
void TimelineView::zoom(double factor)
{
    zoomAt(factor, juce::jlimit(double(headerWidth), double(rows.getWidth()), xFor(playhead)));
}
void TimelineView::zoomAt(double factor, double rowX)
{
    const auto fraction = juce::jlimit(0.0, 1.0, (rowX - headerWidth) / juce::jmax(1, rows.getWidth() - headerWidth));
    const auto nextSeconds = juce::jlimit(.1, 10800.0, viewSeconds * factor);
    viewStart = TimelineInteraction::zoomStart(viewStart, viewSeconds, nextSeconds, fraction); viewSeconds = nextSeconds;
    updateRange(); rows.repaint();
}
void TimelineView::zoomToFit() { viewStart = 0; viewSeconds = juce::jmax(5.0, double(document.getProject().activeTimelineEnd()) / document.getProject().Fs + 1); updateRange(); rows.repaint(); }
void TimelineView::reveal(Sample at) { viewStart = TimelineInteraction::revealStart(viewStart, viewSeconds, double(at) / document.getProject().Fs); updateRange(); rows.repaint(); }
void TimelineView::scrollBarMoved(juce::ScrollBar*, double start) { viewStart = start; recordingPreview.follow = false; rows.repaint(); }
void TimelineView::setRecordingPreview(bool active, Sample placement, Sample elapsed, const RecordingPreviewTargets& targets, const UserSettings& settings)
{
    const auto previous = recordingPreview;
    if (active && !recordingPreview.active)
    {
        recordingPreview.follow = true;
        const auto start = double(placement) / document.getProject().Fs;
        if (start < viewStart || start > viewStart + viewSeconds * .75) viewStart = std::max(0.0, start - viewSeconds * .25);
    }
    recordingPreview.active = active; recordingPreview.start = std::max(Sample{0}, placement); recordingPreview.length = std::max(Sample{0}, elapsed);
    if (active && recordingPreview.follow)
    {
        const auto end = double(previewAdd(recordingPreview.start, recordingPreview.length)) / document.getProject().Fs;
        if (end > viewStart + viewSeconds * .9) viewStart = end - viewSeconds * .9;
        updateRange();
    }
    recordingPreview.cameras = targets.cameras;
    recordingPreview.microphones.fill(false);
    for (const auto logical : targets.microphones) if (logical >= 1 && logical <= recordingPreview.microphones.size())
        recordingPreview.microphones[logical - 1] = true;
    for (std::size_t i = 0; i < recordingPreview.microphones.size(); ++i)
        recordingPreview.microphoneLabels[i] = (settings.microphoneNames[i].isEmpty() ? ko("마이크 ") + juce::String(i + 1) : settings.microphoneNames[i])
            + (settings.stereoSlots[i] ? ko(" · 스테레오") : juce::String());
    // Only labels use settings. Membership is the session's actual capture set.
    // Missing microphone lanes are display-only; the take controller publishes tracks/clips.
    if (previous.active != active || previous.microphones != recordingPreview.microphones || previous.microphoneLabels != recordingPreview.microphoneLabels)
    { rebuildHeaders(); rebuildPreview(); updateRange(); }
    rows.repaint();
}
bool TimelineView::isRecordingTrack(const Track& track) const
{
    return recordingPreview.active && (track.kind == TrackKind::cam1 ? recordingPreview.cameras[0] : track.kind == TrackKind::cam2 ? recordingPreview.cameras[1]
        : track.kind == TrackKind::mic && track.microphoneIndex >= 0 && track.microphoneIndex < 8 && recordingPreview.microphones[std::size_t(track.microphoneIndex)]);
}
void TimelineView::resized()
{
    auto a = getLocalBounds(); transport.setBounds(a.removeFromTop(38)); snapButton.setBounds(transport.getBounds().removeFromRight(125).reduced(0, 2));
    const bool compact = getHeight() < 210;
    auto side = compact ? a.removeFromRight(270) : juce::Rectangle<int>();
    toolbarViewport.setBounds(compact ? side.removeFromTop(46) : a.removeFromTop(46));
    int x = 0; menuButton.setBounds(x, 1, 62, 30); x += 66;
    for (auto& item : buttons)
    { const int w = item.first == TimelineAction::rippleAll ? 170 : item.first == TimelineAction::link ? 126 : item.first == TimelineAction::undo || item.first == TimelineAction::redo ? 220 : 86; item.second->setBounds(x, 1, w, 30); x += w + 4; }
    rangeStart.setBounds(x, 1, 136, 30); rangeEnd.setBounds(x + 140, 1, 136, 30); rangeButton.setBounds(x + 280, 1, 86, 30); toolbar.setSize(x + 370, 32);
    selectionInfo.setBounds(compact ? side.removeFromTop(24) : a.removeFromBottom(24)); sidebar.setBounds(compact ? side : a.removeFromRight(270)); a.removeFromRight(6);
    horizontal.setBounds(a.removeFromBottom(14).withTrimmedLeft(headerWidth)); viewport.setBounds(a);
    rows.setSize(juce::jmax(1, a.getWidth() - 16), juce::jmax(a.getHeight(), rulerHeight + int(tracks.size()) * rowHeight));
    for (unsigned i = 0; i < headers.size(); ++i) headers[i]->setBounds(0, rulerHeight + int(i) * rowHeight, headerWidth - 4, rowHeight - 2);
}
void TimelineView::drawWave(juce::Graphics& g, const Clip& clip, juce::Rectangle<float> box)
{
    const auto found = peaks.find(clip.assetId); if (found == peaks.end() || !found->second.data) return;
    const auto& s = *found->second.data; auto ch = found->second.channel;
    const auto& media = *document.getProject().media; const auto* asset = media.findAsset(clip.assetId);
    const auto width = asset && asset->originalFormat.channels == 2 ? 2u : 1u;
    if (asset && asset->kind == AssetKind::mic)
        for (const auto& take : media.takes)
        {
            unsigned offset = 0;
            for (const auto& id : take.microphoneAssetIds)
            {
                if (id == clip.assetId) { ch = offset; break; }
                if (const auto* preceding = media.findAsset(id)) offset += unsigned(preceding->originalFormat.channels);
            }
        }
    if (ch >= s.channels || s.bins.empty() || !s.samplesPerBin) return;
    const auto bounds = g.getClipBounds();
    const auto [left, right] = TimelineLayout::waveColumns(bounds.getX(), bounds.getRight(),
        int(std::clamp(double(box.getX()), double(bounds.getX()), double(bounds.getRight()))),
        int(std::clamp(std::ceil(double(box.getRight())), double(bounds.getX()), double(bounds.getRight()))));
    lastPaintWaveColumns += std::size_t(right - left);
    g.setColour(Palette::meterGreen); const auto middle = box.getCentreY();
    for (int x = left; x < right; ++x)
    {
        const auto start = TimelineSamples::sourceAt(clip, sampleFor(x)), end = TimelineSamples::sourceAt(clip, sampleFor(x + 1));
        if (!start || !end) continue;
        const auto first = juce::jmax(Sample{0}, *start);
        const auto onePast = TimelineSamples::add(first, 1); if (!onePast) continue;
        const auto last = juce::jmax(*onePast, *end);
        const auto a = std::uint64_t(first) / s.samplesPerBin, b = juce::jmin<std::uint64_t>(s.bins.size(),
            std::uint64_t(last) / s.samplesPerBin + (std::uint64_t(last) % s.samplesPerBin != 0));
        if (a >= b) continue; auto low = s.bins[std::size_t(a)][ch].minimum, high = s.bins[std::size_t(a)][ch].maximum;
        for (auto i = a; i < b; ++i) for (unsigned channel = ch; channel < juce::jmin(ch + width, s.channels); ++channel)
        { low = juce::jmin(low, s.bins[std::size_t(i)][channel].minimum); high = juce::jmax(high, s.bins[std::size_t(i)][channel].maximum); }
        g.drawVerticalLine(x, middle - high * box.getHeight() * .47f, juce::jmax(middle - high * box.getHeight() * .47f + 1, middle - low * box.getHeight() * .47f));
    }
}
void TimelineView::Rows::paint(juce::Graphics& g)
{
    ++view.rowPaintCount;
    view.lastPaintVisitedClips = view.lastPaintWaveColumns = 0;
    auto& v = view; const auto& p = v.document.getProject(); g.fillAll(Palette::card);
    if (getWidth() <= headerWidth) return;
    const auto clipBounds = g.getClipBounds(); auto step = v.viewSeconds <= 2 ? .1 : v.viewSeconds <= 10 ? 1.0 : v.viewSeconds <= 60 ? 5.0 : v.viewSeconds <= 300 ? 30.0 : 60.0;
    const auto maxTicks = std::max(1, (getWidth() - headerWidth) / 80);
    if (v.viewSeconds / step > maxTicks) step = std::pow(10.0, std::ceil(std::log10(v.viewSeconds / maxTicks)));
    g.setFont(juce::Font(juce::FontOptions(14))); g.setColour(Palette::dimText);
    const auto firstTick = std::ceil(v.viewStart / step) * step;
    for (int tick = 0; tick <= maxTicks + 1; ++tick)
    {
        const auto t = firstTick + tick * step, sample = t * p.Fs;
        if (!std::isfinite(sample) || sample >= double((std::numeric_limits<Sample>::max)()) || t > v.viewStart + v.viewSeconds) break;
        const auto at = TimelineSamples::roundNonnegative(sample); const auto pixel = v.xFor(at);
        if (pixel < headerWidth || pixel >= getWidth()) continue;
        const auto x = int(pixel); g.drawVerticalLine(x, 22, float(getHeight()));
        g.drawText(formatRecorderTime(at, p.Fs).substring(3, 8), x + 3, 0, 64, 22, juce::Justification::centredLeft);
    }
    // Bound raster geometry before JUCE converts floating coordinates to pixels.
    const auto paintX = [&](Sample at) { return float(std::clamp(v.xFor(at), double(headerWidth - 8), double(getWidth() + 8))); };
    if (const auto range = v.edits.selectedRange())
    {
        const juce::Graphics::ScopedSaveState save(g); g.reduceClipRegion(headerWidth, 0, getWidth() - headerWidth, getHeight());
        const auto left = paintX(range->start), right = paintX(previewAdd(range->start, range->length));
        g.setColour(Palette::accent.withAlpha(.17f)); g.fillRect(left, 0.0f, std::max(0.0f, right - left), float(getHeight()));
    }
    for (const auto& m : p.markers)
    {
        const auto pixel = v.xFor(m.sample); if (pixel < headerWidth || pixel >= getWidth()) continue; const int x = int(pixel);
        g.setColour(juce::Colour::fromString("ff" + m.colour.substring(1))); g.fillRect(x, 17, 3, 13);
        g.drawText(m.name, x + 4, 16, 90, 14, juce::Justification::centredLeft, true);
    }
    const auto [firstRow, lastRow] = TimelineLayout::visibleRows(clipBounds.getY(), clipBounds.getBottom(), int(v.tracks.size()));
    for (int row = firstRow; row < lastRow; ++row)
    {
        const auto y = rulerHeight + int(row) * rowHeight; if (y > clipBounds.getBottom() || y + rowHeight < clipBounds.getY()) continue;
        g.setColour(Palette::line); g.drawHorizontalLine(y + rowHeight - 1, 0, float(getWidth()));
        const auto visible = v.visibleIndex.visible(std::size_t(row), v.sampleFor(std::max(headerWidth, clipBounds.getX())), v.sampleFor(clipBounds.getRight()) + 1);
        v.lastPaintVisitedClips += visible.size();
        for (const auto* item : visible)
        {
            const auto& c = *item;
            const auto left = paintX(c.timelineStartSample), right = paintX(c.timelineEnd()); if (right <= headerWidth || left >= getWidth()) continue;
            auto box = juce::Rectangle<float>(left, float(y + 3), juce::jmax(2.0f, right - left), float(rowHeight - 6));
            const juce::Graphics::ScopedSaveState save(g); g.reduceClipRegion(headerWidth, y, getWidth() - headerWidth, rowHeight);
            const bool selected = std::find(v.document.getSelection().begin(), v.document.getSelection().end(), c.clipId) != v.document.getSelection().end();
            g.setColour(Palette::card2); g.fillRoundedRectangle(box, 5);
            const auto lookup = v.takeForAsset.find(c.assetId); const Take* take = lookup == v.takeForAsset.end() ? nullptr : lookup->second;
            auto state = take ? take->state == TakeState::complete ? ko("완료") : take->state == TakeState::partial ? ko("확인 필요") : ko("마무리 중") : juce::String();
            if (take && !p.media->takes.empty() && take->takeId == p.media->takes.back().takeId && v.latestStatus.isNotEmpty()) state = v.latestStatus;
            const auto imageBox = box.withTrimmedTop(24).reduced(2);
            const bool video = v.tracks[std::size_t(row)].kind == TrackKind::cam1 || v.tracks[std::size_t(row)].kind == TrackKind::cam2;
            if (video)
            {
                for (float x = juce::jmax(float(headerWidth), imageBox.getX()); x < juce::jmin(float(getWidth()), imageBox.getRight()); x += 72)
                {
                    const auto sample = TimelineSamples::sourceAt(c, v.sampleFor(x)); if (!sample) continue;
                    const auto image = v.thumbnailFor(c.assetId, *sample);
                    if (image.isValid()) g.drawImage(image, juce::Rectangle<float>(x, imageBox.getY(), 70.0f, imageBox.getHeight()), juce::RectanglePlacement::stretchToFit);
                }
            }
            else v.drawWave(g, c, imageBox);
            g.setColour(Palette::text); g.setFont(juce::Font(juce::FontOptions(14)));
            auto labelBounds = box.withHeight(24).withTrimmedLeft(c.linkGroupId.isEmpty() ? 6.0f : 28.0f);
            g.drawText((take ? take->name : ko("클립")) + " · " + state, labelBounds, juce::Justification::centredLeft, true);
            if (c.linkGroupId.isNotEmpty()) { g.setColour(Palette::accent); g.drawRoundedRectangle(box.getX() + 6, box.getY() + 8, 12, 7, 3, 1.5f); g.drawRoundedRectangle(box.getX() + 12, box.getY() + 10, 12, 7, 3, 1.5f); }
            g.setColour(selected ? Palette::accent : Palette::line); g.drawRoundedRectangle(box, 5, selected ? 2.0f : 1.0f);
            if (v.edits.dragPreview() && std::find(v.edits.dragTargets().begin(), v.edits.dragTargets().end(), c.clipId) != v.edits.dragTargets().end())
            { g.setColour(Palette::background.withAlpha(.5f)); g.fillRoundedRectangle(box, 5); }
            if (selected)
            {
                const auto assetEntry = v.assetById.find(c.assetId); const auto* asset = assetEntry == v.assetById.end() ? nullptr : assetEntry->second;
                const auto sourceEnd = TimelineSamples::add(c.sourceIn, c.lengthSamples);
                const bool leftLimit = c.sourceIn == 0, rightLimit = asset && sourceEnd && *sourceEnd == asset->logicalLength;
                g.setColour(leftLimit ? Palette::meterYellow : Palette::accent); g.fillRect(box.getX() + 2, box.getY() + 25, 4.0f, box.getHeight() - 29);
                g.setColour(rightLimit ? Palette::meterYellow : Palette::accent); g.fillRect(box.getRight() - 6, box.getY() + 25, 4.0f, box.getHeight() - 29);
                if (v.edits.focusedClip() == c.clipId) { g.setColour(Palette::text); g.fillEllipse(box.getX() + 5, box.getY() + 3, 4, 4); }
            }
            if (take && !p.media->takes.empty() && take->takeId == p.media->takes.back().takeId
                && (v.lastPaintedTake != take->takeId || !v.lastClipPaintQpc)) { v.lastPaintedTake = take->takeId; v.lastClipPaintQpc = qpcNow(); }
        }
    }
    if (const auto* preview = v.edits.dragPreview() ? v.edits.dragPreview() : v.orderPreview.get())
    {
        const juce::Graphics::ScopedSaveState save(g); g.reduceClipRegion(headerWidth, rulerHeight, getWidth() - headerWidth, getHeight() - rulerHeight);
        const bool drag = v.edits.dragPreview() != nullptr;
        for (int row = firstRow; row < lastRow; ++row)
        {
          const auto visible = v.previewIndex.visible(std::size_t(row), v.sampleFor(std::max(headerWidth, clipBounds.getX())), v.sampleFor(clipBounds.getRight()) + 1);
          v.lastPaintVisitedClips += visible.size();
          for (const auto* item : visible)
          {
            const auto& c = *item;
            const auto left = paintX(c.timelineStartSample), right = paintX(previewAdd(c.timelineStartSample, std::max(Sample{0}, c.lengthSamples)));
            if (right <= std::max(headerWidth, clipBounds.getX()) || left >= clipBounds.getRight()) continue;
            const auto box = juce::Rectangle<float>(left, float(rulerHeight + int(row) * rowHeight + 3), std::max(3.0f, right - left), float(rowHeight - 6));
            g.setColour((preview->status.failed() ? Palette::danger : Palette::accent).withAlpha(.23f)); g.fillRect(box);
            g.setColour(preview->status.failed() ? Palette::danger : Palette::accent); g.drawRect(box, 3.0f);
            if (drag)
            {
                g.drawText(v.edits.dragTargets().size() > 1 ? ko("함께 이동 · ") + juce::String(int(v.edits.dragTargets().size())) + ko("개") : TimelineEditController::text(v.edits.dragAction()),
                           box.reduced(7, 2).withHeight(22), juce::Justification::centredLeft, true);
                const auto edge = v.edits.dragAction() == TimelineAction::trimOut ? right : left;
                g.drawLine(edge, box.getY(), edge, box.getBottom(), 3.0f);
            }
            if (drag && v.edits.dragAction() != TimelineAction::move)
            {
                const auto* original = v.visibleIndex.find(c.clipId); const auto asset = v.assetById.find(c.assetId);
                if (!original || asset == v.assetById.end()) continue;
                const auto first = TimelineSamples::subtract(original->timelineStartSample, original->sourceIn);
                const auto last = first ? TimelineSamples::add(*first, asset->second->logicalLength) : std::nullopt;
                for (const auto limit : {first, last}) if (limit)
                {
                    const auto x = v.xFor(*limit); if (x < headerWidth || x >= getWidth()) continue;
                    g.setColour(Palette::meterYellow); g.drawLine(float(x), box.getY(), float(x), box.getBottom(), 2.0f);
                }
            }
          }
        }
    }
    if (v.recordingPreview.active)
    {
        const juce::Graphics::ScopedSaveState save(g); g.reduceClipRegion(headerWidth, rulerHeight, getWidth() - headerWidth, getHeight() - rulerHeight);
        const auto& recording = v.recordingPreview;
        for (int row = firstRow; row < lastRow; ++row)
        {
            const auto& track = v.tracks[std::size_t(row)];
            if (!v.isRecordingTrack(track)) continue;
            const auto left = paintX(recording.start), right = paintX(previewAdd(recording.start, recording.length));
            if (right <= headerWidth || left >= getWidth()) continue;
            const auto box = juce::Rectangle<float>(left, float(rulerHeight + row * rowHeight + 3), std::max(4.0f, right - left), float(rowHeight - 6));
            g.setColour(Palette::danger.withAlpha(.23f)); g.fillRoundedRectangle(box, 5);
            g.setColour(Palette::danger); g.drawRoundedRectangle(box, 5, 2); g.drawVerticalLine(int(right), box.getY(), box.getBottom());
            g.setColour(Palette::text); g.drawText(ko("● 녹화 중 · ") + formatRecorderTime(recording.length, p.Fs), box.reduced(8, 2).withHeight(24), juce::Justification::centredLeft, true);
        }
    }
    if (snapGuide)
    {
        const auto x = float(v.xFor(*snapGuide));
        if (x >= headerWidth && x < getWidth()) { g.setColour(Palette::meterYellow); g.drawLine(x, 0, x, float(getHeight()), 2); g.drawText(ko("스냅"), int(x) + 4, 0, 45, 19, juce::Justification::centredLeft); }
    }
    const auto pixel = v.xFor(v.playhead); if (pixel >= headerWidth && pixel < getWidth()) { const auto x = int(pixel); g.setColour(Palette::danger); g.drawVerticalLine(x, 0, float(getHeight())); g.fillEllipse(float(x - 4), 0, 8, 8); }
    if (dragging == Drag::scrub) v.drawScrubPreview(g);
}
void TimelineView::drawScrubPreview(juce::Graphics& g)
{
    // Immediate cached feedback for both sources while precise seeks are throttled.
    const auto boxes = TimelineLayout::cameras((std::min)(rows.getWidth() - headerWidth, 336), 90);
    for (unsigned camera = 0; camera < 2; ++camera)
    {
        const auto box = boxes[camera]; auto bounds = juce::Rectangle<int>(headerWidth + box.x, viewport.getViewPositionY() + rulerHeight + box.y, box.width, box.height);
        g.setColour(Palette::background); g.fillRect(bounds);
        const auto clips = visibleIndex.visible(camera, playhead, previewAdd(playhead, 1));
        juce::Image image;
        if (clips.size()) { const auto& c = **clips.begin(); if (const auto sample = TimelineSamples::sourceAt(c, playhead)) image = thumbnailFor(c.assetId, *sample, true); }
        if (image.isValid()) g.drawImage(image, bounds.toFloat(), juce::RectanglePlacement::centred);
        else { g.setColour(Palette::dimText); g.drawText(clips.size() ? ko("썸네일 준비 중") : ko("영상 없음"), bounds, juce::Justification::centred); }
    }
}
void TimelineView::Rows::mouseDown(const juce::MouseEvent& e)
{
    auto& v = view; grabKeyboardFocus(); if (e.x < headerWidth) return;
    cancelGesture(false); previousPlayhead = v.playhead; previousRange = v.edits.selectedRange(); previousViewStart = v.viewStart;
    lastPointer = e.position; lastModifiers = e.mods;
    dragging = Drag::none; moved = false; collapseSelection = false; downSample = v.sampleFor(e.x); downClip.clear();
    if (e.y < rulerHeight)
    {
        if (v.edits.isLocked()) return;
        dragging = e.mods.isShiftDown() ? Drag::range : Drag::scrub;
        if (dragging == Drag::scrub) { v.edits.clearRange(); v.playhead = downSample; v.edits.followPlayhead(downSample); if (v.onScrub) v.onScrub(downSample, false); }
        repaint(); return;
    }
    const auto row = (e.y - rulerHeight) / rowHeight; if (row < 0 || row >= int(v.tracks.size())) return;
    downRow = row;
    // A trim handle can be grabbed on either side of its boundary, including the
    // half-open right edge where the source clip no longer contains the sample.
    const auto nearbyClips = v.visibleIndex.visible(std::size_t(row), v.sampleFor(e.x - 8), v.sampleFor(e.x + 8) + 1);
    for (const auto* item : nearbyClips) { const auto& c = *item; if (
        std::find(v.document.getSelection().begin(), v.document.getSelection().end(), c.clipId) != v.document.getSelection().end()
        && (std::abs(e.x - v.xFor(c.timelineStartSample)) <= 7 || std::abs(e.x - v.xFor(c.timelineEnd())) <= 7))
    { downClip = c.clipId; break; } }
    for (const auto* item : v.visibleIndex.visible(std::size_t(row), downSample, downSample + 1))
    {
        const auto& c = *item;
        if (downClip.isEmpty()) downClip = c.clipId; break;
    }
    if (downClip.isEmpty())
    {
        if (!e.mods.isShiftDown() && !e.mods.isCtrlDown()) v.edits.clearSelection(); v.edits.clearRange();
        if (!v.edits.isLocked()) dragging = Drag::range;
        if (e.mods.isPopupMenu()) { dragging = Drag::none; v.showEditMenu(true); } v.selectionChanged(); return;
    }
    const auto& explicitIds = v.edits.explicitSelection();
    if (!e.mods.isShiftDown() && !e.mods.isCtrlDown() && std::find(explicitIds.begin(), explicitIds.end(), downClip) != explicitIds.end())
    { v.edits.focusSelection(downClip); collapseSelection = !e.mods.isPopupMenu() && explicitIds.size() > 1; }
    else v.edits.clickClip(downClip, e.mods.isShiftDown(), e.mods.isCtrlDown());
    v.selectionChanged();
    downSnapshot = v.document.snapshot(); downRevision = downSnapshot->editRevision;
    downSelection = v.document.getSelection(); downExplicitSelection = v.edits.explicitSelection(); downTargets = v.edits.targets();
    const auto* c = downSnapshot->findClip(downClip);
    // The document can change before the next 33 ms refresh (e.g. a take/version
    // publication). A hit from the displayed index is not proof of membership.
    if (!c || !downSnapshot->isActive(*c)) { cancelGesture(false); return; }
    downStart = c->timelineStartSample; downEnd = c->timelineEnd();
    auto action = TimelineAction::move;
    if (std::abs(e.x - v.xFor(c->timelineStartSample)) <= 7 && std::abs(e.x - v.xFor(c->timelineStartSample)) <= std::abs(e.x - v.xFor(c->timelineEnd()))) action = TimelineAction::trimIn;
    else if (std::abs(e.x - v.xFor(c->timelineEnd())) <= 7) action = TimelineAction::trimOut;
    downEdge = action == TimelineAction::trimOut ? c->timelineEnd() : c->timelineStartSample;
    pendingAction = action;
    if (e.mods.isPopupMenu()) { v.showEditMenu(true); return; }
    if (v.edits.isLocked() || e.mods.isCtrlDown() || e.mods.isShiftDown()) return;
    dragging = Drag::clip;
}
bool TimelineView::Rows::pendingClipUnchanged() const
{
    const auto current = view.document.snapshot();
    if (!downSnapshot || current != downSnapshot || current->editRevision != downRevision
        || view.document.getSelection() != downSelection || view.edits.explicitSelection() != downExplicitSelection
        || view.edits.targets() != downTargets) return false;
    const auto* clip = current->findClip(downClip);
    return clip && current->isActive(*clip) && clip->timelineStartSample == downStart && clip->timelineEnd() == downEnd
        && (pendingAction == TimelineAction::trimOut ? clip->timelineEnd() : clip->timelineStartSample) == downEdge;
}
void TimelineView::Rows::mouseDrag(const juce::MouseEvent& e)
{
    auto& v = view; if (dragging == Drag::none || v.edits.isLocked()) return;
    lastPointer = e.position; lastModifiers = e.mods;
    if (!moved && e.getDistanceFromDragStart() >= TimelineInteraction::dragThreshold)
    {
        if (dragging == Drag::clip && (!pendingClipUnchanged() || !v.edits.beginDrag(pendingAction))) { cancelGesture(false); return; }
        moved = true;
        snapIndex.build(v.document.getProject(), dragging == Drag::clip ? v.edits.dragTargets() : std::vector<Id>{}, previousPlayhead, pendingAction);
        lastAutoScroll = juce::Time::getMillisecondCounter(); startTimer(16);
        setMouseCursor(dragging == Drag::clip ? pendingAction == TimelineAction::move ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::CrosshairCursor);
    }
    if (moved) updateDrag();
}
void TimelineView::Rows::updateDrag()
{
    auto& v = view; if (v.edits.isLocked() || !moved) return;
    const auto bypass = lastModifiers.isAltDown() || !v.snapButton.getToggleState();
    const auto tolerance = TimelineInteraction::snapTolerance(v.viewSeconds, v.document.getProject().Fs, getWidth() - headerWidth, v.document.getProject().fps);
    const auto raw = dragging == Drag::clip ? previewAdd(downEdge, previewSubtract(v.sampleFor(lastPointer.x), downSample)) : v.sampleFor(lastPointer.x); // saturated: never overflows before the range checks
    const auto snapped = snapIndex.snap(std::max(Sample{0}, raw), tolerance, bypass); snapGuide = snapped.guide;
    if (dragging == Drag::scrub) { v.playhead = snapped.value; v.edits.followPlayhead(v.playhead); if (v.onScrub) v.onScrub(v.playhead, false); }
    else if (dragging == Drag::range) { v.edits.setRange(downSample, snapped.value); v.updateControls(); }
    else if (dragging == Drag::clip)
    {
        const auto* candidate = v.edits.dragTo(snapped.value, bypass || snapped.guide.has_value());
        const auto joined = bypass ? std::nullopt : v.edits.dragNeighbourGuide();
        if (joined) snapGuide = joined;
        v.rebuildPreview();
        if (candidate)
        {
            const bool otherRow = (int(lastPointer.y) - rulerHeight) / rowHeight != downRow;
            v.editStatus = candidate->status.failed() ? candidate->status.getErrorMessage()
                : (otherRow ? ko("원래 트랙 유지 · ") : juce::String()) + ko("함께 편집 ") + juce::String(int(v.edits.dragTargets().size())) + ko("개 · 놓아서 확정 · Esc 취소")
                  + (bypass ? ko(" · 스냅 해제") : joined ? ko(" · 이웃 클립에 붙임") : snapGuide ? ko(" · 스냅 맞춤") : ko(" · Alt 스냅 해제"));
            v.selectionInfo.setColour(juce::Label::textColourId, candidate->status.failed() ? Palette::danger : Palette::dimText);
            v.selectionInfo.setText(v.editStatus, juce::dontSendNotification);
        }
    }
    v.snapButton.setButtonText(bypass ? ko("스냅 해제") : snapGuide ? ko("스냅 맞춤") : ko("스냅 켜짐"));
    repaint();
}
void TimelineView::Rows::timerCallback()
{
    auto& v = view;
    if (dragging == Drag::none || v.edits.isLocked() || !isShowing()) { cancelGesture(false); return; }
    const auto now = juce::Time::getMillisecondCounter(); const auto elapsed = std::min(.05, double(now - lastAutoScroll) / 1000.0); lastAutoScroll = now;
    const auto delta = TimelineInteraction::edgeScroll(lastPointer.x, headerWidth, getWidth(), v.viewSeconds, elapsed);
    const auto modifiers = juce::ModifierKeys::getCurrentModifiersRealtime(); const auto changed = modifiers != lastModifiers;
    lastModifiers = modifiers;
    if (delta == 0 && !changed) return;
    v.viewStart = std::max(0.0, v.viewStart + delta); v.updateRange();
    updateDrag();
}
void TimelineView::Rows::cancelGesture(bool restore)
{
    stopTimer(); const auto mode = dragging; dragging = Drag::none; snapGuide.reset(); view.edits.cancelDrag();
    if (restore && mode != Drag::none)
    {
        view.viewStart = previousViewStart;
        if (previousRange) view.edits.setRange(previousRange->start, previousRange->start + previousRange->length); else view.edits.clearRange();
        if (mode == Drag::scrub) { view.playhead = previousPlayhead; view.edits.seek(previousPlayhead); }
        view.viewStart = previousViewStart; view.updateRange();
    }
    collapseSelection = false; moved = false;
    downSnapshot.reset(); downSelection.clear(); downExplicitSelection.clear(); downTargets.clear();
    view.snapButton.setButtonText(view.snapButton.getToggleState() ? ko("스냅 켜짐") : ko("스냅 꺼짐"));
    setMouseCursor(juce::MouseCursor::NormalCursor); view.rebuildPreview(); repaint();
}
void TimelineView::Rows::mouseUp(const juce::MouseEvent& e)
{
    auto& v = view; const auto mode = dragging;
    if (mode == Drag::clip && moved && !v.edits.isLocked()) mouseDrag(e);
    stopTimer(); dragging = Drag::none; snapGuide.reset();
    if (mode == Drag::clip && moved)
    {
        const auto joined = !lastModifiers.isAltDown() && v.snapButton.getToggleState() && v.edits.dragNeighbourGuide().has_value();
        const auto revision = v.document.getProject().editRevision; const auto r = v.edits.commitDrag();
        v.finish(r, revision != v.document.getProject().editRevision);
        if (r.wasOk() && joined) { v.editStatus = ko("이웃 클립에 붙임"); v.updateControls(); }
    }
    else if (mode == Drag::scrub && !v.edits.isLocked()) { v.edits.seek(moved ? v.playhead : downSample); v.selectionChanged(); }
    else if (mode == Drag::range) { if (!moved && !v.edits.isLocked()) v.edits.seek(v.sampleFor(e.x)); v.selectionChanged(); }
    if (collapseSelection && !moved && pendingClipUnchanged()) { v.edits.clickClip(downClip); v.selectionChanged(); }
    downSnapshot.reset(); downSelection.clear(); downExplicitSelection.clear(); downTargets.clear(); collapseSelection = false;
    v.snapButton.setButtonText(v.snapButton.getToggleState() ? ko("스냅 켜짐") : ko("스냅 꺼짐"));
    mouseMove(e);
    repaint();
}
void TimelineView::Rows::mouseMove(const juce::MouseEvent& e)
{
    bool edge = false, clip = false; const auto row = (e.y - rulerHeight) / rowHeight;
    if (!view.edits.isLocked() && e.y >= rulerHeight && e.x >= headerWidth && row >= 0 && row < int(view.tracks.size()))
        for (const auto* c : view.visibleIndex.visible(std::size_t(row), view.sampleFor(e.x - 8), view.sampleFor(e.x + 8) + 1))
        { edge |= std::abs(e.x - view.xFor(c->timelineStartSample)) <= 7 || std::abs(e.x - view.xFor(c->timelineEnd())) <= 7; clip |= c->timelineStartSample <= view.sampleFor(e.x) && c->timelineEnd() > view.sampleFor(e.x); }
    setMouseCursor(view.edits.isLocked() ? juce::MouseCursor::NormalCursor : edge ? juce::MouseCursor::LeftRightResizeCursor : clip ? juce::MouseCursor::DraggingHandCursor : e.x >= headerWidth ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
}
void TimelineView::Rows::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (dragging != Drag::none) return;
    if (e.mods.isCtrlDown()) view.zoomAt(std::pow(2.0, -double(wheel.deltaY) * 2.0), e.position.x);
    else if (e.mods.isShiftDown() || std::abs(wheel.deltaX) > .001f)
    {
        view.recordingPreview.follow = false;
        view.viewStart += -(wheel.deltaX ? wheel.deltaX : wheel.deltaY) * view.viewSeconds; view.updateRange(); repaint();
    }
    else juce::Component::mouseWheelMove(e, wheel);
}
}
