#include "ui/MainComponent.h"
#include "ui/TimelineView.automation.h"
#include "model/SafeFileWrite.h"
#include "../tests/StabilityTestAccess.h"
#include <random>
#include <numeric>

namespace gocue::recorder
{
namespace
{
void need(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
class CutEditStressWindow final : public juce::DocumentWindow, private juce::Timer
{
public:
    CutEditStressWindow(juce::var config, std::function<void(int)> done)
        : DocumentWindow("Recorder cut-edit stress", Palette::background, closeButton), configuration(std::move(config)),
          completion(std::move(done)), reportFile(configuration["report"].toString()),
          settings(reportFile.getParentDirectory().getChildFile("settings")), random(unsigned(int(configuration["seed"])))
    {
        count = int(configuration["iterations"]); need(count >= 1 && count <= 10000, "Invalid iteration count (1..10000)");
        need(reportFile.getParentDirectory().createDirectory().wasOk(), "Cannot create report directory");
        auto user = settings.get(); user.cameraEnabled = {false, false}; settings.set(user);
        const juce::File project(configuration["project"].toString());
        const auto opened = document.openCheckpoint(project); need(opened.wasOk(), opened.getErrorMessage().toRawUTF8());
        original = document.snapshot();
        need(hasEditableClip(*original), "Project needs an active clip of at least half a second");
        log = reportFile.getSiblingFile("operations.jsonl").createOutputStream(); need(log && log->openedOk(), "Cannot open operation log");
        need(log->setPosition(0) && log->truncate().wasOk(), "Cannot reset operation log");
        main = std::make_unique<MainComponent>(document, settings);
        auto& session = StabilityTestAccess::session(*main);
        session.projectChanged(); StabilityTestAccess::openAudio(session);
        clock = std::make_unique<StabilityAudioClock>(session);
        setUsingNativeTitleBar(true); setContentNonOwned(main.get(), true); setResizable(true, false); centreWithSize(1380, 960); setVisible(true);
        StabilityTestAccess::tab(*main, true); view().zoomToFit();
        reproduce = bool(configuration["reproduceNegativeDrag"]);
        std::iota(bag.begin(), bag.end(), 0); std::shuffle(bag.begin(), bag.end(), random);
        startTimer(100);
    }
    ~CutEditStressWindow() override
    { stopTimer(); juce::PopupMenu::dismissAllActiveMenus(); clock.reset(); clearContentComponent(); main.reset(); }
    void closeButtonPressed() override { finish("CANCELLED", "Window closed"); }
private:
    static bool hasEditableClip(const RecorderProject& project)
    {
        for (const auto& track : project.tracks) for (const auto& clip : track.clips.items())
            if (project.isActive(clip) && clip.lengthSamples >= Sample(project.Fs / 2)) return true;
        return false;
    }
    TimelineView& view() { return StabilityTestAccess::timeline(*main); }
    RecorderSession& session() { return StabilityTestAccess::session(*main); }
    void writeLog(const juce::String& stage)
    {
        auto entry = juce::var(new juce::DynamicObject()); auto* r = entry.getDynamicObject();
        r->setProperty("iteration", iteration); r->setProperty("operation", name); r->setProperty("phase", phase); r->setProperty("stage", stage);
        r->setProperty("revision", document.getProject().editRevision); r->setProperty("playing", session().playing());
        r->setProperty("playbackReady", session().showingPlayback()); r->setProperty("cursor", session().playhead());
        r->setProperty("sessionError", session().error); r->setProperty("time", juce::Time::getCurrentTime().toISO8601(true));
        r->setProperty("explicitSelection", int(view().edits.explicitSelection().size()));
        r->setProperty("selectedClips", int(document.getSelection().size()));
        log->writeText(juce::JSON::toString(entry, true) + "\n", false, false, "\n"); log->flush();
        need(log->getStatus().wasOk(), "Cannot write operation log");
    }
    void select()
    {
        StabilityTestAccess::tab(*main, true); view().zoomToFit();
        const auto snapshot = document.snapshot();
        const Clip* found = nullptr;
        for (const auto& track : snapshot->tracks) for (const auto& clip : track.clips.items())
            if (snapshot->isActive(clip) && (!found || clip.lengthSamples > found->lengthSamples)) found = &clip;
        if (!found || found->lengthSamples < Sample(snapshot->Fs / 2))
        {
            name = "restore-project"; writeLog("before");
            need(document.adopt(*original, document.getFile(), {}).wasOk(), "restore");
            session().projectChanged(); view().clearCaches(); StabilityTestAccess::refresh(*main); view().zoomToFit();
            select(); return;
        }
        selected = *found;
        row = 0;
        for (const auto& track : snapshot->tracks)
        {
            if (track.trackId == selected.trackId) break;
            ++row;
        }
        // The view always reserves cam1/cam2, even in single-camera projects.
        if (snapshot->tracks[row].kind == TrackKind::mic || snapshot->tracks[row].kind == TrackKind::importAudio)
        { row = 2; for (const auto& track : snapshot->tracks) { if (track.trackId == selected.trackId) break; if (track.kind == TrackKind::mic || track.kind == TrackKind::importAudio) ++row; } }
        down = {StabilityTestAccess::x(view(), selected.timelineStartSample + selected.lengthSamples / 2), float(TimelineLayout::rulerHeight + row * TimelineLayout::rowHeight + 40)};
        StabilityTestAccess::mouse(view(), 0, down, down); StabilityTestAccess::mouse(view(), 2, down, down);
        // Alternate real running/stopped output before edit actions. The following
        // timer phase lets plan preparation and the synthetic callback advance.
        if ((iteration / int(bag.size())) % 2 == 0) { session().scrub(selected.timelineStartSample + selected.lengthSamples / 2, true); session().play(); }
        else session().stopPlayback();
    }
    void begin()
    {
        if (iteration % int(bag.size()) == 0) std::shuffle(bag.begin(), bag.end(), random);
        operation = reproduce ? 0 : bag[std::size_t(iteration) % bag.size()];
        static const char* names[]{"negative-move", "scrub", "select", "multi-select", "split-S", "trim-in", "trim-out", "move", "move-alt", "snap-off", "delete", "ripple", "earlier", "later", "marker-M", "undo", "redo", "edit-button", "context-menu", "unlink", "zoom", "scroll", "latest-take", "tabs", "split-button", "cancel-drag"};
        name = names[operation]; writeLog("before"); select(); name = names[operation];
        revisionBefore = document.getProject().editRevision;
    }
    void act()
    {
        using A = StabilityTestAccess;
        auto& v = view(); const auto mid = selected.timelineStartSample + selected.lengthSamples / 2;
        const auto key = [&](int code, int mods = 0) { A::key(v, code, mods); };
        switch (operation)
        {
            case 0: case 5: case 6: case 7: case 8: case 9: case 25:
            {
                A::snap(v, operation != 9); modifiers = operation == 8 ? juce::ModifierKeys::altModifier : 0;
                if (operation == 5) down.x = A::x(v, selected.timelineStartSample);
                if (operation == 6) down.x = A::x(v, selected.timelineEnd());
                target = down; target.x += operation == 6 ? -12.0f : 12.0f;
                if (operation == 0) target.x = float(TimelineLayout::headerWidth + 1);
                A::mouse(v, 0, down, down, modifiers); break;
            }
            case 1:
                down.y = target.y = 12; target.x = float(TimelineLayout::headerWidth + 20); A::mouse(v, 0, down, down); break;
            case 2: A::mouse(v, 0, down, down); A::mouse(v, 2, down, down); break;
            case 3:
            {
                int audioRow = 2;
                const auto snapshot = document.snapshot();
                for (const auto& track : snapshot->tracks)
                {
                    const auto lane = track.kind == TrackKind::cam1 ? 0 : track.kind == TrackKind::cam2 ? 1 : audioRow++;
                    for (const auto& clip : track.clips.items()) if (snapshot->isActive(clip) && clip.clipId != selected.clipId)
                    {
                        const juce::Point<float> other{A::x(v, clip.timelineStartSample + clip.lengthSamples / 2), float(30 + lane * 72 + 40)};
                        A::mouse(v, 0, other, other, juce::ModifierKeys::ctrlModifier); A::mouse(v, 2, other, other, juce::ModifierKeys::ctrlModifier);
                        return;
                    }
                }
                break;
            }
            case 4: v.edits.followPlayhead(mid); key('S'); break;
            case 10: key(juce::KeyPress::deleteKey); break;
            case 11:
                down = {A::x(v, selected.timelineStartSample + selected.lengthSamples / 3), 12};
                target = {A::x(v, selected.timelineStartSample + selected.lengthSamples * 2 / 3), 12};
                modifiers = juce::ModifierKeys::shiftModifier; A::mouse(v, 0, down, down, modifiers); break;
            case 12: A::button(v, TimelineAction::earlier); break;
            case 13: A::button(v, TimelineAction::later); break;
            case 14: key('M'); break;
            case 15: key('Z', juce::ModifierKeys::ctrlModifier); break;
            case 16: key('Z', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier); break;
            case 17: A::menu(v); break;
            case 18: A::mouse(v, 0, down, down, juce::ModifierKeys::rightButtonModifier); break;
            case 19: A::button(v, TimelineAction::unlink); break;
            case 20: A::wheel(v, .25f, juce::ModifierKeys::ctrlModifier); A::wheel(v, -.25f, juce::ModifierKeys::ctrlModifier); break;
            case 21: A::wheel(v, -.25f, juce::ModifierKeys::shiftModifier); v.revealTrack(unsigned(row)); A::wheel(v, -.25f, 0); break;
            case 22: A::latest(*main); break;
            case 23: A::tab(*main, false); break;
            case 24: v.edits.followPlayhead(mid); A::button(v, TimelineAction::split); break;
        }
    }
    bool dragging() const { return operation <= 1 || (operation >= 5 && operation <= 9) || operation == 11 || operation == 25; }
    void timerCallback() override
    {
        // Intentionally do not catch UI exceptions here: the baseline must expose
        // the real unhandled-exception exit, with its last operation flushed.
        if (iteration >= count)
        {
            const bool exercised = clock->callbacks.load() > 0 && prepared > 0 && view().rowPaintCount > 0;
            finish(exercised ? "PASS" : "FAIL", exercised ? juce::String() : "Playback or timeline painting was not observed"); return;
        }
        if (phase == 0) begin();
        writeLog("before-phase");
        if (phase == 1) act();
        if (phase == 2 && dragging()) StabilityTestAccess::mouse(view(), 1, target, down, modifiers);
        if (phase == 3)
        {
            if (operation == 25) StabilityTestAccess::key(view(), juce::KeyPress::escapeKey);
            if (dragging()) StabilityTestAccess::mouse(view(), 2, target, down, modifiers);
            if (operation == 11) StabilityTestAccess::button(view(), TimelineAction::rippleAll);
            if (operation == 17 || operation == 18)
            {
                // Exercise the asynchronous menu result callback, including its
                // captured snapshot/selection guard, through JUCE's modal input.
                if (auto* menu = juce::Component::getCurrentlyModalComponent())
                { menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey)); menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)); }
                juce::PopupMenu::dismissAllActiveMenus();
            }
            if (operation == 23) StabilityTestAccess::tab(*main, true);
            const auto valid = document.getProject().validate();
            if (valid.failed()) { finish("FAIL", valid.getErrorMessage()); return; }
            if (session().error.isNotEmpty()) { finish("FAIL", session().error); return; }
            ++coverage[name];
            if (document.getProject().editRevision > revisionBefore) ++effectiveEdits[name];
            if (view().edits.explicitSelection().size() > 1) ++multipleSelections;
            if (session().showingPlayback()) ++prepared;
            if (session().playing()) ++playing;
            writeLog("after"); ++iteration; phase = 0; modifiers = 0; return;
        }
        ++phase;
    }
    void finish(const juce::String& status, const juce::String& reason)
    {
        stopTimer(); juce::PopupMenu::dismissAllActiveMenus();
        auto report = juce::var(new juce::DynamicObject()); auto* r = report.getDynamicObject();
        r->setProperty("status", status); r->setProperty("reason", reason); r->setProperty("iterations", iteration);
        r->setProperty("seed", configuration["seed"]); r->setProperty("project", configuration["project"]);
        r->setProperty("requestedIterations", count); r->setProperty("lastOperation", name); r->setProperty("lastPhase", phase);
        r->setProperty("audioCallbacks", int(clock->callbacks.load())); r->setProperty("playbackReadyOperations", prepared); r->setProperty("playingOperations", playing);
        r->setProperty("rowPaintCount", int(view().rowPaintCount)); r->setProperty("input", "JUCE production mouse/key/button callbacks; real MainComponent and media; synthetic output clock; no capture/ASIO");
        auto counts = juce::var(new juce::DynamicObject()); for (const auto& item : coverage) counts.getDynamicObject()->setProperty(item.first, item.second); r->setProperty("coverage", counts);
        auto changed = juce::var(new juce::DynamicObject()); for (const auto& item : effectiveEdits) changed.getDynamicObject()->setProperty(item.first, item.second); r->setProperty("effectiveEdits", changed);
        r->setProperty("multipleSelectionOperations", multipleSelections);
        const auto written = gocue::SafeFileWrite::writeTextVerified(reportFile, juce::JSON::toString(report));
        if (written.failed()) juce::Logger::writeToLog(written.getErrorMessage());
        const auto shot = main->createComponentSnapshot(main->getLocalBounds());
        if (auto out = reportFile.getSiblingFile("final.png").createOutputStream()) juce::PNGImageFormat().writeImageToStream(shot, *out);
        completion(written.failed() ? 2 : status == "PASS" ? 0 : 1);
    }
    juce::var configuration;
    std::function<void(int)> completion;
    juce::File reportFile;
    RecorderSettings settings;
    RecorderDocument document;
    RecorderDocument::Snapshot original;
    std::unique_ptr<MainComponent> main;
    std::unique_ptr<StabilityAudioClock> clock;
    std::unique_ptr<juce::FileOutputStream> log;
    std::mt19937 random;
    std::array<int, 26> bag{};
    std::map<juce::String, int> coverage, effectiveEdits;
    Clip selected;
    juce::Point<float> down, target;
    juce::String name;
    int count = 0, iteration = 0, phase = 0, operation = 0, modifiers = 0, row = 0, prepared = 0, playing = 0;
    int multipleSelections = 0;
    Sample revisionBefore = 0;
    bool reproduce = false;
};
}
std::unique_ptr<juce::DocumentWindow> createCutEditStressWindow(const juce::var& config, std::function<void(int)> completion)
{
    try { return std::make_unique<CutEditStressWindow>(config, completion); }
    catch (const std::exception& e)
    {
        auto report = juce::var(new juce::DynamicObject());
        report.getDynamicObject()->setProperty("status", "STARTUP_FAILED");
        report.getDynamicObject()->setProperty("reason", juce::String::fromUTF8(e.what()));
        const auto written = gocue::SafeFileWrite::writeTextVerified(juce::File(config["report"].toString()), juce::JSON::toString(report));
        if (written.failed()) juce::Logger::writeToLog(written.getErrorMessage());
        completion(2); return {};
    }
}
}
