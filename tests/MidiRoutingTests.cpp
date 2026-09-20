#include "MidiTestHarness.h"
#include "ui/MidiModalScope.h"
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
using namespace midi_test;
class MidiRoutingTests : public juce::UnitTest
{
public:
    MidiRoutingTests() : UnitTest ("MIDI routing, shared GO and panic", "Enqueue") {}
    void runTest() override
    {
        beginTest ("first note-on routes all 78 commands exactly once without a preceding off or manufactured KeyPress");
        int count = 0;
        for (const auto& entry : ShortcutCatalog::get().getCommands())
        {
            Harness h;
            expect (h.service->setMidiTriggers (entry.id, { note() }).wasOk(), entry.id);
            h.send (on()); h.send (on()); h.send (on());
            if (entry.commandID == CommandIDs::panicAll) { expectEquals (static_cast<int> (h.panics.size()), 1); expectEquals (h.downs (entry.commandID), 0); }
            else { expectEquals (h.downs (entry.commandID), 1, entry.id); if (! h.target.invocations.empty()) expect (! h.target.invocations.back().keyPress.isValid()); }
            h.target.disabledCommands.insert (entry.commandID); h.tap();
            if (entry.commandID == CommandIDs::panicAll) expectEquals (static_cast<int> (h.panics.size()), 1);
            else expectEquals (h.downs (entry.commandID), 1, "disabled " + entry.id);
            ++count;
        }
        expectEquals (count, 78);

        beginTest ("same command overlapping aliases execute once; command conflicts reject/move whole bindings and failed save rolls back");
        {
            Harness h;
            expect (h.service->setMidiTriggers ("transport.preview", { note(), note (60, 0) }).wasOk());
            h.tap(); expectEquals (h.downs (CommandIDs::preview), 1);
            expect (h.service->setMidiTriggers ("transport.go", { note() }).failed());
            const auto previous = h.service->getMidiProfile();
            h.failSave = true;
            expect (h.service->setMidiTriggers ("transport.go", { note() }, ShortcutService::ConflictPolicy::move).failed());
            expect (h.service->getMidiProfile() == previous);
            h.failSave = false;
            expect (h.service->setMidiTriggers ("transport.go", { note() }, ShortcutService::ConflictPolicy::move).wasOk());
            expect (h.service->getMidiTriggers ("transport.preview").empty());
            const int saves = h.inputSaves;
            expect (h.service->setMidiTriggers ("transport.go", { note(), note() }).wasOk()); expectEquals (h.inputSaves, saves);
            h.service->setEditingLocked (true);
            expect (h.service->setMidiTriggers ("transport.go", {}).failed());
            expect (h.service->setMidiInputSettings ({}).failed());
        }

        beginTest ("scope table: main/text/auxiliary/plugin/background/modal; disabled owners never release to a cue");
        {
            Harness h;
            h.service->setMidiTriggers ("transport.preview", { note (60) });
            const auto* edit = ShortcutCatalog::get().find (CommandIDs::saveProject);
            expect (edit != nullptr);
            h.service->setMidiTriggers (edit->id, { note (61) });
            h.service->setMidiTriggers ("transport.panicAll", { note (62) });
            const auto test = [&] (ShortcutKeyContext::Window window, bool active, bool text, bool modal, bool background, bool playback, bool editing)
            {
                MidiInputSettings settings; settings.allowBackgroundPlayback = background; h.service->setMidiInputSettings (settings);
                h.context.window = window; h.context.applicationActive = active; h.context.textEditing = text; h.context.modal = modal;
                const int beforePlay = h.downs (CommandIDs::preview), beforeEdit = h.downs (CommandIDs::saveProject), beforePanic = static_cast<int> (h.panics.size());
                h.tap (60); h.tap (61); h.tap (62);
                expectEquals (h.downs (CommandIDs::preview) - beforePlay, playback ? 1 : 0);
                expectEquals (h.downs (CommandIDs::saveProject) - beforeEdit, editing ? 1 : 0);
                expectEquals (static_cast<int> (h.panics.size()) - beforePanic, 1);
            };
            using W = ShortcutKeyContext::Window;
            test (W::main, true, false, false, true, true, true);
            test (W::main, true, true, false, true, true, false);
            test (W::auxiliary, true, false, false, true, true, false);
            test (W::nativePlugin, true, false, false, true, true, false);
            test (W::outsideApp, false, false, false, true, true, false);
            test (W::outsideApp, false, false, false, false, false, false);
            test (W::main, true, false, true, true, false, false);
            test (W::modal, true, false, false, true, false, false);
            h.context = {};
            h.target.disabledCommands.insert (CommandIDs::preview);
            Cue c; c.midiTriggers = { note (60, 0) }; h.document.cues.add (c);
            const int before = h.downs (CommandIDs::preview);
            h.tap(); h.send (off (60, 2)); h.now += 25; h.send (on (60, 100, 2));
            expectEquals (h.downs (CommandIDs::preview), before); expect (h.cues.empty());
            expect (h.service->setMidiTriggers ("transport.preview", {}).wasOk());
            h.tap(); expectEquals (static_cast<int> (h.cues.size()), 1); expect (h.cues.back() == c.id);
            expect (! MidiModalScope::active());
            { MidiModalScope guard; expect (MidiModalScope::active()); }
            expect (! MidiModalScope::active());
        }

        beginTest ("all-list/cart cue conflicts disable both bindings, preserve data and leave nonconflicting bindings active");
        {
            Harness h;
            Cue a; a.midiTriggers = { note (60, 0), note (70) }; h.document.cues.add (a);
            const int cart = h.document.addContainer ("Cart", true); h.document.setActiveContainer (cart);
            Cue b; b.midiTriggers = { note() }; h.document.cues.add (b);
            h.document.setActiveContainer (0);
            h.tap(); expect (h.cues.empty()); h.tap (70); expect (h.cues == std::vector<juce::Uuid> { a.id });
            expectEquals (static_cast<int> (h.document.getMidiTriggers().size()), 3);
            expect (h.document.setMidiTriggers (b.id, {}).wasOk()); h.tap(); expectEquals (static_cast<int> (h.cues.size()), 2);
            h.document.markClean(); h.service->setMidiTriggers ("transport.go", { note() }); expect (! h.document.isDirty());
        }
        testGo();
        testPanic();
        testBoundaries();
        testCueController();
    }
private:
    void testGo()
    {
        beginTest ("common keyboard down/up retains native token and observation times, including delayed character delivery");
        for (bool releaseBeforeCharacter : { false, true })
        {
            struct RecordingTarget : shortcut_test::CatalogTarget
            {
                RecordingTarget() : CatalogTarget (ShortcutCatalog::get()) {}
                bool perform (const InvocationInfo& info) override
                {
                    if (const auto* input = service->currentInvocation()) inputs.push_back (*input);
                    return CatalogTarget::perform (info);
                }
                ShortcutService* service = nullptr;
                std::vector<InputInvocation> inputs;
            } target;
            juce::ApplicationCommandManager manager;
            manager.registerAllCommandsForTarget (&target); manager.setFirstCommandTarget (&target);
            ShortcutService service (manager, {}); target.service = &service;
            ShortcutRouter::Callbacks callbacks;
            callbacks.applicationActive = [] { return true; };
            callbacks.keyDown = callbacks.nativeKeyDown = [] (int) { return true; };
            ShortcutRouter keyboard (service, manager, callbacks);
            juce::Component origin;
            keyboard.attach (origin, ShortcutKeyContext::Window::main);
            keyboard.prepareNativeEvent (32, 0, true, false, 1234.0);
            if (releaseBeforeCharacter) keyboard.prepareNativeEvent (32, 0, false, false, 1240.0);
            expect (keyboard.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), &origin));
            if (! releaseBeforeCharacter) keyboard.prepareNativeEvent (32, 0, false, false, 1240.0);
            keyboard.keyStateChanged (false, &origin);
            expectEquals (static_cast<int> (target.inputs.size()), 2);
            if (target.inputs.size() == 2)
            {
                expect (target.inputs[0].active && ! target.inputs[1].active);
                expect (! (target.inputs[0].token < target.inputs[1].token) && ! (target.inputs[1].token < target.inputs[0].token));
                expectEquals (target.inputs[0].token.control, -32);
                expectEquals (target.inputs[0].observedTimeMs, 1234.0);
                expectEquals (target.inputs[1].observedTimeMs, 1240.0);
                expect (target.inputs[0].eventID != target.inputs[1].eventID);
                expect (target.invocations[0].keyPress == juce::KeyPress (juce::KeyPress::spaceKey));
            }
        }
        beginTest ("keyboard + Note + gate held GO group, partial releases, pulse cannot bypass requireKeyUp");
        for (bool requireUp : { false, true })
        {
            Harness h; h.requireKeyUp = requireUp;
            auto pulse = cc (MidiTrigger::Edge::both, MidiTrigger::Behavior::pulse, 65);
            expect (h.service->setMidiTriggers ("transport.go", { note(), cc(), pulse }).wasOk());
            std::set<int> down;
            ShortcutRouter::Callbacks callbacks; callbacks.keyDown = [&] (int code) { return down.count (code) != 0; };
            callbacks.nativeKeyDown = [] (int) { return false; }; callbacks.requireGoKeyUp = [requireUp] { return requireUp; };
            ShortcutRouter keyboard (*h.service, h.manager, callbacks);
            h.send (off()); h.send (control (0)); h.send (control (0, 65));
            down.insert (juce::KeyPress::spaceKey);
            keyboard.route (juce::KeyPress (juce::KeyPress::spaceKey), nullptr, {}, h.now);
            h.now += 25; h.send (on()); h.now += 25; h.send (control (127)); h.now += 25; h.send (control (127, 65));
            expectEquals (h.downs (CommandIDs::go), requireUp ? 1 : 4);
            down.clear(); keyboard.pollKeyState(); expect (h.service->activations().anyHeld());
            h.send (off()); expect (h.service->activations().anyHeld());
            h.now += 25; h.send (control (0, 65)); expect (h.service->activations().anyHeld());
            expectEquals (h.downs (CommandIDs::go), requireUp ? 1 : 5);
            h.send (control (0)); expect (! h.service->activations().anyHeld());
            h.now += 25; h.send (control (127, 65));
            expectEquals (h.downs (CommandIDs::go), requireUp ? 2 : 6); expect (! h.service->activations().anyHeld());
        }
        beginTest ("MIDI held GO survives mapping removal until off, disabled aliases and disconnect release");
        {
            Harness h; h.requireKeyUp = true;
            h.service->setMidiTriggers ("transport.go", { note(), cc() });
            h.send (off()); h.send (control (0)); h.now += 25; h.send (on());
            h.service->setMidiTriggers ("transport.go", { cc() });
            h.now += 25; h.send (control (127)); expectEquals (h.downs (CommandIDs::go), 1);
            h.send (off()); expect (h.service->activations().anyHeld());
            h.send (control (0)); expect (! h.service->activations().anyHeld());
            h.now += 25; h.send (control (127)); expectEquals (h.downs (CommandIDs::go), 2);
            h.router->connectionChanged (1, 1, false); expect (! h.service->activations().anyHeld());
        }
        beginTest ("retired GO mapping cannot acquire new presses after its old release; same-CC pulse cannot release a gate");
        {
            Harness h; h.requireKeyUp = true;
            auto oppositePulse = cc (MidiTrigger::Edge::falling, MidiTrigger::Behavior::pulse);
            h.service->setMidiTriggers ("transport.go", { note(), cc(), oppositePulse });
            h.send (off()); h.send (control (0)); h.now += 25; h.send (on());
            h.service->setMidiTriggers ("transport.go", { cc(), oppositePulse });
            h.send (off()); h.now += 25; h.send (on());
            expect (! h.service->activations().anyHeld());
            h.now += 225; h.send (control (127)); expect (h.service->activations().anyHeld());
            h.send (control (127)); expect (h.service->activations().anyHeld());
            h.now += 25; h.send (control (0)); expect (! h.service->activations().anyHeld());
            expectEquals (h.downs (CommandIDs::go), 3);
        }
    }
    void testPanic()
    {
        beginTest ("panic shared 499/500/501ms boundary, mixed MIDI/native/UI, duplicates and reversed delivery times");
        for (int gap : { 499, 500, 501 })
        {
            Harness h; h.service->setMidiTriggers ("transport.panicAll", { note() });
            PanicKeyHook hook (*h.service, [&] (double time, bool hard) { h.panicTimes.push_back (time); h.panics.push_back (hard); });
            h.send (off()); h.now = 2000; h.send (on()); h.send (on()); h.send (off());
            hook.fromUi (2000.0 + gap);
            expectEquals (static_cast<int> (h.panics.size()), 2); expect (! h.panics[0]); expect (h.panics[1] == (gap <= 500));
            h.service->panicGestures().invalidate(); h.panics.clear();
            hook.fromUi (4000.0 + gap); // observed later, delivered first
            h.now = 4000; h.send (on());
            expectEquals (static_cast<int> (h.panics.size()), 2); expect (h.panics[1] == (gap <= 500));
            const auto duplicate = h.service->panicGestures().activate (999999, 9000);
            expect (duplicate.has_value()); expect (! h.service->panicGestures().activate (999999, 9000).has_value());
        }
        beginTest ("CC panic repeats/releases do not count; capture and mapping boundaries reset gesture history");
        {
            Harness h; h.service->setMidiTriggers ("transport.panicAll", { cc() });
            h.send (control (0)); h.now += 25; h.send (control (127)); h.send (control (127)); h.send (control (126)); h.send (control (0));
            expectEquals (static_cast<int> (h.panics.size()), 1);
            int token = 0; h.service->beginCapture (&token); h.service->endCapture (&token);
            h.now += 25; h.send (control (127)); expectEquals (static_cast<int> (h.panics.size()), 2); expect (! h.panics.back());
            h.send (control (0)); h.service->setKeys ("transport.go", { juce::KeyPress::F13Key });
            h.now += 25; h.send (control (127)); expect (! h.panics.back());
            h.send (control (127, 120)); h.send (control (0, 123)); expectEquals (static_cast<int> (h.panics.size()), 3);
        }
        beginTest ("native hook observation and MIDI share a gesture even when async native delivery is reversed");
        {
            Harness h; h.service->setMidiTriggers ("transport.panicAll", { note() });
            PanicKeyHook hook (*h.service, [&] (double time, bool hard) { h.panicTimes.push_back (time); h.panics.push_back (hard); });
            h.send (off());
            const auto native = hook.observe (0x1b, 0, true, false, 1000);
            expect (native.has_value());
            if (native)
            {
                hook.dispatch (*native); h.now = 1400; h.send (on());
               #if JUCE_WINDOWS
                MSG message {};
                while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage (&message); DispatchMessageW (&message); }
               #endif
                expectEquals (static_cast<int> (h.panics.size()), 2); expect (! h.panics.front() && h.panics.back());
            }
        }
    }
    void testBoundaries()
    {
        beginTest ("capture owns all input; stale tokens cannot end a session; old queued on/off cannot fire new mappings");
        Harness h;
        h.service->setMidiTriggers ("transport.go", { note() }); h.service->setMidiTriggers ("transport.panicAll", { note (61) });
        h.send (off()); h.send (off (61));
        auto beforeCapture = h.event (on());
        int token = 0, other = 1, captured = 0;
        h.service->beginCapture (&token);
        h.service->setMidiCaptureReceiver (&token, [&] (const MidiInputEvent&, const juce::String&) { ++captured; });
        h.send (on()); h.send (off()); h.send (on (61)); h.send (off (61));
        expectEquals (captured, 4); expectEquals (h.downs (CommandIDs::go), 0); expect (h.panics.empty());
        h.service->endCapture (&other); expect (h.service->isCapturing());
        const auto during = h.event (on());
        h.service->endCapture (&token);
        h.router->route (beforeCapture, "A"); h.router->route (during, "A"); expectEquals (h.downs (CommandIDs::go), 0);
        h.send (off()); h.now += 25; h.send (on()); expectEquals (h.downs (CommandIDs::go), 1);
        const auto beforeMap = h.event (on (62));
        h.service->setMidiTriggers ("transport.preview", { note (62) }); h.router->route (beforeMap, "A");
        expectEquals (h.downs (CommandIDs::preview), 0);
        h.send (off (62)); h.now += 25; h.send (on (62)); expectEquals (h.downs (CommandIDs::preview), 1);
        auto beforeProject = h.event (on (62)); h.document.newProject(); h.router->route (beforeProject, "A");
        expectEquals (h.downs (CommandIDs::preview), 1);
        h.router->connectionChanged (1, 2, true);
        h.router->route (h.event (off (62)), "A"); h.router->route (h.event (on (62)), "A");
        expectEquals (h.downs (CommandIDs::preview), 1);
        auto fresh = message (off (62), h.now, 1, 2, h.service->getInputGeneration()); h.router->route (fresh, "A");
        fresh = message (on (62), h.now + 25, 1, 2, h.service->getInputGeneration()); h.router->route (fresh, "A");
        expectEquals (h.downs (CommandIDs::preview), 2);

        beginTest ("captured off/baseline seeds a newly registered mapping; pulse waits for 200ms quiet");
        Harness learned; int capture = 0;
        learned.service->beginCapture (&capture); learned.send (on (70)); learned.send (off (70));
        learned.send (control (0, 70));
        auto pulse = cc (MidiTrigger::Edge::both, MidiTrigger::Behavior::pulse, 70);
        learned.failSave = true;
        expect (learned.service->setMidiTriggers ("transport.preview", { note (70), pulse }).failed());
        expect (learned.service->isCapturing() && learned.service->getMidiTriggers ("transport.preview").empty());
        learned.failSave = false;
        expect (learned.service->setMidiTriggers ("transport.preview", { note (70), pulse }).wasOk());
        learned.service->endCapture (&capture);
        learned.now += 25; learned.send (on (70)); expectEquals (learned.downs (CommandIDs::preview), 1);
        learned.send (control (127, 70)); expectEquals (learned.downs (CommandIDs::preview), 1);
        learned.now += 200; learned.send (control (0, 70)); expectEquals (learned.downs (CommandIDs::preview), 2);
    }
    void testCueController()
    {
        beginTest ("real cue ID execution shares hotkey prewait/sequence/second trigger/armed/playhead across lists and cart");
        Harness h;
        AudioEngine engine (0); engine.prepare (44100, 128);
        double seconds = 10;
        Scheduler scheduler ([&] { return seconds; }); CueController controller (engine, h.document, scheduler);
        h.document.settings.doubleGoSeconds = 0.0;
        h.onCue = [&] (const juce::Uuid& id, const InputInvocation& input) { controller.triggerCueById (id, input); };
        Cue first; first.name = "first"; first.hotkey = "A"; first.preWaitSeconds = 2.0; first.continueMode = ContinueMode::autoContinue;
        first.secondTrigger = SecondTriggerAction::nothing; first.midiTriggers = { note() };
        Cue second; second.preWaitSeconds = 3.0;
        h.document.cues.add (first); h.document.cues.add (second);
        const int remote = h.document.addContainer ("Remote", false); h.document.setActiveContainer (remote);
        h.now = seconds * 1000; h.tap();
        expect (controller.hasPendingFor (first.id)); expect (controller.hasPendingFor (second.id));
        const int pending = controller.getNumPending();
        h.tap(); expectEquals (controller.getNumPending(), pending); expectEquals (h.document.cues.getPlayheadIndex(), -1);
        controller.cancelPending();
        h.document.listContaining (first.id)->update (0, [] (Cue& cue) { cue.armed = false; });
        h.tap(); expectEquals (controller.getNumPending(), 0);
        h.document.listContaining (first.id)->update (0, [] (Cue& cue) { cue.armed = true; });
        expect (controller.handleHotkey (juce::KeyPress ('A'))); expectEquals (controller.getNumPending(), pending);
        controller.cancelPending();
        const int cart = h.document.addContainer ("Cart", true); h.document.setActiveContainer (cart);
        Cue cartCue; cartCue.preWaitSeconds = 1; cartCue.midiTriggers = { note (65) }; h.document.cues.add (cartCue);
        h.document.setActiveContainer (remote); h.tap (65); expect (controller.hasPendingFor (cartCue.id));
        controller.panicAll (false); h.tap(); expectEquals (controller.getNumPending(), 0);

        beginTest ("double GO shares observation time across keyboard/MIDI and per-cue hotkey/cart calls");
        controller.resetForNewProject(); seconds = 20; h.document.settings.doubleGoSeconds = 0.5; h.document.settings.doubleGoHotkeys = true;
        h.document.setActiveContainer (0);
        expect (controller.go (false, 20) != CueController::GoResult::rejectedDoubleGo); controller.goKeyReleased();
        expect (controller.go (false, 20.499) == CueController::GoResult::rejectedDoubleGo);
        expect (controller.go (false, 20.5) != CueController::GoResult::rejectedDoubleGo); controller.goKeyReleased();
        controller.cancelPending(); controller.resetForNewProject();
        InputInvocation invocation; invocation.kind = InputKind::midi; invocation.observedTimeMs = 30000;
        expect (controller.triggerCueById (first.id, invocation));
        int rejected = 0; controller.onGoRejected = [&] { ++rejected; };
        seconds = 30.1; expect (controller.handleHotkey (juce::KeyPress ('A'))); expectEquals (rejected, 1);
        expect (controller.fire (first.id) == CueController::GoResult::rejectedDoubleGo); expectEquals (rejected, 2);
        invocation.observedTimeMs = 30100; expect (controller.triggerCueById (cartCue.id, invocation)); expectEquals (rejected, 2);
        controller.cancelPending();
    }
};
static MidiRoutingTests midiRoutingTests;
}
