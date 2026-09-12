#include <juce_gui_extra/juce_gui_extra.h>
#include "TestSupport.h"
#include "ui/TimelineView.h"
#include "ui/TimelineView.automation.h"
#include "ui/RecordView.h"
#include "ui/ShortcutSettingsPanel.h"
#include "playback/ImportedAudioCache.h"
#include "export/TimelineExporter.h"
#include "storage/EditJournal.h"
#include "CutSeamChecks.h"
#include "PlaybackGapChecks.h"
#include <limits>

namespace gocue::recorder
{
struct TimelineUxTestAccess
{
    static juce::TextButton* button(const TimelineView& view, TimelineAction action)
    { const auto it = view.buttons.find(action); return it == view.buttons.end() ? nullptr : it->second.get(); }
    static std::size_t buttonCount(const TimelineView& view) { return view.buttons.size(); }
    static int menuCount(const TimelineView& view)
    {
        const auto menu = view.createEditMenu(); int count = 0;
        for (juce::PopupMenu::MenuItemIterator i(menu); i.next();)
            if (i.getItem().itemID > 0 && i.getItem().itemID <= int(TimelineAction::showTrack) + 1) ++count;
        return count;
    }
    static juce::PopupMenu menu(const TimelineView& view) { return view.createEditMenu(); }
    static auto menuContext(const TimelineView& view) { return view.captureEditMenuContext(); }
    static void menuResult(TimelineView& view, int result, const TimelineView::EditMenuContext& context)
    { view.handleEditMenuResult(result, context); }
    static juce::PopupMenu headerMenu(TimelineView& view, std::size_t row) { return view.headers.at(row)->createContextMenu(); }
    static void headerMenuResult(TimelineView& view, std::size_t row, int result, const RecorderDocument::Snapshot& base)
    { view.headers.at(row)->handleContextMenuResult(result, base); }
    static juce::Component& header(TimelineView& view, std::size_t row) { return *view.headers.at(row); }
    static bool transportClearOfSnap(const TimelineView& view) { return view.transport.getRight() <= view.snapButton.getX(); }
    static void selectRange(TimelineView& view, Sample start, Sample end)
    { view.rangeStart.setText(juce::String(start)); view.rangeEnd.setText(juce::String(end)); view.rangeButton.onClick(); }
    static juce::Image waveImage(TimelineView& view, const Clip& clip)
    {
        juce::Image image(juce::Image::ARGB, 800, 160, true, juce::SoftwareImageType{});
        juce::Graphics g(image); view.drawWave(g, clip, {250, 20, 240, 100}); return image;
    }
    static juce::Image rowImage(TimelineView& view)
    {
        juce::Image image(juce::Image::ARGB, view.rows.getWidth(), view.rows.getHeight(), true, juce::SoftwareImageType{});
        juce::Graphics g(image); view.rows.paint(g); return image;
    }
    static juce::String status(const TimelineView& view) { return view.selectionInfo.getText(); }
    static juce::String statusTooltip(TimelineView& view) { return view.selectionInfo.getTooltip(); }
    static int verticalScroll(const TimelineView& view) { return view.viewport.getViewPositionY(); }
    static std::optional<Sample> guide(const TimelineView& view) { return view.rows.snapGuide; }
    static Sample sample(const TimelineView& view, float x) { return view.sampleFor(x); }
    static void snap(TimelineView& view, bool on) { view.snapButton.setToggleState(on, juce::dontSendNotification); }
    static const std::vector<Track>& tracks(const TimelineView& view) { return view.tracks; }
    static bool recordingRow(const TimelineView& view, std::size_t row) { return view.isRecordingTrack(view.tracks.at(row)); }
    static bool displayOnlyRow(const TimelineView& view, std::size_t row) { return !view.headers.at(row)->isEnabled(); }
    static juce::String menuLabel(const TimelineView& view, TimelineAction action)
    {
        const auto menu = view.createEditMenu();
        for (juce::PopupMenu::MenuItemIterator i(menu); i.next();)
            if (i.getItem().itemID == int(action) + 1) return i.getItem().text;
        return {};
    }
};
}

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
juce::Component* rowsOf(TimelineView& view)
{
    for (auto* child : view.getChildren()) if (auto* viewport = dynamic_cast<juce::Viewport*>(child))
        if (viewport->getViewedComponent() && viewport->getViewedComponent()->getHeight() > 100) return viewport->getViewedComponent();
    throw std::runtime_error("Timeline rows missing");
}
juce::ScrollBar* horizontalOf(TimelineView& view)
{
    for (auto* child : view.getChildren()) if (auto* bar = dynamic_cast<juce::ScrollBar*>(child)) return bar;
    throw std::runtime_error("Timeline scrollbar missing");
}
juce::MouseEvent mouse(juce::Component& rows, float x, float y, float downX, float downY, int mods = juce::ModifierKeys::leftButtonModifier)
{
    return {juce::Desktop::getInstance().getMainMouseSource(), {x, y}, juce::ModifierKeys(mods), 1, 0, 0, 0, 0,
            &rows, &rows, juce::Time::getCurrentTime(), {downX, downY}, juce::Time::getCurrentTime(), 1, x != downX || y != downY};
}
float xAt(TimelineView& view, double seconds)
{
    const auto* bar = horizontalOf(view);
    return float(TimelineLayout::headerWidth + (seconds - bar->getCurrentRangeStart()) / bar->getCurrentRangeSize() * (rowsOf(view)->getWidth() - TimelineLayout::headerWidth));
}
void adopt(RecorderDocument& document) { require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "UI fixture"); }
bool trackShown(const TimelineView& view, const Id& id)
{
    const auto& tracks = TimelineUxTestAccess::tracks(view);
    return std::any_of(tracks.begin(), tracks.end(), [&](const Track& t) { return t.trackId == id; });
}
juce::PopupMenu::Item menuItem(const juce::PopupMenu& menu, const juce::String& label)
{
    for (juce::PopupMenu::MenuItemIterator i(menu); i.next();) if (i.getItem().text == label) return i.getItem();
    throw std::runtime_error("Menu item missing: " + label.toStdString());
}
RecorderProject offGridFixture()
{
    auto p = makeTimelineUiFixture(); p.fps = {60, 1};
    for (auto& track : p.tracks)
    {
        auto& clips = track.clips.edit();
        clips[0].lengthSamples = 320640;
        clips[1].timelineStartSample = 600000;
    }
    require(p.validate().wasOk(), "Off-grid fixture"); return p;
}
void dragRows(TimelineView& v, const Id& id, TimelineAction action, Sample at, int modifiers = 0)
{
    const auto p = v.edits.document.snapshot(); const auto& c = *p->findClip(id);
    const auto edge = action == TimelineAction::trimOut ? c.timelineEnd() : c.timelineStartSample;
    const auto grab = action == TimelineAction::move ? c.lengthSamples / 2 : 0;
    if (action != TimelineAction::move) { v.edits.clickClip(id); v.selectionChanged(); }
    auto* rows = rowsOf(v); const auto x = xAt(v, double(edge + grab) / p->Fs), target = xAt(v, double(at + grab) / p->Fs);
    const auto mods = modifiers | juce::ModifierKeys::leftButtonModifier;
    rows->mouseDown(mouse(*rows, x, 45, x, 45, mods)); rows->mouseDrag(mouse(*rows, target, 45, x, 45, mods));
    require(v.edits.dragPreview() != nullptr, "Rows drag did not start");
    require(v.edits.dragPreview()->status.wasOk(), v.edits.dragPreview()->status.getErrorMessage().toRawUTF8());
    rows->mouseUp(mouse(*rows, target, 45, x, 45, mods));
}
void paint(TimelineView& view)
{
    juce::Image image(juce::Image::ARGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType{});
    juce::Graphics graphics(image); view.paintEntireComponent(graphics, true);
}
RecorderProject stereoWaveFixture()
{
    auto p = makeTimelineUiFixture(); auto registry = std::make_shared<MediaRegistry>(*p.media); p.media = registry;
    auto& take = registry->takes[0]; take.capture.physicalInputs = {0, 2}; take.capture.physicalInputsRight = {1, 3};
    for (auto& asset : registry->assets)
        if (std::find(take.microphoneAssetIds.begin(), take.microphoneAssetIds.end(), asset.assetId) != take.microphoneAssetIds.end()) asset.originalFormat.channels = 2;
    auto imported = *registry->findAsset(take.microphoneAssetIds[1]); imported.assetId = newId(); imported.kind = AssetKind::importAudio;
    imported.relativePath = "media/imports/" + imported.assetId + "/original.wav"; registry->assets.push_back(imported);
    Track track; track.kind = TrackKind::importAudio; track.name = "Stereo import";
    Clip clip; clip.assetId = imported.assetId; clip.trackId = track.trackId; clip.lengthSamples = imported.logicalLength;
    track.clips.edit().push_back(clip); p.tracks.push_back(track);
    require(p.validate().wasOk(), p.validate().getErrorMessage().toRawUTF8()); return p;
}
int greenPixels(const juce::Image& image, int top, int bottom)
{
    int count = 0;
    for (int y = top; y < bottom; ++y) if (image.getPixelAt(350, y) == Palette::meterGreen) ++count;
    return count;
}
RecorderProject extremeProject()
{
    auto p = makeTimelineUiFixture(); const auto maximum = (std::numeric_limits<Sample>::max)();
    auto media = std::make_shared<MediaRegistry>(*p.media); p.media = media;
    p.linkGroups.resize(1); media->takes[0].logicalLength = maximum;
    for (auto& track : p.tracks)
    {
        auto& clips = track.clips.edit(); clips.resize(1); auto& clip = clips[0];
        clip.timelineStartSample = maximum - clip.lengthSamples - 1; clip.sourceIn = maximum / 2;
        for (auto& asset : media->assets) if (asset.assetId == clip.assetId)
        { asset.logicalLength = maximum; asset.availableRanges = {{0, maximum}}; }
    }
    require(p.validate().wasOk(), "Extreme project must be valid"); return p;
}
}
int runTimelineUxTests()
{
    juce::ScopedJuceInitialiser_GUI gui;
    Suite suite;
    suite.test("toolbar and edit menu expose only the retained actions", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620);
        for (const bool locked : {false, true})
        {
            v.refresh(locked, 0, {});
            require(TimelineUxTestAccess::buttonCount(v) == 7 && TimelineUxTestAccess::menuCount(v) == 7, "Unexpected edit action count");
            for (const auto action : {TimelineAction::rippleAll, TimelineAction::rippleAudio, TimelineAction::earlier,
                                     TimelineAction::later, TimelineAction::unlink, TimelineAction::link, TimelineAction::hideTrack, TimelineAction::showTrack})
                require(!TimelineUxTestAccess::button(v, action) && TimelineUxTestAccess::menuLabel(v, action).isEmpty(), "Removed action remains in toolbar/menu");
            for (const auto action : {TimelineAction::split, TimelineAction::trimIn, TimelineAction::trimOut, TimelineAction::remove,
                                     TimelineAction::undo, TimelineAction::redo, TimelineAction::addMarker})
                require(TimelineUxTestAccess::button(v, action) && TimelineUxTestAccess::menuLabel(v, action).isNotEmpty(), "Retained action missing");
        }
        int requests = 0; v.onAddMarkerRequested = [&] { ++requests; };
        TimelineUxTestAccess::button(v, TimelineAction::addMarker)->onClick();
        require(requests == 1 && d.getProject().markers.empty(), "Marker toolbar bypassed the host hook");
    });
    suite.test("range inputs and delete preserve time with undo still available", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); const auto Fs = d.getProject().Fs;
        TimelineUxTestAccess::selectRange(v, 2 * Fs, 3 * Fs);
        auto* remove = TimelineUxTestAccess::button(v, TimelineAction::remove); require(remove->isEnabled(), "Range delete disabled"); remove->onClick();
        for (const auto& track : d.getProject().tracks)
        {
            require(track.clips.items().size() == 3, "Range delete did not retain both sides");
            require(track.clips.items()[1].timelineStartSample == 3 * Fs && track.clips.items().back().timelineStartSample == 12 * Fs,
                "Delete pulled later material forward");
        }
        TimelineUxTestAccess::button(v, TimelineAction::undo)->onClick();
        require(d.getProject().tracks[0].clips.items().size() == 2, "Range delete undo missing");
    });
    suite.test("track headers have red mute and yellow solo without target selection", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620);
        auto& header = TimelineUxTestAccess::header(v, 2); juce::TextButton* mute = nullptr; juce::TextButton* solo = nullptr; int count = 0;
        for (auto* child : header.getChildren()) if (auto* button = dynamic_cast<juce::TextButton*>(child))
        {
            ++count;
            if (button->getButtonText() == ko("음소거")) mute = button;
            if (button->getButtonText() == ko("솔로")) solo = button;
        }
        require(count == 2 && mute && solo, "Target button remains or listening controls missing");
        require(mute->findColour(juce::TextButton::buttonOnColourId) == Palette::muteOn
            && mute->findColour(juce::TextButton::textColourOnId) == juce::Colours::white, "Mute active contrast");
        require(solo->findColour(juce::TextButton::buttonOnColourId) == Palette::soloOn
            && solo->findColour(juce::TextButton::textColourOnId) == juce::Colours::black, "Solo active contrast");
        require(mute->getWidth() >= 90 && solo->getWidth() >= 90 && mute->getRight() < solo->getX() && solo->getRight() <= header.getWidth(), "Listening controls overlap or overflow");
        header.mouseDown(mouse(header, 10, 10, 10, 10)); mute->onClick(); solo->onClick();
        require(d.getProject().tracks[2].mute && d.getProject().tracks[2].solo && mute->getToggleState() && solo->getToggleState(), "Listening controls stopped editing tracks");
        require(v.edits.selectedTracks().empty() && d.getSelection().empty(), "Header selected a ripple target or clip");
    });
    suite.test("waveform buttons step through bounded session scale and stay clear of snap", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); const auto before = d.snapshot();
        require(v.waveformScale() == 1 && !v.transport.waveOut.isEnabled(), "Initial waveform scale");
        for (const auto size : {juce::Point<int>(960, 240), juce::Point<int>(1180, 620)})
        {
            v.setSize(size.x, size.y); require(TimelineUxTestAccess::transportClearOfSnap(v), "Snap covers waveform buttons");
            require(v.transport.waveIn.getWidth() > 0 && v.transport.waveOut.getRight() < v.transport.waveIn.getX(), "Waveform controls overlap");
        }
        for (unsigned scale : {2u, 4u, 8u, 16u}) { v.transport.waveIn.onClick(); require(v.waveformScale() == scale, "Waveform increment skipped a step"); }
        require(!v.transport.waveIn.isEnabled(), "Upper bound button enabled"); v.changeWaveformScale(true); require(v.waveformScale() == 16, "Upper bound exceeded");
        bool label = false;
        for (auto* child : v.transport.getChildren()) if (auto* l = dynamic_cast<juce::Label*>(child)) label |= l->getText() == ko("×16");
        require(label && v.transport.waveOut.getButtonText() == ko("−") && v.transport.zoomOut.getButtonText() == ko("−"), "UTF-8 waveform/zoom labels are broken");
        require(v.transport.play.getTooltip() == ko("재생 / 정지 · Space"), "Playback tooltip changed");
        v.clearCaches(); v.refresh(false, 0, {}); require(v.waveformScale() == 16, "Cache refresh reset session waveform scale");
        for (unsigned scale : {8u, 4u, 2u, 1u}) { v.transport.waveOut.onClick(); require(v.waveformScale() == scale, "Waveform decrement skipped a step"); }
        v.changeWaveformScale(false); require(v.waveformScale() == 1 && !v.transport.waveOut.isEnabled(), "Lower bound exceeded");
        TimelineView fresh(d); require(fresh.waveformScale() == 1 && d.snapshot() == before && d.getHistory().undoDepth() == 0, "Display scale persisted into the document");
    });
    suite.test("live and loaded stereo microphone peaks draw the physical L/R lanes", []
    {
        RecorderDocument d; require(d.adopt(stereoWaveFixture(), {}, {}).wasOk(), "Stereo fixture"); TimelineView v(d); v.setSize(1180, 620);
        const auto& clip = d.getProject().tracks[3].clips.items()[0];
        auto cache = std::make_shared<PeakCache>(48000, 4, 480);
        std::vector<std::int32_t> block(480 * 4);
        for (unsigned frame = 0; frame < 480; ++frame)
        {
            block[frame * 4] = -4194304; block[frame * 4 + 1] = 4194304;
            block[frame * 4 + 2] = frame % 2 ? 1048576 : 0; block[frame * 4 + 3] = frame % 2 ? -2097152 : 0;
        }
        for (unsigned frame = 0; frame < 480000; frame += 480) cache->append(block.data(), 480, frame);
        for (const bool loaded : {false, true})
        {
            v.clearCaches(); v.refresh(false, 0, {});
            if (loaded) { cache->finish(); v.setLoadedPeaks(clip.assetId, cache->snapshot(), 1); }
            else { v.setPeaks(clip.assetId, cache, 1); v.refresh(false, 0, {}); }
            const auto image = TimelineUxTestAccess::waveImage(v, clip);
            require(greenPixels(image, 20, 45) >= 2 && greenPixels(image, 46, 69) == 0, "Left lane is missing or uses the logical slot index");
            require(greenPixels(image, 71, 94) == 0 && greenPixels(image, 96, 120) >= 4, "Right lane merged into left or reversed");
            require(image.getPixelAt(350, 70) == Palette::line, "Stereo separator missing");
            const auto normal = greenPixels(image, 20, 120); v.transport.waveIn.onClick();
            require(greenPixels(TimelineUxTestAccess::waveImage(v, clip), 20, 120) > normal, "Microphone waveform scale was not applied");
            v.transport.waveOut.onClick();
        }
    });
    suite.test("imported stereo scaling stays inside both lanes and legacy mono snapshots paint", []
    {
        RecorderDocument d; require(d.adopt(stereoWaveFixture(), {}, {}).wasOk(), "Stereo fixture"); TimelineView v(d); v.setSize(1180, 620);
        const auto& clip = d.getProject().tracks.back().clips.items()[0]; const auto before = d.snapshot();
        CachedImportedAudio cache; cache.sampleRate = 48000; cache.channels = 2; cache.samples = 480000; cache.samplesPerPeak = 480;
        cache.peaks.resize(1000); for (auto& bin : cache.peaks) { bin.minimum = {0, -.25f}; bin.maximum = {.125f, 0}; }
        v.setLoadedPeaks(clip.assetId, ImportedAudioCache::peakSnapshot(cache), 0);
        const auto initial = TimelineUxTestAccess::waveImage(v, clip);
        require(greenPixels(initial, 20, 45) >= 2 && greenPixels(initial, 71, 94) == 0 && greenPixels(initial, 96, 120) >= 4, "Imported L/R envelope was merged");
        v.transport.waveIn.onClick(); const auto doubled = TimelineUxTestAccess::waveImage(v, clip);
        require(greenPixels(doubled, 20, 45) > greenPixels(initial, 20, 45), "Imported scale does not change waveform height");
        for (unsigned i = 0; i < 3; ++i) v.transport.waveIn.onClick(); const auto largest = TimelineUxTestAccess::waveImage(v, clip);
        require(greenPixels(largest, 20, 45) >= 22 && greenPixels(largest, 46, 69) == 0
            && greenPixels(largest, 71, 94) == 0 && greenPixels(largest, 96, 120) >= 22, "Scaled channels overflow their own lanes");
        for (int y = 0; y < largest.getHeight(); ++y) for (int x = 0; x < largest.getWidth(); ++x)
            if (x < 250 || x >= 490 || y < 20 || y >= 120) require(largest.getPixelAt(x, y).isTransparent(), "Waveform escaped its clip box");
        v.clearCaches(); v.refresh(false, 0, {}); cache.channels = 1;
        v.setLoadedPeaks(clip.assetId, ImportedAudioCache::peakSnapshot(cache), 0);
        require(greenPixels(TimelineUxTestAccess::waveImage(v, clip), 20, 120) > 0, "Legacy mono summary for stereo asset stopped painting");
        require(d.snapshot() == before, "Waveform scale edited media");
    });
    suite.test("marker labels and two-pixel guides paint above clips across every row", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620);
        Marker primary; primary.sample = 2 * d.getProject().Fs; primary.name = ko("아주 긴 마커 이름의 말줄임과 한글 표시 확인"); primary.colour = "#4c8dff";
        require(d.addMarker(primary).wasOk(), "Marker fixture");
        for (unsigned i = 0; i < 60; ++i)
        { Marker marker; marker.sample = (69 - i) * d.getProject().Fs; marker.name = "Marker " + juce::String(i); require(d.addMarker(marker).wasOk(), "Many markers fixture"); }
        v.refresh(false, 0, {}); const auto before = d.snapshot(); const auto image = TimelineUxTestAccess::rowImage(v);
        const int x = int(xAt(v, 2)); const auto colour = juce::Colour::fromString("ff4c8dff");
        for (int row = 0; row < 4; ++row) require(image.getPixelAt(x, TimelineLayout::rulerHeight + row * TimelineLayout::rowHeight + 40) == colour, "Marker hidden behind a clip");
        require(image.getPixelAt(x + 8, 2) == colour, "Marker coloured label missing");
        require(image.getPixelAt(int(xAt(v, 11)) - 1, 2) != colour, "Dense marker label overlaps its neighbour");
        require(image.getPixelAt(x, image.getHeight() - 2) == colour, "Marker guide stops above the last track");
        paint(v); v.zoom(.1); paint(v); v.reveal(40 * d.getProject().Fs); paint(v); v.setSize(300, 620); paint(v);
        require(d.snapshot() == before, "Marker paint changed the document");
    });
    suite.test("marker names stay readable on white, yellow and dark markers", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620);
        const char* colours[] {"#ffffff", "#ffd400", "#202020"};
        for (unsigned i = 0; i < 3; ++i)
        { Marker m; m.sample = (2 + 4 * i) * d.getProject().Fs; m.name = "MARKER"; m.colour = colours[i]; require(d.addMarker(m).wasOk(), "Marker colour fixture"); }
        v.refresh(false, 0, {}); const auto image = TimelineUxTestAccess::rowImage(v);
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto fill = juce::Colour::fromString("ff" + juce::String(colours[i]).substring(1)); const int x = int(xAt(v, 2 + 4 * i));
            require(image.getPixelAt(x + 8, 2) == fill, "Marker label fill missing");
            bool ink = false; // bold glyph stems leave fully inked pixels; contrast against the fill is what the reader sees
            for (int px = x + 8; px < x + 70 && !ink; ++px) for (int py = 3; py < 19 && !ink; ++py)
                ink = std::abs(image.getPixelAt(px, py).getPerceivedBrightness() - fill.getPerceivedBrightness()) > .5f;
            require(ink, (juce::String("Marker name invisible on ") + colours[i]).toRawUTF8());
        }
    });
    suite.test("camera cards and ruler scrubbing paint without timeline video previews", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); auto* rows = rowsOf(v); const auto x = xAt(v, 4);
        v.refresh(false, TimelineUxTestAccess::sample(v, x), {}); const auto before = TimelineUxTestAccess::rowImage(v);
        const auto fill = before.getPixelAt(int(x) + 15, 85);
        const auto expected = Palette::clipVideoTop.interpolatedWith(Palette::clipVideoBottom,
            (85.5f - float(TimelineLayout::rulerHeight + 3)) / float(TimelineLayout::rowHeight - 6));
        require(std::abs(int(fill.getRed()) - int(expected.getRed())) <= 2
            && std::abs(int(fill.getGreen()) - int(expected.getGreen())) <= 2
            && std::abs(int(fill.getBlue()) - int(expected.getBlue())) <= 2, "Camera card gradient unexpectedly contains a filmstrip");
        rows->mouseDown(mouse(*rows, x, 10, x, 10)); const auto during = TimelineUxTestAccess::rowImage(v);
        for (int y = TimelineLayout::rulerHeight; y < TimelineLayout::rulerHeight + 2 * TimelineLayout::rowHeight; ++y)
            for (int at = TimelineLayout::headerWidth; at < before.getWidth(); ++at)
                require(before.getPixelAt(at, y) == during.getPixelAt(at, y), "Scrubbing added an inline video preview");
        rows->mouseUp(mouse(*rows, x, 10, x, 10));
    });
    suite.test("selected clip handles turn yellow only where the trim already reaches the source start or end", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        const auto Fs = d.getProject().Fs; const auto second = d.getProject().tracks[0].clips.items()[1].clipId; // take 2: 12 s .. 18 s, the whole 6 s asset
        v.edits.clickClip(second); v.selectionChanged();
        const int y = TimelineLayout::rulerHeight + 3 + 26 + (TimelineLayout::rowHeight - 6 - 32) / 2; // handle centre on row 0
        auto image = TimelineUxTestAccess::rowImage(v);
        require(image.getPixelAt(int(xAt(v, 12)) + 5, y) == Palette::meterYellow && image.getPixelAt(int(xAt(v, 18)) - 5, y) == Palette::meterYellow, "Whole-asset clip lacks yellow limit handles");
        require(v.invoke(TimelineAction::trimIn, 13 * Fs, true).wasOk(), "Trim in"); v.refresh(false, 0, {});
        const auto* clip = d.getProject().findClip(second); require(clip && clip->sourceIn == Fs && clip->timelineStartSample == 13 * Fs, "Trim fixture");
        v.edits.clickClip(second); v.selectionChanged(); image = TimelineUxTestAccess::rowImage(v);
        require(image.getPixelAt(int(xAt(v, 13)) + 5, y) == Palette::accent && image.getPixelAt(int(xAt(v, 18)) - 5, y) == Palette::meterYellow, "Handles do not distinguish the extendable start from the source end");
    });
    suite.test("Rows magnet commits the exact off-grid neighbour end", []
    {
        RecorderDocument d; const auto p = offGridFixture(); require(d.adopt(p, {}, {}).wasOk(), "Off-grid adopt");
        TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        auto* rows = rowsOf(v); const auto& moving = p.tracks[0].clips.items()[1];
        const auto x = xAt(v, double(moving.timelineStartSample + 48000) / p.Fs);
        const auto target = xAt(v, double(320640 + 48000 + 100) / p.Fs);
        rows->mouseDown(mouse(*rows, x, 45, x, 45)); rows->mouseDrag(mouse(*rows, target, 45, x, 45));
        rows->mouseUp(mouse(*rows, target, 45, x, 45));
        const auto actual = d.getProject().findClip(moving.clipId)->timelineStartSample;
        std::cout << "SNAP off-grid expected=320640 actual=" << actual << " gapSamples=" << actual - 320640 << '\n';
        require(actual == 320640, "Magnetic neighbour target was rounded back to the frame grid");
        require(TimelineUxTestAccess::status(v).contains(juce::String::fromUTF8("이웃 클립에 붙임")), "Committed join status missing");
    });
    for (const auto action : {TimelineAction::trimIn, TimelineAction::trimOut})
        suite.test(action == TimelineAction::trimIn ? "Rows trim-in joins an off-grid neighbour exactly" : "Rows trim-out joins an off-grid neighbour exactly", [action]
        {
            auto p = offGridFixture();
            for (auto& t : p.tracks)
            {
                auto& clips = t.clips.edit();
                if (action == TimelineAction::trimOut) { clips[0].lengthSamples = 240000; clips[1].timelineStartSample = 320640; }
                else { clips[1].timelineStartSample = 400000; clips[1].sourceIn = 120000; clips[1].lengthSamples = 120000; }
            }
            RecorderDocument d; require(d.adopt(p, {}, {}).wasOk(), "Trim fixture"); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
            const auto id = p.tracks[0].clips.items()[action == TimelineAction::trimIn ? 1 : 0].clipId;
            dragRows(v, id, action, 320640 + 100);
            const auto* c = d.getProject().findClip(id);
            require((action == TimelineAction::trimIn ? c->timelineStartSample : c->timelineEnd()) == 320640, "Trim retained a fractional gap");
            require(TimelineUxTestAccess::status(v).contains(juce::String::fromUTF8("이웃 클립에 붙임")), "Trim join status missing");
        });
    suite.test("Rows free move retains the project grid and linked offsets", []
    {
        auto p = offGridFixture(); p.tracks[3].clips.edit()[1].timelineStartSample += 37;
        RecorderDocument d; require(d.adopt(p, {}, {}).wasOk(), "Offset fixture"); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        const auto id = p.tracks[0].clips.items()[1].clipId, mic = p.tracks[3].clips.items()[1].clipId;
        dragRows(v, id, TimelineAction::move, 950123);
        require(d.getProject().findClip(id)->timelineStartSample == 950400, "Free move lost frame quantisation");
        require(d.getProject().findClip(mic)->timelineStartSample - d.getProject().findClip(id)->timelineStartSample == 37, "Free move lost link offset");
        dragRows(v, id, TimelineAction::move, 320640 + 100);
        require(d.getProject().findClip(id)->timelineStartSample == 320640, "Bundle anchor did not join");
        require(d.getProject().findClip(mic)->timelineStartSample == 320677, "Joined bundle was independently rounded");
        require(d.undo().wasOk() && d.redo().wasOk() && d.getProject().findClip(id)->timelineStartSample == 320640, "Join history was not exact");
    });
    for (const bool alt : {true, false})
        suite.test(alt ? "Rows Alt bypass preserves the exact mouse sample beside a neighbour" : "Rows snap-off preserves the exact mouse sample beside a neighbour", [alt]
        {
            RecorderDocument d; const auto p = offGridFixture(); require(d.adopt(p, {}, {}).wasOk(), "Bypass fixture");
            TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {}); if (!alt) TimelineUxTestAccess::snap(v, false);
            const auto& c = p.tracks[0].clips.items()[1]; const auto grab = c.lengthSamples / 2;
            const auto x = xAt(v, double(c.timelineStartSample + grab) / p.Fs), target = xAt(v, double(321217 + grab) / p.Fs);
            const auto exact = c.timelineStartSample + TimelineUxTestAccess::sample(v, target) - TimelineUxTestAccess::sample(v, float(int(x)));
            dragRows(v, c.clipId, TimelineAction::move, 321217, alt ? juce::ModifierKeys::altModifier : 0);
            require(d.getProject().findClip(c.clipId)->timelineStartSample == exact && exact != 320640, "Bypass was magnetised or frame-rounded");
        });
    for (const bool marker : {true, false})
        suite.test(marker ? "Rows marker magnet overrides the frame grid" : "Rows playhead magnet overrides the frame grid", [marker]
        {
            auto p = offGridFixture(); if (marker) { Marker m; m.sample = 900123; p.markers.push_back(m); }
            RecorderDocument d; require(d.adopt(p, {}, {}).wasOk(), "Point fixture"); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, marker ? 0 : 900123, {});
            dragRows(v, p.tracks[0].clips.items()[1].clipId, TimelineAction::move, 900223);
            require(d.getProject().findClip(p.tracks[0].clips.items()[1].clipId)->timelineStartSample == 900123, "Point magnet was frame-rounded");
        });
    suite.test("snap uses 12 pixels, a rational one-frame/20ms floor, and clip priority on ties", []
    {
        require(TimelineInteraction::snapPixels == 12, "Pixel tolerance");
        require(TimelineInteraction::snapTolerance(.1, 48000, 1000, {120, 1}) == 960, "20ms minimum");
        require(TimelineInteraction::snapTolerance(.1, 48000, 1000, {60, 1}) == 1200, "Frame plus rounding margin");
        require(TimelineInteraction::snapTolerance(.1, 48000, 1000, {30, 1}) == 2400, "One-frame plus rounding margin");
        require(TimelineInteraction::snapTolerance(.1, 48000, 1000, {30000, 1001}) == 2403, "Fractional frame minimum");
        require(TimelineInteraction::snapTolerance(600, 48000, 1000, {60, 1}) == 345600, "Zoomed-out pixel tolerance");
        auto p = offGridFixture(); Marker m; m.sample = 320840; p.markers.push_back(m);
        TimelineSnapIndex snap; const auto ids = TimelineEditController::expandLinks(p, {p.tracks[0].clips.items()[1].clipId});
        snap.build(p, ids, 320840, TimelineAction::trimIn);
        const auto tied = snap.snap(320740, 100, false);
        require(tied.value == 320640 && tied.clipBoundary, "Marker/playhead beat an equidistant clip edge");
        p.markers.clear(); snap.build(p, ids, 0, TimelineAction::trimIn);
        require(snap.snap(321600, 960, false).guide == 320640, "Tolerance boundary should be inclusive");
        require(!snap.snap(321601, 960, false).guide, "Outside tolerance joined");
    });
    suite.test("interactive model moves absorb subframe gaps/overlaps with one shared delta", []
    {
        auto p = offGridFixture(); p.tracks[3].clips.edit()[1].timelineStartSample += 37;
        const auto id = p.tracks[0].clips.items()[1].clipId, mic = p.tracks[3].clips.items()[1].clipId;
        for (const Sample distance : {-799, -1, 0, 1, 799})
        {
            const auto r = ClipEdits::move(p, {id}, 320640 + distance - 600000, true, true);
            require(r.status.wasOk() && r.project.findClip(id)->timelineStartSample == 320640, "Subframe move did not close");
            require(r.project.findClip(mic)->timelineStartSample == 320677, "Subframe move lost relative offset");
        }
        require(ClipEdits::move(p, {id}, 320640 - 800 - 600000, true, true).status.failed(), "One-frame overlap was silently absorbed");
        const auto beyond = ClipEdits::move(p, {id}, 320640 + 800 - 600000, true, true);
        require(beyond.status.wasOk() && beyond.project.findClip(id)->timelineStartSample != 320640, "One-frame gap was silently absorbed");
    });
    suite.test("grid rounding cannot leave a subframe gap or defeat the magnet radius", []
    {
        auto p = offGridFixture(); for (auto& t : p.tracks) t.clips.edit()[0].lengthSamples = 320160;
        const auto id = p.tracks[0].clips.items()[1].clipId;
        const auto r = ClipEdits::move(p, {id}, 321100 - 600000, true, true);
        require(r.status.wasOk() && r.project.findClip(id)->timelineStartSample == 320160, "Rounding left a 640-sample gap");
        TimelineSnapIndex snap; snap.build(p, TimelineEditController::expandLinks(p, {id}), 0, TimelineAction::move);
        const auto tolerance = TimelineInteraction::snapTolerance(.1, p.Fs, 1000, p.fps);
        require(snap.snap(321100, tolerance, false).value == 320160, "Temporal radius missed rounding closure");
        const auto outside = snap.snap(320160 + tolerance + 1, tolerance, false);
        require(!outside.guide, "Outside radius magnetised");
        const auto free = ClipEdits::move(p, {id}, outside.value - 600000, true, true);
        require(free.status.wasOk() && free.project.findClip(id)->timelineStartSample - 320160 >= 800, "Outside radius silently joined after rounding");
    });
    for (const bool in : {true, false})
        suite.test(in ? "interactive model trim-in absorbs subframe gaps/overlaps" : "interactive model trim-out absorbs subframe gaps/overlaps", [in]
        {
            auto p = offGridFixture();
            for (auto& t : p.tracks)
            {
                auto& c = t.clips.edit();
                if (in) { c[1].timelineStartSample = 400000; c[1].sourceIn = 120000; c[1].lengthSamples = 120000; }
                else { c[0].lengthSamples = 240000; c[1].timelineStartSample = 320640; }
            }
            const auto id = p.tracks[0].clips.items()[in ? 1 : 0].clipId;
            for (const Sample distance : {-799, -1, 1, 799})
            {
                const auto r = in ? ClipEdits::trimIn(p, {id}, 320640 + distance, true, true) : ClipEdits::trimOut(p, {id}, 320640 + distance, true, true);
                require(r.status.wasOk(), "Subframe trim rejected");
                const auto* c = r.project.findClip(id); require((in ? c->timelineStartSample : c->timelineEnd()) == 320640, "Subframe trim gap remains");
            }
            const auto r = in ? ClipEdits::trimIn(p, {id}, 319840, true, true) : ClipEdits::trimOut(p, {id}, 321440, true, true);
            require(r.status.failed(), "One-frame trim overlap was absorbed");
        });
    suite.test("legacy subframe video seams hold the preceding PTS even on a cold seek", []
    {
        using namespace recorder_cut_seam;
        for (const Sample gap : {1, 160, 799, 800, 801})
        {
            auto source = syntheticIndex(); auto stats = std::make_shared<DecodeStats>();
            PlaybackVideoClip a; a.source = source; a.mapping.clipId = newId(); a.mapping.trackId = newId(); a.mapping.mediaGeneration = 1; a.mapping.lengthSamples = 320640;
            auto b = a; b.mapping.clipId = newId(); b.mapping.timelineStartSample = 320640 + gap; b.mapping.sourceIn = 110400; b.mapping.lengthSamples = 4800;
            VideoPlaybackEngine video([stats](auto s) { return std::make_unique<DelayedDecoder>(s, stats); }); video.prepare({a, b});
            PlaybackDisplayState display; unsigned black = 0, missing = 0;
            for (const Sample at : {320639LL, 320640LL, 320640 + gap - 1, 320640 + gap})
            {
                const auto gen = video.seek(at); awaitFrame([&] { return video.ready(at, gen); });
                const auto selection = video.displaySelection(0); const auto decision = submitPicture(display, selection);
                const bool trueGap = gap >= 800 && at >= 320640 && at < b.mapping.timelineStartSample;
                require(selection.gap == trueGap, "Gap classification changed");
                if (!trueGap)
                {
                    missing += !selection.frame; black += decision.action == PlaybackDisplayAction::clear;
                    require(selection.frame && selection.frame->pts == (at < b.mapping.timelineStartSample ? 400 : 138), "Wrong held/incoming PTS");
                }
            }
            require(black == 0 && missing == 0, "Subframe seam lost its picture");
            std::cout << "SNAP legacy gapSamples=" << gap << " black=" << black << " missing=" << missing << '\n';
        }
    });
    suite.test("session recovered tail gap agrees with final export", []
    { recorder_playback_gap::sessionGapCheck(recorder_playback_gap::Missing::tail); });
    suite.test("session incoming leading source gap agrees with final export", []
    { recorder_playback_gap::sessionGapCheck(recorder_playback_gap::Missing::head); });
    suite.test("session internal one-sample camera gap agrees with final export", []
    { recorder_playback_gap::sessionGapCheck(recorder_playback_gap::Missing::interior); });
    suite.test("session joined complete clips keep zero black or empty frames", []
    { recorder_playback_gap::sessionGapCheck(recorder_playback_gap::Missing::none); });
    suite.test("session trimmed clips retain legacy subframe hold and final source coordinate", []
    { recorder_playback_gap::sessionGapCheck(recorder_playback_gap::Missing::none, 799, 800); });
    suite.test("engine explicit gap split preserves revoked original tail boundary", []
    { recorder_playback_gap::fragmentGapSplitCheck(true); });
    suite.test("engine explicit gap split preserves revoked original head boundary", []
    { recorder_playback_gap::fragmentGapSplitCheck(false); });
    suite.test("subframe asset gaps and different tracks remain explicit gaps", []
    {
        using namespace recorder_cut_seam;
        auto source = syntheticIndex(); auto stats = std::make_shared<DecodeStats>();
        PlaybackVideoClip a; a.source = source; a.mapping.clipId = newId(); a.mapping.trackId = newId(); a.mapping.mediaGeneration = 1; a.mapping.lengthSamples = 4800; a.mapping.gaps = {{1600, 1}};
        VideoPlaybackEngine video([stats](auto s) { return std::make_unique<DelayedDecoder>(s, stats); }); video.prepare({a});
        video.seek(1600); require(video.displaySelection(0).gap, "One-sample asset gap was filled");
        a.mapping.gaps.clear(); auto b = a; b.mapping.clipId = newId(); b.mapping.trackId = newId(); b.mapping.timelineStartSample = 4801;
        video.prepare({a, b}); video.seek(4800); require(video.displaySelection(0).gap, "Different-track gap was filled");
    });
    if (juce::SystemStats::getEnvironmentVariable("RECORDER_SNAP_PROJECT", {}).isNotEmpty())
        suite.test("copied real takes split-delete-drag with exact seam and legacy gap playback", []
        {
            using namespace recorder_cut_seam;
            const juce::File file(juce::SystemStats::getEnvironmentVariable("RECORDER_SNAP_PROJECT", {}));
            RecorderProject p; require(RecorderSerializer::readCheckpoint(file, p).wasOk(), "Read copied checkpoint");
            require(p.media->takes.size() == 1 && p.tracks.size() == 2, "Expected the supplied single-take camera/microphone fixture");
            auto registry = std::make_shared<MediaRegistry>(*p.media); p.media = registry;
            auto take = registry->takes.front(); take.takeId = newId(); take.number = 2; take.placementSample = 600000; take.microphoneAssetIds.clear();
            LinkGroup group;
            for (auto& track : p.tracks)
            {
                auto clip = track.clips.items().front(); auto asset = *registry->findAsset(clip.assetId);
                asset.assetId = newId(); asset.contentIdentity = "snap-independent-copy-" + asset.assetId;
                if (asset.kind == AssetKind::camera)
                {
                    const auto original = file.getParentDirectory().getChildFile(asset.relativePath);
                    const auto second = original.getSiblingFile("cam-second.mp4");
                    require(second.existsAsFile(), "Independent camera copy missing");
                    asset.relativePath = second.getRelativePathFrom(file.getParentDirectory()).replaceCharacter('\\', '/');
                    take.cam1AssetId = asset.assetId;
                }
                else
                {
                    take.microphoneAssetIds.push_back(asset.assetId);
                    for (auto& chunk : asset.chunks)
                    {
                        const auto original = file.getParentDirectory().getChildFile(chunk.relativePath);
                        const auto second = original.getSiblingFile(original.getFileNameWithoutExtension() + "-second.wav");
                        require(second.existsAsFile(), "Independent PCM copy missing");
                        chunk.relativePath = second.getRelativePathFrom(file.getParentDirectory()).replaceCharacter('\\', '/');
                    }
                }
                registry->assets.push_back(asset); clip.assetId = asset.assetId; clip.clipId = newId();
                clip.timelineStartSample = 600000; clip.linkGroupId = group.linkGroupId; group.clipIds.push_back(clip.clipId);
                track.clips.edit().push_back(clip);
            }
            registry->takes.push_back(take); p.linkGroups.push_back(group);
            require(p.validate().wasOk(), p.validate().getErrorMessage().toRawUTF8());
            RecorderDocument d; require(d.adopt(p, {}, {}).wasOk(), "Adopt copied takes"); TimelineView v(d); v.setSize(1380, 620); v.refresh(false, 0, {});
            const auto cameraAt = [&](Sample at)
            {
                for (const auto& c : d.getProject().tracks[0].clips.items()) if (c.timelineStartSample <= at && at < c.timelineEnd()) return c.clipId;
                throw std::runtime_error("Edited real camera clip missing");
            };
            for (const Sample start : {0, 600000})
            {
                v.edits.clickClip(cameraAt(start)); v.edits.followPlayhead(start + 48000); require(v.invoke(TimelineAction::split).wasOk(), "Real first split");
                v.edits.clickClip(cameraAt(start + 60000)); v.edits.followPlayhead(start + 110400); require(v.invoke(TimelineAction::split).wasOk(), "Real second split");
                v.edits.clickClip(cameraAt(start + 60000)); require(v.invoke(TimelineAction::remove).wasOk(), "Real middle delete");
                dragRows(v, cameraAt(start + 110400), TimelineAction::move, start + 48000 + 100);
            }
            const auto tailA = cameraAt(60000), headB = cameraAt(600000), tailB = cameraAt(660000);
            const auto boundary = d.getProject().findClip(tailA)->timelineEnd();
            v.edits.clickClip(headB); v.edits.clickClip(tailB, false, true);
            dragRows(v, headB, TimelineAction::move, boundary + 100);
            require(d.getProject().findClip(headB)->timelineStartSample == boundary && boundary == 418080, "Real take seam has a residual gap");
            require(d.getProject().findClip(tailB)->timelineStartSample == boundary + 48000, "Selected fragments lost their shared delta");
            require(RecorderSerializer::writeCheckpoint(file.getSiblingFile("snap-joined.recorder"), d.getProject()).wasOk(), "Write edited copy");
            MediaIndex index;
            for (const Sample legacyGap : {0, 160})
            {
                const auto project = legacyGap == 0 ? d.getProject() : checked(ClipEdits::move(d.getProject(), {headB, tailB}, legacyGap, false));
                const auto plan = RenderPlanCompiler::compile(project); std::vector<PlaybackVideoClip> clips;
                for (const auto& c : plan->activeClips) if (c.trackId == project.tracks[0].trackId)
                {
                    const auto* asset = project.media->findAsset(c.assetId);
                    auto source = index.openVideo(file.getParentDirectory().getChildFile(asset->relativePath), project.Fs);
                    auto mapping = c; mapping.mediaGeneration = Sample(source->generation); clips.push_back({mapping, 0, source});
                }
                std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) { return a.mapping.timelineStartSample < b.mapping.timelineStartSample; });
                VideoPlaybackEngine video; video.prepare(clips); video.seek(boundary - 24 * step, 1);
                awaitFrame([&] { require(video.status().wasOk(), video.status().getErrorMessage().toRawUTF8()); return video.ready(boundary - 24 * step, 1); });
                PlaybackDisplayState display; unsigned missing = 0, black = 0, empty = 0, wrongPts = 0, orderErrors = 0;
                Sample previous = -1; const auto began = std::chrono::steady_clock::now();
                juce::String csv = "relativeFrame,timelineSample,expectedPts,selectedPts,displayedPts,black,empty\n";
                for (int relative = -24; relative <= 10; ++relative)
                {
                    std::this_thread::sleep_until(began + std::chrono::microseconds((relative + 24) * 1000000 / 60));
                    const auto sample = boundary + relative * step; video.requestFrames(sample, 1, true);
                    const auto selection = video.displaySelection(0); const auto decision = submitPicture(display, selection);
                    if (relative < -10) continue;
                    const PlaybackVideoClip* expected = nullptr;
                    for (const auto& c : clips) if (c.mapping.timelineStartSample <= sample) expected = &c;
                    require(expected != nullptr, "Expected real clip");
                    const auto sourceSample = expected->mapping.sourceIn + (std::min)(sample - expected->mapping.timelineStartSample, expected->mapping.lengthSamples - 1);
                    const auto pts = expected->source->packets[expected->source->frameAt(sourceSample)].pts;
                    missing += !selection.frame; black += decision.action == PlaybackDisplayAction::clear; empty += !decision.frame;
                    if (selection.frame)
                    {
                        wrongPts += selection.frame->source != expected->source || selection.frame->pts != pts;
                        orderErrors += selection.frame->begin < previous; previous = selection.frame->begin;
                    }
                    csv += juce::String(relative) + "," + juce::String(sample) + "," + juce::String(pts) + ","
                        + (selection.frame ? juce::String(selection.frame->pts) : "empty") + ","
                        + (decision.frame ? juce::String(decision.frame->pts) : "empty") + ","
                        + juce::String(int(decision.action == PlaybackDisplayAction::clear)) + "," + juce::String(int(!decision.frame)) + "\n";
                }
                auto report = jsonObject(); jsonSet(report, "measurement", "Copied H264 files; production D3D11VA decoder and display policy; synthetic successful sink, no capture/ASIO/HWND Present");
                jsonSet(report, "frames", 21); jsonSet(report, "boundarySample", boundary); jsonSet(report, "gapSamples", legacyGap);
                jsonSet(report, "missingExactFrames", missing); jsonSet(report, "blackSubmissions", black); jsonSet(report, "emptyFrames", empty);
                jsonSet(report, "wrongPts", wrongPts); jsonSet(report, "orderErrors", orderErrors); jsonSet(report, "engine", video.telemetry());
                writeReport(legacyGap ? "real-snap-legacy-gap" : "real-snap-joined", csv, report);
                std::cout << "SNAP REAL boundary=" << boundary << " gap=" << legacyGap << " black=" << black << " empty=" << empty << " missing=" << missing << " wrongPts=" << wrongPts << '\n';
                require(missing == 0 && black == 0 && empty == 0 && wrongPts == 0 && orderErrors == 0, "Real seam picture mismatch (see CSV)");
            }
        });
    suite.test("3px jitter selects without opening a drag or changing history; 4px starts a preview", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        auto* rows = rowsOf(v); const auto x = xAt(v, 4), y = 45.0f; const auto snapshot = d.snapshot();
        rows->mouseDown(mouse(*rows, x, y, x, y)); rows->mouseDrag(mouse(*rows, x + 3, y, x, y));
        require(!v.edits.dragPreview(), "Subthreshold move creates preview"); rows->mouseUp(mouse(*rows, x + 3, y, x, y));
        require(d.snapshot() == snapshot && d.getSelection().size() == 4, "Click moved media or lost linked selection");
        rows->mouseDown(mouse(*rows, x, y, x, y)); rows->mouseDrag(mouse(*rows, x + 4, y, x, y));
        require(v.edits.dragPreview() != nullptr, "4px drag did not start"); require(d.snapshot() == snapshot, "Preview mutated document");
        v.clearCaches(); require(!v.edits.dragPreview(), "Reset retained drag");
    });
    suite.test("Clip body drag selects a range across all tracks and Delete cuts it out; a body click still selects", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        auto* rows = rowsOf(v); const auto x = xAt(v, 4), endX = xAt(v, 5); const auto before = d.snapshot();
        const auto plain = TimelineUxTestAccess::rowImage(v);
        rows->mouseDown(mouse(*rows, x, 66, x, 66)); rows->mouseDrag(mouse(*rows, endX, 66, x, 66));
        require(!v.edits.dragPreview() && d.getSelection().empty(), "Body drag moved the clip or kept the clip selection");
        rows->mouseUp(mouse(*rows, endX, 66, x, 66));
        const auto range = v.edits.selectedRange(); require(range && range->length > 0 && d.snapshot() == before, "Body drag did not select a range");
        const auto highlighted = TimelineUxTestAccess::rowImage(v); const int mid = int((x + endX) / 2), outside = int(x) - 24;
        require(highlighted.getPixelAt(mid, 4) != plain.getPixelAt(mid, 4) && highlighted.getPixelAt(outside, 4) == plain.getPixelAt(outside, 4), "Ruler does not show the selected range");
        require(highlighted.getPixelAt(mid, TimelineLayout::rulerHeight + 40) != plain.getPixelAt(mid, TimelineLayout::rulerHeight + 40), "Range tint hidden under the opaque clip");
        require(v.edits.enabled(TimelineAction::remove) && v.invoke(TimelineAction::remove).wasOk(), "Delete did not cut the range");
        require(d.getProject().editRevision == before->editRevision + 1 && !v.edits.selectedRange(), "Range cut was not one edit");
        const auto inside = range->start + range->length / 2;
        for (const auto& track : d.getProject().tracks) for (const auto& clip : track.clips.items())
            require(!d.getProject().isActive(clip) || clip.timelineStartSample > inside || clip.timelineEnd() <= inside, "Range still covered on a track");
        require(d.getProject().activeTimelineEnd() == before->activeTimelineEnd(), "Range cut rippled the timeline");
        require(d.undo().wasOk() && d.snapshot()->editRevision == before->editRevision + 2, "Undo of the cut failed");
        v.refresh(false, 0, {}); // the app refreshes the visible index every 33 ms; the click below hits the restored clips
        rows->mouseDown(mouse(*rows, x, 66, x, 66)); rows->mouseUp(mouse(*rows, x, 66, x, 66));
        require(d.getSelection().size() == 4 && !v.edits.selectedRange(), "Body click did not select the linked clips");
    });
    suite.test("linked movement keeps original tracks when pointer crosses lanes and commits once", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        auto* rows = rowsOf(v); const auto x = xAt(v, 4), endX = xAt(v, 5); const auto before = d.snapshot();
        rows->mouseDown(mouse(*rows, x, 45, x, 45)); rows->mouseDrag(mouse(*rows, endX, 138, x, 66));
        require(v.edits.dragPreview() && v.edits.dragTargets().size() == 4, "Linked ghosts missing");
        rows->mouseUp(mouse(*rows, endX, 138, x, 66)); require(d.getProject().editRevision == before->editRevision + 1, "Gesture was not one edit");
        for (std::size_t lane = 0; lane < 4; ++lane)
        {
            const auto& old = before->tracks[lane].clips.items()[0]; const auto* after = d.getProject().findClip(old.clipId);
            require(after && after->trackId == old.trackId && after->timelineStartSample > 0, "Crossing pointer changed a source lane");
        }
        require(d.undo().wasOk(), "Gesture undo failed");
        require(d.getProject().findClip(before->tracks[0].clips.items()[0].clipId)->timelineStartSample == 0, "Undo did not restore position");
    });
    for (int change = 0; change < 3; ++change)
        suite.test(change == 0 ? "Rows cancel a delayed drag after undo" : change == 1 ? "Rows cancel a delayed drag after selection replacement" : "Rows cancel a delayed drag after clip deletion", [change]
        {
            for (const auto action : {TimelineAction::move, TimelineAction::trimIn, TimelineAction::trimOut})
            {
                RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
                const auto id = d.getProject().tracks[0].clips.items()[0].clipId;
                const auto other = d.getProject().tracks[0].clips.items()[1].clipId;
                v.edits.clickClip(id); require(v.invoke(TimelineAction::move, 48000, true).wasOk(), "Initial move for undo");
                auto* rows = rowsOf(v); const auto x = xAt(v, action == TimelineAction::trimIn ? 1 : action == TimelineAction::trimOut ? 11 : 5);
                rows->mouseDown(mouse(*rows, x, 45, x, 45));
                require(!v.edits.dragPreview(), "mouseDown must defer editing");
                if (change == 0) require(v.invoke(TimelineAction::undo).wasOk(), "Intervening undo");
                else if (change == 1) { d.setSelection({other}); v.refresh(false, 0, {}); }
                else require(v.invoke(TimelineAction::remove).wasOk(), "Intervening delete");
                const auto after = d.snapshot(); const auto selection = d.getSelection(); const auto depth = d.getHistory().undoDepth();
                rows->mouseDrag(mouse(*rows, x + 4, 45, x, 45));
                require(!v.edits.dragPreview(), "Changed mouseDown target opened a drag");
                rows->mouseUp(mouse(*rows, x + 4, 45, x, 45));
                require(d.snapshot() == after && d.getSelection() == selection && d.getHistory().undoDepth() == depth,
                    "Delayed gesture changed the intervening document/selection/history");
            }
        });
    suite.test("delayed multi-selection collapse does not overwrite a replacement selection", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        const auto first = d.getProject().tracks[0].clips.items()[0].clipId, second = d.getProject().tracks[0].clips.items()[1].clipId;
        v.edits.clickClip(first); v.edits.clickClip(second, false, true);
        auto* rows = rowsOf(v); const auto x = xAt(v, 4); rows->mouseDown(mouse(*rows, x, 45, x, 45));
        v.edits.clickClip(second); const auto snapshotSelection = d.getSelection(); const auto before = d.snapshot();
        rows->mouseUp(mouse(*rows, x, 45, x, 45));
        require(d.getSelection() == snapshotSelection && d.snapshot() == before, "mouseUp restored a stale selection");
        v.edits.clickClip(first, false, true); v.refresh(true, 0, {});
        rows->mouseDown(mouse(*rows, x, 45, x, 45)); rows->mouseUp(mouse(*rows, x, 45, x, 45));
        require(v.edits.explicitSelection() == std::vector<Id>{first} && d.snapshot() == before, "Locked timeline no longer allows click selection");
    });
    suite.test("magnet snaps group trailing edges, markers and playhead; Alt bypass is exact", []
    {
        auto p = makeTimelineUiFixture(); Marker marker; marker.sample = 4 * p.Fs; marker.name = "snap"; p.markers.push_back(marker);
        const auto ids = TimelineEditController::expandLinks(p, {p.tracks[0].clips.items()[0].clipId}); TimelineSnapIndex snap;
        snap.build(p, ids, 7 * p.Fs, TimelineAction::move);
        const auto trailing = snap.snap(2 * p.Fs - 120, 200, false);
        require(trailing.value == 2 * p.Fs && trailing.guide == 12 * p.Fs, "Trailing edge did not snap to next clip");
        require(snap.snap(4 * p.Fs + 80, 200, false).value == 4 * p.Fs, "Marker magnet missing");
        require(snap.snap(7 * p.Fs - 80, 200, false).value == 7 * p.Fs, "Playhead magnet missing");
        require(snap.snap(4 * p.Fs + 80, 200, true).value == 4 * p.Fs + 80 && !snap.snap(4 * p.Fs + 80, 200, true).guide, "Alt snapped");
        require(!snap.snap(p.Fs, 200, false).guide, "Outside threshold snapped");
    });
    suite.test("snap arithmetic excludes unrepresentable edges and candidate starts", []
    {
        const auto hi = (std::numeric_limits<Sample>::max)(), lo = (std::numeric_limits<Sample>::min)();
        require(!TimelineSamples::add(hi, 1) && !TimelineSamples::add(lo, -1)
            && !TimelineSamples::subtract(hi, -1) && !TimelineSamples::subtract(lo, 1), "Overflow was representable");
        require(TimelineSamples::subtract(lo, lo) == 0 && TimelineSamples::add(hi, -hi) == 0, "Representable cancellation lost");
        require(TimelineSamples::distance(lo, hi) == (std::numeric_limits<std::uint64_t>::max)()
            && TimelineSamples::distance(lo, 0) == std::uint64_t(hi) + 1, "Full-width distance wrapped");
        RecorderProject p; Track track; Clip reference, other;
        reference.trackId = other.trackId = track.trackId; reference.lengthSamples = other.lengthSamples = 1;
        other.timelineStartSample = hi - 10; track.clips.edit() = {reference, other}; p.tracks.push_back(track);
        TimelineSnapIndex snap; snap.build(p, {reference.clipId, other.clipId}, hi, TimelineAction::trimIn);
        require(!snap.snap(20, 1, false).guide, "Overflowing value + offset snapped");
        auto& clips = p.tracks[0].clips.edit(); clips[0].timelineStartSample = hi / 2 + 10; clips[1].timelineStartSample = 0;
        snap.build(p, {reference.clipId, other.clipId}, hi - clips[0].timelineStartSample + 1, TimelineAction::trimIn);
        require(!snap.snap(hi, 1, false).guide, "Overflowing target - offset snapped");
        snap.build(RecorderProject{}, {}, hi, TimelineAction::move);
        require(!snap.snap(lo, 1, false).guide, "INT64_MIN distance snapped to zero");
        const auto wide = snap.snap(hi - 1, hi, false);
        require(wide.value == hi && wide.guide == hi, "Maximum tolerance overflowed");
        require(!snap.snap(hi - 1, -1, false).guide && !snap.snap(hi - 1, hi, true).guide, "Disabled snap changed value");
    });
    suite.test("snap tolerance and time labels handle the entire sample domain", []
    {
        const auto hi = (std::numeric_limits<Sample>::max)();
        require(TimelineInteraction::snapTolerance(double(hi) / 48000, 48000, 1) == hi, "Fit tolerance was not capped");
        require(TimelineInteraction::snapTolerance(std::numeric_limits<double>::infinity(), 48000, 1) == hi, "Infinite tolerance");
        require(TimelineInteraction::snapTolerance(std::numeric_limits<double>::quiet_NaN(), 48000, 1) == 2400, "NaN tolerance must retain the temporal floor");
        require(TimelineSamples::roundNonnegative(std::nextafter(double(hi), 0.0)) > hi - 2048, "Last finite rounding changed");
        require(formatRecorderTime(48048, 48000) == "00:00:01.001" && formatRecorderTime(-1, 48000) == "00:00:00.000", "Ordinary time labels changed");
        require(formatRecorderTime(hi, 1) == "2562047788015215:30:07.000", "Extreme time label overflowed");
        require(formatRecorderTime(hi, 48000) == "53375995583:39:01.162", "Extreme sample-rate time label overflowed");
    });
    suite.test("Rows updateDrag and paint finish near INT64_MAX at fit and narrow zooms", []
    {
        RecorderDocument d; require(d.adopt(extremeProject(), {}, {}).wasOk(), "Extreme fixture");
        TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {}); const auto before = d.snapshot();
        for (const auto& asset : before->media->assets) if (asset.kind == AssetKind::mic)
        {
            PeakSnapshot peaks; peaks.sampleRate = before->Fs; peaks.channels = 1; peaks.complete = true;
            peaks.samples = std::uint64_t((std::numeric_limits<Sample>::max)()); peaks.samplesPerBin = peaks.samples / 2;
            peaks.bins.resize(3); for (auto& bin : peaks.bins) bin[0] = {-.5f, .5f}; v.setLoadedPeaks(asset.assetId, peaks, 0);
        }
        const auto& clip = before->tracks[0].clips.items()[0]; v.edits.clickClip(clip.clipId);
        require(TimelineSamples::sourceAt(clip, clip.timelineStartSample + 48000) == clip.sourceIn + 48000, "Source calculation overflowed before subtraction");
        v.reveal(clip.timelineEnd()); paint(v);
        require(v.lastPaintWaveColumns > 0, "Extreme source waveform path was not painted");
        require(v.edits.beginDrag(TimelineAction::trimOut), "Begin extreme trim");
        v.edits.dragTo((std::numeric_limits<Sample>::max)(), true); v.selectionChanged(); paint(v); v.edits.cancelDrag(); v.selectionChanged();
        v.zoomToFit(); paint(v);
        // One sample-area pixel makes the snap tolerance exceed INT64_MAX.
        v.setSize(503, 620); v.zoomToFit(); auto* rows = rowsOf(v);
        const auto x = xAt(v, double(clip.timelineStartSample) / before->Fs);
        rows->mouseDown(mouse(*rows, x, 45, x, 45)); rows->mouseDrag(mouse(*rows, x + 4, 45, x, 45));
        require(v.edits.dragPreview() != nullptr, "Extreme pointer did not exercise updateDrag"); paint(v);
        v.clearCaches(); rows->mouseUp(mouse(*rows, x + 4, 45, x, 45));
        v.setSize(300, 620); paint(v); // No sample area remains beside the header.
        require(d.snapshot() == before && d.getHistory().undoDepth() == 0, "Extreme preview changed the document");
    });
    suite.test("mouse anchored zoom, Shift wheel, and scrub release preserve the visible time", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {}); auto* rows = rowsOf(v); auto* bar = horizontalOf(v);
        const auto x = xAt(v, 8); juce::MouseWheelDetails wheel{}; wheel.deltaY = .5f;
        rows->mouseWheelMove(mouse(*rows, x, 10, x, 10, juce::ModifierKeys::ctrlModifier), wheel);
        require(std::abs(xAt(v, 8) - x) < .01f && std::abs(bar->getCurrentRangeSize() - 10) < .001, "Zoom anchor moved");
        const auto start = bar->getCurrentRangeStart(); const auto scrubX = xAt(v, 8);
        rows->mouseDown(mouse(*rows, scrubX, 10, scrubX, 10)); rows->mouseUp(mouse(*rows, scrubX, 10, scrubX, 10));
        require(std::abs(bar->getCurrentRangeStart() - start) < .001, "Scrub release scrolled");
        v.refresh(false, 9 * 48000, {}); require(std::abs(bar->getCurrentRangeStart() - start) < .001, "Playback refresh moved viewport");
        wheel.deltaY = -.1f; rows->mouseWheelMove(mouse(*rows, x, 10, x, 10, juce::ModifierKeys::shiftModifier), wheel);
        require(bar->getCurrentRangeStart() > start, "Shift wheel did not scroll");
        require(TimelineInteraction::edgeScroll(995, 210, 1000, 20, .016) > 0 && TimelineInteraction::edgeScroll(220, 210, 1000, 20, .016) < 0
            && TimelineInteraction::edgeScroll(500, 210, 1000, 20, .016) == 0, "Edge scroll policy");
    });
    suite.test("recording during a drag cancels preview and rejects structural edits", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {}); auto* rows = rowsOf(v); const auto x = xAt(v, 4);
        rows->mouseDown(mouse(*rows, x, 45, x, 45)); rows->mouseDrag(mouse(*rows, x + 20, 45, x, 45)); const auto before = d.snapshot();
        d.setRecordingStructureLock(true); v.refresh(true, 0, {}); rows->mouseUp(mouse(*rows, x + 20, 45, x, 45));
        require(d.snapshot() == before && !v.edits.dragPreview() && !v.edits.enabled(TimelineAction::split), "Recording allowed pending drag commit");
        require(v.invoke(TimelineAction::split).failed(), "Recording split was enabled"); d.setRecordingStructureLock(false);
    });
    suite.test("recording layout keeps both preview hosts and controls above an editable sized timeline", []
    {
        RecorderDocument d; adopt(d); UserSettings s; s.cameraEnabled = {true, true}; s.physicalInputs = {0, 1};
        RecordView root; TimelineView timeline(d); root.addAndMakeVisible(timeline);
        for (const auto size : {juce::Point<int>(960, 640), juce::Point<int>(1180, 780), juce::Point<int>(1920, 1080)})
        {
            root.setSize(size.x, size.y); auto ui = mapUiState(d.getProject(), s, TakeController::State::recording, true, false, true, true);
            root.update(ui, d.getProject(), s, ko("녹화 중"), {}, 3 * 48000, 1000000000, true);
            timeline.setBounds(root.timelineBounds()); timeline.setRecordingPreview(true, 12 * 48000, 3 * 48000, {{true, true}, {1, 2}}, s); timeline.refresh(true, 15 * 48000, {});
            int hosts = 0;
            std::function<void(juce::Component&)> inspect = [&](juce::Component& c)
            {
                if (auto* host = dynamic_cast<juce::HWNDComponent*>(&c)) { ++hosts; const auto box = root.getLocalArea(host, host->getLocalBounds()); require(box.getHeight() >= 45 && box.getBottom() < timeline.getY(), "Preview hidden behind timeline"); }
                for (auto* child : c.getChildren()) inspect(*child);
            };
            inspect(root); require(hosts == 2 && timeline.getHeight() >= 240 && root.stopButton.isEnabled() && root.markerButton.isEnabled(), "Timeline recording controls unavailable");
            const auto snapshot = d.snapshot(); const auto image = root.createComponentSnapshot(root.getLocalBounds());
            require(image.isValid() && d.snapshot() == snapshot, "Growing clip mutated document"); timeline.setRecordingPreview(false, 0, 0, {}, s);
        }
    });
    suite.test("a soloed track keeps its row after its last clip is deleted so it can be un-soloed; un-soloed empty tracks hide", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        std::vector<Id> mics; for (const auto& t : d.getProject().tracks) if (t.kind == TrackKind::mic) mics.push_back(t.trackId);
        require(mics.size() == 2, "Fixture needs two microphone tracks");
        require(v.edits.setTrackListening(mics[0], true).wasOk(), "Solo the first microphone track");
        // The fixture holds two linked takes: delete every clip on the soloed track (its linked partners go with it).
        for (int guard = 0; guard < 8; ++guard)
        {
            Id clip; for (const auto& t : d.getProject().tracks) if (t.trackId == mics[0] && !t.clips.items().empty()) clip = t.clips.items().front().clipId;
            if (clip.isEmpty()) break;
            v.edits.clickClip(clip); require(v.edits.execute(TimelineAction::remove).wasOk(), "Delete a linked take"); v.refresh(false, 0, {});
        }
        for (const auto& t : d.getProject().tracks) require(t.clips.items().empty(), "Fixture still has clips after deleting every take");
        v.refresh(false, 0, {});
        const auto shown = [&](const Id& id) { const auto& ts = TimelineUxTestAccess::tracks(v); return std::any_of(ts.begin(), ts.end(), [&](const Track& t) { return t.trackId == id; }); };
        require(shown(mics[0]) && !shown(mics[1]), "Soloed empty track was hidden or the plain empty track stayed");
        require(v.edits.setTrackHidden(mics[0], true).wasOk(), "Manually hide empty soloed track"); v.refresh(false, 0, {});
        require(!shown(mics[0]), "Manual hiding lost to empty-track solo visibility");
        require(v.edits.setTrackHidden(mics[0], false).wasOk(), "Show empty soloed track"); v.refresh(false, 0, {});
        require(shown(mics[0]), "Unhidden empty soloed track did not return");
        require(v.edits.setTrackListening(mics[0], true).wasOk(), "Un-solo"); v.refresh(false, 0, {});
        require(!shown(mics[0]), "Empty, un-soloed track stayed visible");
        require(v.edits.setTrackHidden(mics[0], true).wasOk() && v.edits.setTrackHidden(mics[0], false).wasOk(), "Toggle empty unused track visibility"); v.refresh(false, 0, {});
        require(!shown(mics[0]), "Showing an unused empty track bypassed automatic hiding");
    });
    suite.test("manual track hiding overrides clips and listening state with journaled undo and redo", []
    {
        struct Journal : IEditJournalSink
        {
            std::vector<EditDelta> edits;
            juce::Result enqueue(const EditDelta& delta) override { edits.push_back(delta); return juce::Result::ok(); }
        } journal;
        RecorderDocument d; require(d.adopt(stereoWaveFixture(), {}, {}).wasOk(), "Visibility fixture");
        TimelineView v(d); v.setSize(1180, 620); d.setJournalSink(&journal);
        const auto mic = d.getProject().tracks[2].trackId;
        require(v.edits.setTrackListening(mic, true).wasOk() && v.edits.setTrackListening(mic, false).wasOk(), "Solo and mute fixture microphone");
        int playbackChanges = 0; v.onListeningChanged = [&] { ++playbackChanges; };
        const auto original = d.snapshot();
        for (const auto row : {2u, 0u, 1u, 4u})
        {
            const auto id = original->tracks[row].trackId; const auto depth = d.getHistory().undoDepth(); const auto visible = d.snapshot();
            require(v.edits.setTrackHidden(id, true).wasOk(), "Hide populated track"); v.refresh(false, 0, {});
            require(!trackShown(v, id) && d.getProject().tracks[row].hidden && d.getHistory().undoDepth() == depth + 1, "Manual hide did not remove the row or create history");
            const auto& delta = journal.edits.back();
            require(delta.name == ko("트랙 숨기기") && delta.entities.size() == 1 && delta.entities[0].entityId == id
                && bool(delta.entities[0].value["hidden"]), "Journal omitted the hidden track state");
            require(delta.validationHash == RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(d.getProject())), "Visibility journal hash mismatch");
            const auto replayed = EditJournal::apply(*visible, EditJournal::payload(*visible, delta, d.getProject()));
            require(RecorderSerializer::toJson(replayed) == RecorderSerializer::toJson(d.getProject()), "Journal replay lost hiding");
            const auto hidden = d.snapshot();
            require(v.edits.setTrackHidden(id, true).wasOk() && d.snapshot() == hidden, "Repeated hide published a no-op");
            require(v.invoke(TimelineAction::undo).wasOk() && trackShown(v, id) && !d.getProject().tracks[row].hidden, "Undo did not restore the row");
            require(v.invoke(TimelineAction::redo).wasOk() && !trackShown(v, id) && d.getProject().tracks[row].hidden, "Redo did not hide the row");
            const auto beforeShow = d.snapshot();
            require(v.edits.setTrackHidden(id, false).wasOk(), "Show populated track"); v.refresh(false, 0, {});
            require(trackShown(v, id) && journal.edits.back().name == ko("트랙 보이기"), "Explicit show did not restore the row");
            require(RecorderSerializer::toJson(EditJournal::apply(*beforeShow, EditJournal::payload(*beforeShow, journal.edits.back(), d.getProject())))
                == RecorderSerializer::toJson(d.getProject()), "Journal replay lost showing");
            require(d.getProject().tracks[row].mute == original->tracks[row].mute && d.getProject().tracks[row].solo == original->tracks[row].solo
                && &d.getProject().tracks[row].clips.items() == &original->tracks[row].clips.items() && d.getProject().media == original->media,
                "Visibility changed listening, clips or media");
        }
        const auto before = d.snapshot();
        require(v.edits.setTrackHidden("placeholder-cam1", true).failed() && v.edits.setTrackHidden(newId(), false).failed()
            && d.snapshot() == before, "Missing/display-only track accepted");
        require(playbackChanges == 0, "Visibility undo/redo restarted playback");
    });
    suite.test("hidden recording targets do not reappear as placeholder rows", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620);
        for (const auto row : {0u, 1u, 2u}) require(v.edits.setTrackHidden(d.getProject().tracks[row].trackId, true).wasOk(), "Hide recording target");
        UserSettings settings; settings.cameraEnabled = {true, true}; const auto before = d.snapshot();
        v.setRecordingPreview(true, 20 * 48000, 48000, {{true, true}, {1, 2, 4}}, settings); v.refresh(true, 21 * 48000, {});
        const auto& tracks = TimelineUxTestAccess::tracks(v);
        require(tracks.size() == 2 && tracks[0].trackId == before->tracks[3].trackId && tracks[1].trackId == "placeholder-mic-4",
            "Hidden target was replaced by a display-only row");
        require(TimelineUxTestAccess::recordingRow(v, 0) && TimelineUxTestAccess::recordingRow(v, 1)
            && TimelineUxTestAccess::displayOnlyRow(v, 1), "Unhidden capture targets or new microphone placeholder changed");
        paint(v); require(d.snapshot() == before, "Recording preview edited the hidden tracks");
    });
    suite.test("track visibility edits are locked during recording", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d);
        const auto mic = d.getProject().tracks[2].trackId, camera = d.getProject().tracks[0].trackId;
        require(v.edits.setTrackHidden(mic, true).wasOk(), "Hide fixture track");
        const auto before = d.snapshot(); const auto depth = d.getHistory().undoDepth();
        d.setRecordingStructureLock(true);
        require(v.edits.setTrackHidden(camera, true).failed() && v.edits.setTrackHidden(mic, false).failed(), "Recording document accepted a visibility edit");
        d.setRecordingStructureLock(false); v.refresh(true, 0, {});
        require(v.edits.setTrackHidden(camera, true).failed() && v.edits.setTrackHidden(mic, false).failed(), "Controller lock accepted a visibility edit");
        require(d.snapshot() == before && d.getHistory().undoDepth() == depth, "Rejected visibility edits mutated the document");
    });
    suite.test("track headers offer hiding only for editable unlocked rows", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620);
        for (const auto row : {0u, 2u})
        {
            const auto item = menuItem(TimelineUxTestAccess::headerMenu(v, row), ko("트랙 숨기기"));
            require(item.isEnabled && item.itemID > 0, "Editable header hide command missing");
            auto& header = TimelineUxTestAccess::header(v, row); const auto before = d.snapshot();
            header.mouseDown(mouse(header, 10, 10, 10, 10));
            require(d.snapshot() == before && d.getSelection().empty() && header.getWidth() == 210 && header.getHeight() == 72,
                "Header left click or geometry changed");
        }
        d.setRecordingStructureLock(true);
        require(!menuItem(TimelineUxTestAccess::headerMenu(v, 2), ko("트랙 숨기기")).isEnabled, "Locked header hide command enabled");
        d.setRecordingStructureLock(false); const auto before = d.snapshot(); int playbackChanges = 0;
        v.onListeningChanged = [&] { ++playbackChanges; };
        TimelineUxTestAccess::headerMenuResult(v, 2, menuItem(TimelineUxTestAccess::headerMenu(v, 2), ko("트랙 숨기기")).itemID, before);
        require(!trackShown(v, before->tracks[2].trackId) && d.getProject().tracks[2].hidden && playbackChanges == 0,
            "Header menu failed to hide/refresh the row or restarted playback");
        RecorderDocument empty; TimelineView preview(empty); UserSettings settings; settings.cameraEnabled = {true, true};
        preview.setRecordingPreview(true, 0, 48000, {{true, true}, {1}}, settings);
        require(TimelineUxTestAccess::tracks(preview).size() == 3, "Placeholder fixture missing");
        for (std::size_t row = 0; row < TimelineUxTestAccess::tracks(preview).size(); ++row)
            require(TimelineUxTestAccess::displayOnlyRow(preview, row) && TimelineUxTestAccess::headerMenu(preview, row).getNumItems() == 0,
                "Display-only header exposed a hide menu");
    });
    suite.test("edit menu lists hidden tracks and restores one or all", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620);
        const auto none = menuItem(TimelineUxTestAccess::menu(v), ko("숨긴 트랙 없음"));
        require(!none.isEnabled && !none.subMenu, "Empty hidden list must be a disabled item");
        for (const auto row : {0u, 2u, 3u}) require(v.edits.setTrackHidden(d.getProject().tracks[row].trackId, true).wasOk(), "Hide menu fixture");
        v.refresh(false, 0, {}); const auto before = d.snapshot();
        const auto submenu = menuItem(TimelineUxTestAccess::menu(v), ko("숨긴 트랙 3개"));
        require(submenu.isEnabled && submenu.subMenu && submenu.subMenu->getNumItems() == 4, "Hidden submenu count incorrect");
        for (const auto row : {0u, 2u, 3u})
            require(menuItem(*submenu.subMenu, before->tracks[row].name).isEnabled, "Hidden track name missing");
        const auto hint = ko(" · 숨긴 트랙 3개는 편집 메뉴에서 보이기");
        require(TimelineUxTestAccess::status(v).contains(hint) && TimelineUxTestAccess::statusTooltip(v).contains(hint), "Hidden-track help or tooltip missing");
        int playbackChanges = 0; v.onListeningChanged = [&] { ++playbackChanges; };
        TimelineUxTestAccess::menuResult(v, menuItem(*submenu.subMenu, before->tracks[2].name).itemID, TimelineUxTestAccess::menuContext(v));
        require(trackShown(v, before->tracks[2].trackId) && !trackShown(v, before->tracks[0].trackId) && !trackShown(v, before->tracks[3].trackId),
            "Single-track show restored the wrong tracks");
        const auto remaining = menuItem(TimelineUxTestAccess::menu(v), ko("숨긴 트랙 2개"));
        const auto revisionBeforeShowAll = d.getProject().editRevision;
        TimelineUxTestAccess::menuResult(v, menuItem(*remaining.subMenu, ko("모두 보이기")).itemID, TimelineUxTestAccess::menuContext(v));
        require(std::none_of(d.getProject().tracks.begin(), d.getProject().tracks.end(), [](const Track& t) { return t.hidden; })
            && TimelineUxTestAccess::tracks(v).size() == before->tracks.size(), "Show all left hidden tracks");
        require(d.getProject().editRevision == revisionBeforeShowAll + 1, "Show all must be one journaled edit");
        require(d.undo().wasOk() && std::count_if(d.getProject().tracks.begin(), d.getProject().tracks.end(), [](const Track& t) { return t.hidden; }) == 2
            && d.redo().wasOk(), "One undo must hide the two tracks again");
        require(!menuItem(TimelineUxTestAccess::menu(v), ko("숨긴 트랙 없음")).isEnabled
            && !TimelineUxTestAccess::status(v).contains(ko("숨긴 트랙")) && !TimelineUxTestAccess::statusTooltip(v).contains(ko("숨긴 트랙"))
            && playbackChanges == 0, "Restoring visibility retained stale hints or refreshed playback");
    });
    suite.test("hidden-track menu rejects changed documents selections and recording locks", []
    {
        for (const bool all : {false, true}) for (const int change : {0, 1, 2})
        {
            RecorderDocument d; adopt(d); TimelineView v(d);
            require(v.edits.setTrackHidden(d.getProject().tracks[0].trackId, true).wasOk()
                && v.edits.setTrackHidden(d.getProject().tracks[2].trackId, true).wasOk(), "Hide menu safety fixture");
            const auto context = TimelineUxTestAccess::menuContext(v);
            const auto submenu = menuItem(TimelineUxTestAccess::menu(v), ko("숨긴 트랙 2개"));
            const auto result = menuItem(*submenu.subMenu, all ? ko("모두 보이기") : d.getProject().tracks[0].name).itemID;
            if (change == 0) require(d.performEdit("reorder tracks", [](EditState& e) { std::swap(e.tracks[0], e.tracks[2]); }).wasOk(), "Reorder menu targets");
            if (change == 1) d.setSelection({d.getProject().tracks[0].clips.items()[0].clipId});
            if (change == 2)
            {
                d.setRecordingStructureLock(true);
                const auto lockedMenu = menuItem(TimelineUxTestAccess::menu(v), ko("숨긴 트랙 2개"));
                require(!lockedMenu.isEnabled && !menuItem(*lockedMenu.subMenu, ko("모두 보이기")).isEnabled
                    && !menuItem(*lockedMenu.subMenu, d.getProject().tracks[0].name).isEnabled, "Locked show commands enabled");
            }
            const auto before = d.snapshot(); const auto depth = d.getHistory().undoDepth();
            TimelineUxTestAccess::menuResult(v, result, context);
            require(d.snapshot() == before && d.getHistory().undoDepth() == depth, "Stale/locked submenu changed visibility");
            require(TimelineUxTestAccess::status(v).contains(change == 2 ? ko("녹화 중") : ko("편집 대상이 바뀌었습니다")), "Menu safety status missing");
        }
    });
    suite.test("track hidden state survives JSON roundtrip and legacy projects default to visible", []
    {
        auto p = makeTimelineUiFixture(); const auto original = RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p));
        const auto resign = [](juce::var& root)
        {
            root.getDynamicObject()->removeProperty("checksum");
            root.getDynamicObject()->setProperty("checksum", RecorderSerializer::fingerprint(root));
        };
        p.tracks[0].hidden = p.tracks[2].hidden = true;
        require(RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)) != original, "Visibility omitted from edit hash");
        RecorderProject restored; require(RecorderSerializer::fromJson(RecorderSerializer::toJson(p), restored).wasOk(), "Hidden project roundtrip");
        for (std::size_t row = 0; row < p.tracks.size(); ++row) require(restored.tracks[row].hidden == p.tracks[row].hidden, "Roundtrip lost hidden state");
        require(restored.schemaVersion == p.schemaVersion && restored.validate().wasOk(), "Visibility changed schema or validation");
        auto legacy = juce::JSON::parse(RecorderSerializer::toJson(p));
        for (auto& track : *legacy["edit"]["tracks"].getArray()) track.getDynamicObject()->removeProperty("hidden");
        resign(legacy);
        require(RecorderSerializer::fromJson(juce::JSON::toString(legacy), restored).wasOk()
            && std::none_of(restored.tracks.begin(), restored.tracks.end(), [](const Track& t) { return t.hidden; }), "Legacy tracks did not default to visible");
        require(RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(restored)) == original
            && RecorderSerializer::fingerprint(juce::JSON::parse(RecorderSerializer::toJson(restored))) == RecorderSerializer::fingerprint(legacy), "Legacy checkpoint/journal hashes changed");
        legacy["edit"]["tracks"][0].getDynamicObject()->setProperty("hidden", false); resign(legacy);
        require(RecorderSerializer::fromJson(juce::JSON::toString(legacy), restored).wasOk() && !restored.tracks[0].hidden, "Explicit false visibility rejected");
        legacy["edit"]["tracks"][0].getDynamicObject()->setProperty("hidden", "true");
        resign(legacy);
        require(RecorderSerializer::fromJson(juce::JSON::toString(legacy), restored).failed(), "Non-boolean hidden field accepted");
    });
    suite.test("hidden tracks retain playback and export clips and listening state", []
    {
        auto p = stereoWaveFixture(); p.tracks[2].solo = true; p.tracks[3].mute = true;
        const auto before = compileAudioRenderPlan(p); const ExportJob exportBefore(p, juce::File::getCurrentWorkingDirectory());
        for (auto& t : p.tracks) t.hidden = true;
        const auto after = compileAudioRenderPlan(p); const ExportJob exportAfter(p, juce::File::getCurrentWorkingDirectory());
        require(p.validate().wasOk() && p.activeTimelineEnd() == before->timeline->timelineEnd && after->timeline->timelineEnd == before->timeline->timelineEnd
            && after->timeline->tracks.size() == before->timeline->tracks.size() && after->timeline->activeClips.size() == before->timeline->activeClips.size()
            && after->sources.size() == before->sources.size() && after->timeline->audibleTrackCount == before->timeline->audibleTrackCount,
            "Hiding tracks changed playback duration, sources or mix membership");
        for (std::size_t row = 0; row < p.tracks.size(); ++row)
        {
            const auto& a = before->timeline->tracks[row]; const auto& b = after->timeline->tracks[row];
            require(a.trackId == b.trackId && a.mute == b.mute && a.solo == b.solo && a.audible == b.audible && a.spans.size() == b.spans.size(), "Hiding changed a listening lane");
            for (const auto& clip : p.tracks[row].clips.items())
                require(std::any_of(after->timeline->activeClips.begin(), after->timeline->activeClips.end(), [&](const RenderClip& c)
                    { return c.clipId == clip.clipId && c.trackId == clip.trackId && c.assetId == clip.assetId && c.lengthSamples == clip.lengthSamples; }), "Hidden clip missing from playback plan");
            if (p.tracks[row].kind == TrackKind::mic || p.tracks[row].kind == TrackKind::importAudio)
            {
                const AudioSourceMask mask{AudioSourceMask::Kind::materialTrack, p.tracks[row].trackId};
                const auto assets = TimelineExporter::assetIds(exportBefore, mask);
                require(!assets.isEmpty() && TimelineExporter::assetIds(exportAfter, mask) == assets, "Hidden audio omitted from export");
            }
        }
        const AudioSourceMask mix{AudioSourceMask::Kind::microphoneMix};
        require(TimelineExporter::assetIds(exportAfter, mix) == TimelineExporter::assetIds(exportBefore, mix)
            && exportAfter.range.sampleCount == exportBefore.range.sampleCount && exportAfter.plan().activeClips.size() == exportBefore.plan().activeClips.size(),
            "Hidden soloed microphone or cameras changed the export plan");
    });
    suite.test("revealTrack restores hidden imported tracks and quietly respects recording locks", []
    {
        RecorderDocument d; require(d.adopt(stereoWaveFixture(), {}, {}).wasOk(), "Import visibility fixture");
        TimelineView v(d); v.setSize(1180, 240); const auto id = d.getProject().tracks.back().trackId;
        require(v.edits.setTrackHidden(id, true).wasOk(), "Hide import track"); v.refresh(false, 0, {});
        require(!trackShown(v, id), "Import track still visible");
        v.revealTrack(d.getProject().tracks.back().trackId);
        require(!d.getProject().tracks.back().hidden && trackShown(v, id) && TimelineUxTestAccess::verticalScroll(v) > 0, "Reveal did not show and scroll to the imported track");
        const auto shown = d.snapshot(); v.revealTrack(id); require(d.snapshot() == shown, "Revealing a visible track created history");
        require(v.edits.setTrackHidden(id, true).wasOk(), "Hide import for lock test");
        d.setRecordingStructureLock(true); v.refresh(false, 0, {}); const auto before = d.snapshot(); const auto status = TimelineUxTestAccess::status(v);
        v.revealTrack(id);
        require(d.snapshot() == before && !trackShown(v, id) && TimelineUxTestAccess::status(v) == status, "Locked reveal edited or reported an error");
    });
    suite.test("first recording creates display-only rows from armed logical microphones", []
    {
        RecorderDocument d; TimelineView v(d); v.setSize(1180, 620); const auto before = d.snapshot();
        RecorderAudioEngine audio; require(audio.openSynthetic(48000, 480, 4, 2).wasOk(), "Synthetic audio");
        require(audio.setInputMap({0, -1, -1, -1, -1, -1, -1, -1}).wasOk() && audio.arm(0, true).wasOk(), "Arm logical microphone");
        UserSettings settings; settings.cameraEnabled = {false, true}; settings.microphoneArmed.fill(false);
        settings.microphoneNames[0] = ko("첫 마이크");
        v.setRecordingPreview(true, 0, 48000, {{true, false}, audio.armedMicrophones()}, settings); v.refresh(true, 48000, {});
        const auto& tracks = TimelineUxTestAccess::tracks(v);
        require(tracks.size() == 3 && tracks[2].kind == TrackKind::mic && tracks[2].microphoneIndex == 0 && tracks[2].name == ko("첫 마이크"), "First microphone row missing");
        require(TimelineUxTestAccess::recordingRow(v, 0) && !TimelineUxTestAccess::recordingRow(v, 1) && TimelineUxTestAccess::recordingRow(v, 2), "Settings overrode actual capture targets");
        require(TimelineUxTestAccess::displayOnlyRow(v, 2) && tracks[2].clips.items().empty(), "Preview row became editable media");
        paint(v); require(d.snapshot() == before && d.getHistory().undoDepth() == 0, "First recording preview published media");
        v.setRecordingPreview(false, 0, 0, {}, settings); v.refresh(false, 0, {});
        // 0.1.6: unused tracks are hidden — the disabled, empty camera one and the finished microphone preview go; the enabled camera two stays.
        require(TimelineUxTestAccess::tracks(v).size() == 1 && TimelineUxTestAccess::tracks(v)[0].kind == TrackKind::cam2 && d.snapshot() == before, "Stopped preview left a microphone track or hid the enabled camera");
    });
    suite.test("new stereo logical slot gets one row and yields to the published track", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620);
        RecorderAudioEngine audio; require(audio.openSynthetic(48000, 480, 4, 2).wasOk(), "Synthetic audio");
        UserSettings settings; settings.stereoSlots[3] = true; settings.microphoneNames[3] = ko("스테레오 입력");
        require(audio.setInputMap({0, -1, -1, 2, -1, -1, -1, -1}, settings.stereoSlots).wasOk(), "Stereo mapping");
        require(audio.arm(0, true).wasOk() && audio.arm(3, true).wasOk(), "Arm stereo logical slot");
        const RecordingPreviewTargets targets{{true, false}, audio.armedMicrophones()}; const auto before = d.snapshot();
        v.setRecordingPreview(true, 20 * 48000, 48000, targets, settings); v.refresh(true, 21 * 48000, {});
        const auto& tracks = TimelineUxTestAccess::tracks(v);
        require(tracks.size() == 5 && tracks.back().microphoneIndex == 3 && tracks.back().name.contains(ko(" · 스테레오")), "Stereo was missing or split into two rows");
        require(TimelineUxTestAccess::recordingRow(v, 2) && !TimelineUxTestAccess::recordingRow(v, 3) && TimelineUxTestAccess::recordingRow(v, 4), "Sparse logical slots were treated as packed channels");
        paint(v); require(d.snapshot() == before, "Stereo preview changed document");
        Track published; published.kind = TrackKind::mic; published.microphoneIndex = 3; published.name = ko("스테레오 입력");
        require(d.performEdit("publish new slot", [&](EditState& state) { state.tracks.push_back(published); }).wasOk(), "Publish actual track");
        v.refresh(true, 21 * 48000, {});
        require(TimelineUxTestAccess::tracks(v).size() == 5 && TimelineUxTestAccess::tracks(v).back().trackId == published.trackId, "Published track duplicated placeholder");
        v.setRecordingPreview(false, 0, 0, {}, settings); v.refresh(false, 0, {});
        // 0.1.6: an empty microphone track that is no longer a recording target is hidden until it holds a clip.
        require(TimelineUxTestAccess::tracks(v).size() == 4 && std::none_of(TimelineUxTestAccess::tracks(v).begin(), TimelineUxTestAccess::tracks(v).end(),
            [](const Track& t) { return t.microphoneIndex == 3; }) && d.getProject().tracks.back().trackId == published.trackId, "Stop did not hide the empty stereo track or dropped it from the project");
    });
    suite.test("enabled but unavailable camera two has no growing recording clip", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); const auto before = d.snapshot();
        UserSettings settings; settings.cameraEnabled = {true, true}; settings.physicalInputs = {0, 1};
        v.setRecordingPreview(true, 20 * 48000, 48000, {{true, false}, {}}, settings); v.refresh(true, 21 * 48000, {});
        require(TimelineUxTestAccess::tracks(v).size() == 4 && TimelineUxTestAccess::recordingRow(v, 0), "Camera one disappeared");
        for (std::size_t row = 1; row < 4; ++row) require(!TimelineUxTestAccess::recordingRow(v, row), "Unready camera or unarmed microphone looked live");
        paint(v); require(d.snapshot() == before, "Camera failure preview changed document");
    });
    suite.test("edit menu displays current split and marker bindings with reserved history keys", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); RecorderShortcuts shortcuts;
        shortcuts.keys[std::size_t(RecorderCommand::split)] = "ctrl + shift + X";
        shortcuts.keys[std::size_t(RecorderCommand::marker)] = "F8"; require(shortcuts.validate().wasOk(), "Custom shortcuts invalid"); v.setShortcuts(shortcuts);
        require(TimelineUxTestAccess::menuLabel(v, TimelineAction::split).endsWith("(ctrl + shift + X)")
            && TimelineUxTestAccess::menuLabel(v, TimelineAction::addMarker).endsWith("(F8)"), "Menu retained default shortcuts");
        require(TimelineUxTestAccess::menuLabel(v, TimelineAction::undo).endsWith("(Ctrl+Z)")
            && TimelineUxTestAccess::menuLabel(v, TimelineAction::redo).endsWith("(Ctrl+Shift+Z)"), "Reserved history shortcuts changed");
        v.setShortcuts({}); require(TimelineUxTestAccess::menuLabel(v, TimelineAction::split).endsWith("(S)"), "Reset binding not reflected");
    });
    suite.test("shortcut defaults, modified-key identity, text focus, duplicates and reserved keys", []
    {
        RecorderShortcuts keys; require(keys.validate().wasOk(), "Default shortcuts invalid");
        require(shortcutCommand(keys, juce::KeyPress(juce::KeyPress::F9Key), nullptr) == RecorderCommand::recordStart, "F9 default");
        require(!shortcutCommand(keys, juce::KeyPress('M', juce::ModifierKeys::ctrlModifier, 0), nullptr), "Ctrl+M incorrectly matched M");
        juce::TextEditor text; juce::Component child; text.addChildComponent(child);
        require(!shortcutCommand(keys, juce::KeyPress(juce::KeyPress::F10Key), &text) && !shortcutCommand(keys, juce::KeyPress('M'), &child), "Typing invoked global shortcut");
        keys.keys[0] = "ctrl + shift + R";
        require(keys.validate().wasOk() && shortcutCommand(keys, juce::KeyPress::createFromDescription(keys.keys[0]), nullptr) == RecorderCommand::recordStart, "Modified shortcut missing");
        keys.keys[1] = "CTRL + SHIFT + r"; require(keys.validate().failed(), "Duplicate accepted");
        for (const auto* reserved : {"escape", "delete", "ctrl + Z", "ctrl + shift + Z", "alt + F4", "tab"})
        { keys = {}; keys.keys[0] = reserved; require(keys.validate().failed(), "Reserved key accepted"); }
    });
    suite.test("shortcut settings persist JUCE descriptions, restore legacy defaults and reject corrupt duplicates", []
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-shortcuts-" + juce::Uuid().toString());
        RecorderSettings settings(root); UserSettings value; value.shortcuts.keys[0] = juce::KeyPress('R', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0).getTextDescription();
        require(settings.set(value).wasOk() && settings.save().get().wasOk(), "Shortcut save");
        RecorderSettings loaded(root); require(loaded.load().wasOk() && loaded.get().shortcuts.keys == value.shortcuts.keys, "Shortcut round trip");
        const auto xml = juce::parseXML(settings.getFile().loadFileAsString()); require(xml != nullptr, "Saved XML");
        juce::PropertySet legacy; legacy.restoreFromXml(*xml);
        for (std::size_t i = 0; i < RecorderShortcuts::count; ++i) legacy.removeValue(RecorderShortcuts::field(RecorderCommand(i)));
        require(settings.getFile().replaceWithText(legacy.createXml("RECORDER_SETTINGS")->toString()), "Legacy fixture write");
        require(loaded.load().wasOk() && loaded.get().shortcuts.keys == RecorderShortcuts{}.keys, "Legacy defaults missing");
        legacy.setValue("shortcutRecordStart", "spacebar"); require(settings.getFile().replaceWithText(legacy.createXml("RECORDER_SETTINGS")->toString()), "Invalid fixture write");
        require(loaded.load().failed() && loaded.get().shortcuts.keys == RecorderShortcuts{}.keys, "Failed load damaged working bindings");
    });
    return suite.result("timeline-ux");
}
