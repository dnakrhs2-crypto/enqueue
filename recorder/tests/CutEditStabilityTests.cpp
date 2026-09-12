#include <juce_gui_extra/juce_gui_extra.h>
#include "TestSupport.h"
#include "ui/TimelineView.h"
#include "ui/TimelineView.automation.h"
#include "StabilityTestAccess.h"
#include <limits>

using namespace gocue::recorder;
using namespace recorder_test;

int runCutEditStabilityTests()
{
    Suite suite;
    suite.test("legacy stress model actions work after their toolbar buttons are removed", []
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        for (const auto action : {TimelineAction::rippleAll, TimelineAction::rippleAudio, TimelineAction::earlier,
                                 TimelineAction::later, TimelineAction::unlink, TimelineAction::link})
        {
            RecorderDocument document; require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
            TimelineView view(document); const auto id = document.getProject().tracks[0].clips.items()[0].clipId;
            view.edits.clickClip(id); view.edits.setRange(48000, 96000);
            StabilityTestAccess::button(view, action);
            require(document.getProject().validate().wasOk(), "Legacy model action damaged the project");
            if (action == TimelineAction::later || action == TimelineAction::unlink || action == TimelineAction::rippleAll)
                require(document.getProject().editRevision > 0, "Legacy model action was silently skipped");
        }
        RecorderDocument document; TimelineView view(document); int requests = 0;
        view.onAddMarkerRequested = [&] { ++requests; }; StabilityTestAccess::button(view, TimelineAction::addMarker);
        require(requests == 1, "Retained marker button bypassed the host UI hook");
    });
    for (const auto action : {TimelineAction::move, TimelineAction::trimIn})
        suite.test(action == TimelineAction::move ? "negative move preview paints and rejects commit" : "negative trim preview paints and rejects commit", [action]
        {
            juce::ScopedJuceInitialiser_GUI runtime;
            RecorderDocument document;
            require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
            TimelineView view(document); view.setSize(1180, 620);
            const auto before = document.snapshot();
            view.edits.clickClip(before->tracks[0].clips.items()[0].clipId);
            require(view.edits.beginDrag(action), "begin drag");
            const auto* preview = view.edits.dragTo(-48000, true);
            require(preview && preview->status.failed(), "negative edit must remain rejected");
            view.selectionChanged(); // production ghost rebuild used by Rows::mouseDrag
            require(StabilityTestAccess::previewContains(view, before->tracks[0].clips.items()[0].clipId), "rejected ghost lost");
            juce::Image picture(juce::Image::ARGB, 1180, 620, true, juce::SoftwareImageType{});
            juce::Graphics graphics(picture); view.paintEntireComponent(graphics, true);
            require(view.edits.commitDrag().failed(), "invalid preview committed");
            require(document.snapshot() == before && document.getHistory().undoDepth() == 0, "preview changed original/history");
        });
    suite.test("overflowing linked move preview remains drawable", []
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        RecorderDocument document;
        require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
        TimelineView view(document); view.setSize(1180, 620);
        view.edits.clickClip(document.getProject().tracks[0].clips.items()[0].clipId);
        require(view.edits.beginDrag(TimelineAction::move), "begin drag");
        const auto* preview = view.edits.dragTo((std::numeric_limits<Sample>::max)(), true);
        require(preview && preview->status.failed(), "overflow edit must be rejected");
        view.selectionChanged();
        juce::Image picture(juce::Image::ARGB, 1180, 620, true, juce::SoftwareImageType{});
        juce::Graphics graphics(picture); view.paintEntireComponent(graphics, true);
        require(StabilityTestAccess::previewContains(view, document.getProject().tracks[0].clips.items()[0].clipId), "overflow ghost lost");
        require(view.edits.commitDrag().failed(), "overflow committed");
    });
    suite.test("extreme trim edges preserve a drawable rejected ghost", []
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        RecorderDocument document; require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
        TimelineView view(document); view.setSize(1180, 620);
        const auto before = document.snapshot(); const auto id = before->tracks[0].clips.items()[0].clipId;
        view.edits.clickClip(id);
        for (const auto action : {TimelineAction::trimIn, TimelineAction::trimOut})
            for (const auto edge : {(std::numeric_limits<Sample>::min)(), (std::numeric_limits<Sample>::max)()})
            {
                require(view.edits.beginDrag(action), "begin trim");
                const auto* preview = view.edits.dragTo(edge, true);
                require(preview && preview->status.failed(), "extreme trim accepted");
                view.selectionChanged();
                require(StabilityTestAccess::previewContains(view, id), "extreme ghost lost");
                juce::Image picture(juce::Image::ARGB, 1180, 620, true, juce::SoftwareImageType{});
                juce::Graphics graphics(picture); view.paintEntireComponent(graphics, true);
                require(view.edits.commitDrag().failed(), "extreme trim committed");
            }
        require(document.snapshot() == before && document.getHistory().undoDepth() == 0, "preview changed document/history");
    });
    suite.test("coordinate saturation leaves room for half-open hit queries", []
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        RecorderDocument document; require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
        TimelineView view(document); view.setSize(1180, 620);
        const auto huge = StabilityTestAccess::sample(view, (std::numeric_limits<double>::max)());
        require(huge == (std::numeric_limits<Sample>::max)() - 1 && huge + 1 > huge, "one-past coordinate overflow");
        require(StabilityTestAccess::sample(view, std::numeric_limits<double>::infinity()) == huge, "infinite coordinate");
        require(StabilityTestAccess::sample(view, -std::numeric_limits<double>::infinity()) == 0, "negative coordinate");
        require(StabilityTestAccess::sample(view, std::numeric_limits<double>::quiet_NaN()) == 0, "NaN coordinate");
        require(std::abs(StabilityTestAccess::sample(view, StabilityTestAccess::x(view, 240000)) - 240000) <= 1, "ordinary coordinate changed");
    });
    suite.test("Rows mouse gesture rejects negative drag without changing history", []
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        RecorderDocument document; require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
        TimelineView view(document); view.setSize(1180, 620);
        const auto before = document.snapshot();
        const juce::Point<float> down{StabilityTestAccess::x(view, 240000), 70}, target{215, 70};
        StabilityTestAccess::mouse(view, 0, down, down);
        StabilityTestAccess::mouse(view, 1, target, down);
        StabilityTestAccess::mouse(view, 2, target, down);
        require(document.snapshot() == before && !view.edits.dragPreview(), "invalid mouse gesture committed or remained active");
    });
    suite.test("stale hit-test clip after external publication is harmless", []
    {
        juce::ScopedJuceInitialiser_GUI runtime;
        RecorderDocument document; require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
        TimelineView view(document); view.setSize(1180, 620);
        const auto clip = document.getProject().tracks[0].clips.items()[0];
        require(document.performEdit("external delete", {}, [&](const auto& p) { return ClipEdits::remove(p, {clip.clipId}); }).wasOk(), "delete");
        const auto after = document.snapshot();
        const juce::Point<float> down{StabilityTestAccess::x(view, 240000), 70};
        StabilityTestAccess::mouse(view, 0, down, down); StabilityTestAccess::mouse(view, 2, down, down);
        require(document.snapshot() == after && !view.edits.dragPreview(), "stale hit mutated document");
    });
    suite.test("edit or selection changes during a drag reject its stale commit", []
    {
        RecorderDocument document; require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
        TimelineEditController edits(document); const auto clip = document.getProject().tracks[0].clips.items()[0];
        edits.clickClip(clip.clipId); require(edits.beginDrag(TimelineAction::move), "begin");
        require(edits.dragTo(48000, true)->status.wasOk(), "preview");
        require(document.addMarker({}).wasOk(), "intervening edit"); const auto edited = document.snapshot();
        require(edits.commitDrag().failed() && document.snapshot() == edited, "stale revision committed");
        require(edits.beginDrag(TimelineAction::move), "begin after cancellation"); edits.dragTo(48000, true);
        document.setSelection({}); require(edits.commitDrag().failed(), "stale selection committed");
        require(document.snapshot() == edited, "stale selection changed geometry");
    });
    suite.test("notification reentry cannot edit or replace a publishing document", []
    {
        RecorderDocument document; require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
        const auto before = document.snapshot(); bool visited = false;
        document.onChanged = [&]
        {
            visited = true;
            require(document.undo().failed(), "reentrant undo accepted");
            require(document.performEdit("nested", [](EditState& e) { e.tracks.clear(); }).failed(), "reentrant edit accepted");
            require(document.adopt(*before, {}, {}).failed(), "reentrant replacement accepted");
        };
        require(document.addMarker({}).wasOk() && visited, "publication"); document.onChanged = {};
        require(document.getProject().markers.size() == 1 && document.getHistory().undoDepth() == 1, "history changed under reentry");
    });
    suite.test("late derived results require current project request and media generation", []
    {
        RecorderDocument document; require(document.adopt(makeTimelineUiFixture(), {}, {}).wasOk(), "fixture");
        RecorderSession session(document);
        const auto old = document.snapshot(); const auto asset = old->tracks[2].clips.items()[0].assetId;
        const auto generation = StabilityTestAccess::derivedGeneration(session);
        int delivered = 0; session.onLoadedPeaks = [&](const auto&, auto, unsigned) { ++delivered; };
        session.projectChanged();
        StabilityTestAccess::queueDerived(session, *old, asset, generation); session.tick();
        require(delivered == 0, "old request published into reopened project with identical IDs");
        auto changed = *old->media->findAsset(asset); ++changed.mediaGeneration;
        require(document.updateMediaAsset(changed).wasOk(), "new media generation");
        const auto current = StabilityTestAccess::derivedGeneration(session);
        StabilityTestAccess::queueDerived(session, *old, asset, current); session.tick();
        require(delivered == 0, "stale source generation published");
        auto other = *document.snapshot(); other.projectId = newId();
        StabilityTestAccess::queueDerived(session, other, asset, current); session.tick();
        require(delivered == 0, "foreign project published matching asset ID");
        StabilityTestAccess::queueDerived(session, document.getProject(), asset, current); session.tick();
        require(delivered == 1, "current derived result lost");
    });
    suite.test("failed async preparation retires its future and reports on the owner", []
    {
        RecorderDocument document; RecorderSession session(document);
        StabilityTestAccess::failPreparation(session, std::make_exception_ptr(std::runtime_error("injected plan failure")));
        session.tick();
        require(session.error.contains("injected plan failure") && !session.busy() && !session.playing(), "future failure escaped or remained busy");
        require(!StabilityTestAccess::preparing(session) && session.notice.isEmpty(), "failed future retained playback intent");
        StabilityTestAccess::failPreparation(session, std::make_exception_ptr(42));
        session.requestShutdown(); session.tick();
        require(session.readyForShutdownCommit() && session.error.isNotEmpty(), "unknown exception prevented shutdown barrier");
    });
    suite.test("reported plan error clears latest-take retry and preparation notice", []
    {
        RecorderDocument document; RecorderSession session(document);
        StabilityTestAccess::planError(session); session.tick();
        require(session.error.contains("injected media failure") && !StabilityTestAccess::preparing(session), "failed latest-take preparation kept retrying");
        require(session.notice.isEmpty(), "failure still says preparing");
    });
    return suite.result("cut-edit-stability");
}
