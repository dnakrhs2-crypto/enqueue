#include <juce_gui_extra/juce_gui_extra.h>
#include "TestSupport.h"
#include "ui/TimelineView.h"
#include "ui/TimelineView.automation.h"
#include "ui/RecordView.h"
#include "ui/ShortcutSettingsPanel.h"
#include <limits>

namespace gocue::recorder
{
struct TimelineUxTestAccess
{
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
void paint(TimelineView& view)
{
    juce::Image image(juce::Image::ARGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType{});
    juce::Graphics graphics(image); view.paintEntireComponent(graphics, true);
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
    suite.test("3px jitter selects without opening a drag or changing history; 4px starts a preview", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        auto* rows = rowsOf(v); const auto x = xAt(v, 4), y = 66.0f; const auto snapshot = d.snapshot();
        rows->mouseDown(mouse(*rows, x, y, x, y)); rows->mouseDrag(mouse(*rows, x + 3, y, x, y));
        require(!v.edits.dragPreview(), "Subthreshold move creates preview"); rows->mouseUp(mouse(*rows, x + 3, y, x, y));
        require(d.snapshot() == snapshot && d.getSelection().size() == 4, "Click moved media or lost linked selection");
        rows->mouseDown(mouse(*rows, x, y, x, y)); rows->mouseDrag(mouse(*rows, x + 4, y, x, y));
        require(v.edits.dragPreview() != nullptr, "4px drag did not start"); require(d.snapshot() == snapshot, "Preview mutated document");
        v.clearCaches(); require(!v.edits.dragPreview(), "Reset retained drag");
    });
    suite.test("linked movement keeps original tracks when pointer crosses lanes and commits once", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        auto* rows = rowsOf(v); const auto x = xAt(v, 4), endX = xAt(v, 5); const auto before = d.snapshot();
        rows->mouseDown(mouse(*rows, x, 66, x, 66)); rows->mouseDrag(mouse(*rows, endX, 138, x, 66));
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
                rows->mouseDown(mouse(*rows, x, 66, x, 66));
                require(!v.edits.dragPreview(), "mouseDown must defer editing");
                if (change == 0) require(v.invoke(TimelineAction::undo).wasOk(), "Intervening undo");
                else if (change == 1) { d.setSelection({other}); v.refresh(false, 0, {}); }
                else require(v.invoke(TimelineAction::remove).wasOk(), "Intervening delete");
                const auto after = d.snapshot(); const auto selection = d.getSelection(); const auto depth = d.getHistory().undoDepth();
                rows->mouseDrag(mouse(*rows, x + 4, 66, x, 66));
                require(!v.edits.dragPreview(), "Changed mouseDown target opened a drag");
                rows->mouseUp(mouse(*rows, x + 4, 66, x, 66));
                require(d.snapshot() == after && d.getSelection() == selection && d.getHistory().undoDepth() == depth,
                    "Delayed gesture changed the intervening document/selection/history");
            }
        });
    suite.test("delayed multi-selection collapse does not overwrite a replacement selection", []
    {
        RecorderDocument d; adopt(d); TimelineView v(d); v.setSize(1180, 620); v.refresh(false, 0, {});
        const auto first = d.getProject().tracks[0].clips.items()[0].clipId, second = d.getProject().tracks[0].clips.items()[1].clipId;
        v.edits.clickClip(first); v.edits.clickClip(second, false, true);
        auto* rows = rowsOf(v); const auto x = xAt(v, 4); rows->mouseDown(mouse(*rows, x, 66, x, 66));
        v.edits.clickClip(second); const auto snapshotSelection = d.getSelection(); const auto before = d.snapshot();
        rows->mouseUp(mouse(*rows, x, 66, x, 66));
        require(d.getSelection() == snapshotSelection && d.snapshot() == before, "mouseUp restored a stale selection");
        v.edits.clickClip(first, false, true); v.refresh(true, 0, {});
        rows->mouseDown(mouse(*rows, x, 66, x, 66)); rows->mouseUp(mouse(*rows, x, 66, x, 66));
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
        require(TimelineInteraction::snapTolerance(std::numeric_limits<double>::quiet_NaN(), 48000, 1) == 1, "NaN tolerance");
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
        // One sample-area pixel makes an eight-pixel snap tolerance exceed INT64_MAX.
        v.setSize(503, 620); v.zoomToFit(); auto* rows = rowsOf(v);
        const auto x = xAt(v, double(clip.timelineStartSample) / before->Fs);
        rows->mouseDown(mouse(*rows, x, 66, x, 66)); rows->mouseDrag(mouse(*rows, x + 4, 66, x, 66));
        require(v.edits.dragPreview() != nullptr, "Extreme pointer did not exercise updateDrag"); paint(v);
        v.clearCaches(); rows->mouseUp(mouse(*rows, x + 4, 66, x, 66));
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
        rows->mouseDown(mouse(*rows, x, 66, x, 66)); rows->mouseDrag(mouse(*rows, x + 20, 66, x, 66)); const auto before = d.snapshot();
        d.setRecordingStructureLock(true); v.refresh(true, 0, {}); rows->mouseUp(mouse(*rows, x + 20, 66, x, 66));
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
        require(TimelineUxTestAccess::tracks(v).size() == 2 && d.snapshot() == before, "Stopped preview left a microphone track");
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
        require(TimelineUxTestAccess::tracks(v).size() == 5 && !TimelineUxTestAccess::recordingRow(v, 4), "Stop removed actual stereo track");
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
        legacy.setValue("shortcutRecordStart", "F10"); require(settings.getFile().replaceWithText(legacy.createXml("RECORDER_SETTINGS")->toString()), "Invalid fixture write");
        require(loaded.load().failed() && loaded.get().shortcuts.keys == RecorderShortcuts{}.keys, "Failed load damaged working bindings");
    });
    return suite.result("timeline-ux");
}
