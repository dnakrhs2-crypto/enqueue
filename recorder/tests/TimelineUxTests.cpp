#include <juce_gui_extra/juce_gui_extra.h>
#include "TestSupport.h"
#include "ui/TimelineView.h"
#include "ui/TimelineView.automation.h"
#include "ui/RecordView.h"
#include "ui/ShortcutSettingsPanel.h"

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
            timeline.setBounds(root.timelineBounds()); timeline.setRecordingPreview(true, 12 * 48000, 3 * 48000, s); timeline.refresh(true, 15 * 48000, {});
            int hosts = 0;
            std::function<void(juce::Component&)> inspect = [&](juce::Component& c)
            {
                if (auto* host = dynamic_cast<juce::HWNDComponent*>(&c)) { ++hosts; const auto box = root.getLocalArea(host, host->getLocalBounds()); require(box.getHeight() >= 45 && box.getBottom() < timeline.getY(), "Preview hidden behind timeline"); }
                for (auto* child : c.getChildren()) inspect(*child);
            };
            inspect(root); require(hosts == 2 && timeline.getHeight() >= 240 && root.stopButton.isEnabled() && root.markerButton.isEnabled(), "Timeline recording controls unavailable");
            const auto snapshot = d.snapshot(); const auto image = root.createComponentSnapshot(root.getLocalBounds());
            require(image.isValid() && d.snapshot() == snapshot, "Growing clip mutated document"); timeline.setRecordingPreview(false, 0, 0, s);
        }
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
