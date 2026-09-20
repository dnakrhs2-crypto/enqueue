#include "MidiTestHarness.h"
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
using namespace midi_test;
namespace
{
// Use the same physical GO down/up adapter as MainComponent, with a real
// controller and wait cue so acceptance is observable without an audio device.
struct ControllerHarness : Harness
{
    struct Target : shortcut_test::CatalogTarget
    {
        explicit Target (ControllerHarness& h) : CatalogTarget (ShortcutCatalog::get()), harness (h) {}
        bool perform (const InvocationInfo& info) override
        {
            if (info.commandID != CommandIDs::go) return CatalogTarget::perform (info);
            const auto* input = harness.service->currentInvocation();
            invocations.push_back (info);
            if (input != nullptr) inputs.push_back (*input);
            if (info.isKeyDown)
                results.push_back (harness.controller.go (false, input != nullptr ? input->observedTimeMs * 0.001 : -1.0));
            else if (harness.service->activations().consumeRelease())
            {
                harness.controller.goKeyReleased();
                ++releases;
            }
            return true;
        }
        ControllerHarness& harness;
        std::vector<CueController::GoResult> results;
        std::vector<InputInvocation> inputs;
        int releases = 0;
    };

    ControllerHarness() : scheduler ([this] { return now * 0.001; }), controller (engine, document, scheduler), goTarget (*this)
    {
        requireKeyUp = true;
        manager.setFirstCommandTarget (&goTarget);
        configureProject();
    }
    ~ControllerHarness() override
    {
        router.reset();
        manager.setFirstCommandTarget (&target);
    }
    void configureProject()
    {
        document.settings.requireKeyUp = true;
        document.settings.doubleGoSeconds = 0.0;
        Cue cue;
        cue.type = CueType::control;
        cue.control.kind = ControlKind::wait;
        cue.control.seconds = 10.0;
        document.cues.add (cue);
        document.cues.setPlayheadIndex (0);
    }
    void replaceProject (bool adopt)
    {
        controller.resetForNewProject();
        if (adopt)
        {
            Project project;
            project.settings.requireKeyUp = true;
            document.adopt (std::move (project), {});
        }
        else document.newProject();
        configureProject();
    }
    AudioEngine engine { 0 };
    Scheduler scheduler;
    CueController controller;
    Target goTarget;
};
}

class MidiGoTests : public juce::UnitTest
{
public:
    MidiGoTests() : UnitTest ("MIDI GO hold and controller boundaries", "Enqueue") {}
    void runTest() override
    {
        testAliases();
        testProjectRelease();
        testMappingCarryOver();
        testInputRemoval();
        testFirstNotes();
        testNoteQuarantine();
        testRetiredMappingRestore();
        testRetiredGateHysteresisRestore();
        testQuietPortFault();
        testCaptureRelease();
    }
private:
    void testFirstNotes()
    {
        for (const auto* boundary : { "connect", "reconnect", "project replace", "new project", "input loss" })
        {
            beginTest (juce::String ("first note-on after ") + boundary + " fires GO, panic and cue through the input service");
            ControllerHarness h; FakeDevices backend; backend.list = { { "A", "A" } };
            MidiInputSettings settings; settings.selected["A"] = "A";
            expect (h.service->setMidiInputSettings (settings).wasOk());
            expect (h.service->setMidiTriggers ("transport.go", { note() }).wasOk());
            expect (h.service->setMidiTriggers ("transport.panicAll", { note (61) }).wasOk());
            expect (h.service->setMidiTriggers ("transport.preview", { cc() }).wasOk());
            MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
            const juce::String change (boundary);
            if (change != "connect")
            {
                // The boundary must reset previously prepared addresses too;
                // in particular an old CC low must not execute the new high.
                backend.send ("A", off()); backend.send ("A", off (61)); backend.send ("A", control (0));
                drain (input, backend.now);
            }
            if (change == "reconnect")
            {
                backend.list.clear(); input.refresh();
                backend.list = { { "A", "A" } }; input.refresh();
            }
            else if (change == "project replace" || change == "new project") h.replaceProject (change == "project replace");
            else if (change == "input loss")
            {
                for (int i = 0; i < 8192; ++i) backend.send ("A", on (90));
                for (int i = 0; i < 512; ++i) backend.send ("A", off (61));
                backend.send ("A", on (61)); // force a reserved loss, resetting both rule paths
                expectEquals (static_cast<int> (input.counters().panicDropped), 1);
                drain (input, backend.now);
            }
            const auto cue = h.document.cues.get (0).id;
            expect (h.document.setMidiTriggers (cue, { note (62) }).wasOk());
            h.onCue = [&] (const juce::Uuid& id, const InputInvocation& invocation)
            { expect (h.controller.triggerCueById (id, invocation)); };
            backend.now = h.now = 1100;
            for (int n : { 60, 61, 62 })
            {
                backend.send ("A", on (n)); backend.send ("A", on (n));
                drain (input, backend.now);
            }
            expect (h.goTarget.results == std::vector { CueController::GoResult::started });
            expectEquals (static_cast<int> (h.panics.size()), 1);
            expect (h.panics.empty() || ! h.panics.front());
            expect (h.cues == std::vector { cue });
            expect (! h.controller.getRunningWaits().empty());
            expect (input.devices()[0].status == MidiInputService::Status::connected);
            backend.send ("A", control (127)); backend.send ("A", control (127)); drain (input, backend.now);
            expectEquals (h.goTarget.invocationCount (CommandIDs::preview), 0, "first CC value remains a baseline");
            backend.send ("A", control (0)); backend.now = 1125; backend.send ("A", control (127)); drain (input, backend.now);
            expectEquals (h.goTarget.invocationCount (CommandIDs::preview), 1);
        }
    }
    void testNoteQuarantine()
    {
        for (const auto* boundary : { "capture", "mapping", "project replace", "new project" })
        {
            beginTest (juce::String ("held notes across ") + boundary + " remain quarantined for GO, panic and cue until off");
            Harness h;
            expect (h.service->setMidiTriggers ("transport.go", { note() }).wasOk());
            expect (h.service->setMidiTriggers ("transport.panicAll", { note (61) }).wasOk());
            Cue cue; cue.midiTriggers = { note (62) }; h.document.cues.add (cue);
            for (int n : { 60, 61, 62, 63 }) { h.send (off (n)); h.now += 25; h.send (on (n)); }
            expectEquals (h.downs (CommandIDs::go), 1);
            expectEquals (static_cast<int> (h.panics.size()), 1);
            expectEquals (static_cast<int> (h.cues.size()), 1);
            const juce::String change (boundary);
            if (change == "capture")
            {
                int token = 0; h.service->beginCapture (&token); h.service->endCapture (&token);
            }
            else if (change == "mapping")
                expect (h.service->setMidiTriggers ("transport.go", { note(), note (64) }).wasOk());
            else
            {
                if (change == "project replace") h.document.adopt (Project(), {});
                else h.document.newProject();
                // A new cue ID and a previously unmapped held address both inherit quarantine.
                cue = Cue(); cue.midiTriggers = { note (62), note (63) }; h.document.cues.add (cue);
                expect (! h.service->activations().anyHeld(), "project replacement releases the old MIDI GO group");
            }
            h.now += 25;
            for (int n : { 60, 61, 62, 63 }) h.send (on (n));
            expectEquals (h.downs (CommandIDs::go), 1);
            expectEquals (static_cast<int> (h.panics.size()), 1);
            expectEquals (static_cast<int> (h.cues.size()), 1);
            for (int n : { 60, 61, 62 }) { h.send (off (n)); h.now += 25; h.send (on (n)); }
            expectEquals (h.downs (CommandIDs::go), 2);
            expectEquals (static_cast<int> (h.panics.size()), 2);
            expectEquals (static_cast<int> (h.cues.size()), 2);
        }
    }
    void testRetiredMappingRestore()
    {
        for (bool gate : { false, true })
        {
            beginTest (juce::String ("retired GO ") + (gate ? "gate" : "Note") + " restoration carries B hold until both ports release");
            Harness h; h.requireKeyUp = true;
            const auto trigger = gate ? cc() : note();
            expect (h.service->setMidiTriggers ("transport.go", { trigger, note (65) }).wasOk());
            h.send (gate ? control (0) : off()); h.now += 25; h.send (gate ? control (127) : on());
            expectEquals (h.downs (CommandIDs::go), 1);
            expect (h.service->setMidiTriggers ("transport.go", { note (65) }).wasOk());
            h.send (gate ? control (127) : on(), 2, "B");
            expect (h.service->setMidiTriggers ("transport.go", { trigger, note (65) }).wasOk());
            expectEquals (h.downs (CommandIDs::go), 1, "restoration only synchronises state");
            h.send (gate ? control (0) : off());
            expect (h.service->activations().anyHeld(), "B remains physically held after A releases");
            h.tap (65); expectEquals (h.downs (CommandIDs::go), 1);
            h.send (gate ? control (0) : off(), 2, "B");
            expect (! h.service->activations().anyHeld());
            h.tap (65); expectEquals (h.downs (CommandIDs::go), 2);
        }
    }
    void testRetiredGateHysteresisRestore()
    {
        for (bool falling : { false, true })
            for (bool releaseBeforeRestore : { false, true })
            {
                beginTest (juce::String ("retired GO gate hysteresis restoration: ") + (falling ? "falling 0->62" : "rising 127->62")
                    + (releaseBeforeRestore ? ", A releases before restore" : ", A releases after restore"));
                Harness h; h.requireKeyUp = true;
                const auto trigger = cc (falling ? MidiTrigger::Edge::falling : MidiTrigger::Edge::rising);
                const int pressed = falling ? 0 : 127, released = falling ? 64 : 60;
                expect (h.service->setMidiTriggers ("transport.go", { trigger, note (65) }).wasOk());
                h.send (control (released)); h.now += 25; h.send (control (pressed));
                expectEquals (h.downs (CommandIDs::go), 1);
                expect (h.service->setMidiTriggers ("transport.go", { note (65) }).wasOk());
                h.send (control (pressed), 2, "B"); h.now += 25; h.send (control (62), 2, "B");
                if (releaseBeforeRestore)
                {
                    h.send (control (released));
                    expect (! h.service->activations().anyHeld(), "new retired observations must not acquire GO tokens");
                }
                expectEquals (h.downs (CommandIDs::go), 1, "retired observations never execute GO");
                expect (h.service->setMidiTriggers ("transport.go", { trigger, note (65) }).wasOk());
                expectEquals (h.downs (CommandIDs::go), 1, "restoration never executes GO");
                if (! releaseBeforeRestore) h.send (control (released));
                expect (h.service->activations().anyHeld(), "B stays held inside the hysteresis band after A releases");
                h.tap (65); expectEquals (h.downs (CommandIDs::go), 1, "a mixed Note GO must wait for B's real release");
                h.send (control (62), 2, "B");
                h.tap (65); expectEquals (h.downs (CommandIDs::go), 1, "repeating the middle value is not a release");
                h.send (control (released), 2, "B");
                expect (! h.service->activations().anyHeld());
                h.tap (65); expectEquals (h.downs (CommandIDs::go), 2);
            }
    }
    void testQuietPortFault()
    {
        for (bool notified : { false, true }) testQuietPortFault (notified);
    }
    void testQuietPortFault (bool notified)
    {
        beginTest (juce::String ("quiet port A reserved GO-release loss with B traffic: ") + (notified ? "real notifier" : "manual drain"));
        ControllerHarness h; FakeDevices backend; backend.list = { { "A", "A" }, { "B", "B" } };
        MidiInputSettings settings; settings.autoUseAll = true;
        expect (h.service->setMidiInputSettings (settings).wasOk());
        expect (h.service->setMidiTriggers ("transport.panicAll", { cc() }).wasOk());
        expect (h.service->setMidiTriggers ("transport.go", { cc (MidiTrigger::Edge::falling), note() }).wasOk());
        int ordinaryFaults = 0, panicFaults = 0;
        auto callbacks = h.router->inputCallbacks();
        const auto fault = callbacks.fault;
        callbacks.fault = [&] (uint64_t port, bool panic)
        {
            if (port == 1) ++(panic ? panicFaults : ordinaryFaults);
            fault (port, panic);
        };
        MidiInputService input (*h.service, callbacks, backend.backend (! notified), notified);
        const auto flush = [&]
        {
            if (! notified) { drain (input, backend.now); return; }
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
            while (input.hasPending() && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
               #if JUCE_WINDOWS
                MSG message {};
                while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage (&message); DispatchMessageW (&message); }
                MsgWaitForMultipleObjectsEx (0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
               #endif
            }
            expect (! input.hasPending(), "the notifier drains both packets and pending faults");
        };
        backend.send ("A", control (127)); flush();
        backend.now = 1025; backend.send ("A", control (0)); flush();
        expect (h.goTarget.results == std::vector { CueController::GoResult::started });
        expect (input.devices()[0].status == MidiInputService::Status::connected);
        // Capacity is per port: exhaust A, with B independently pending. No A
        // packet after the lost release is needed to report its fault.
        for (int i = 0; i < 8192; ++i) backend.send ("A", on (90));
        for (int i = 0; i < 512; ++i) backend.send ("A", control (0));
        backend.send ("B", on (90));
        backend.send ("A", control (127)); // the sole A release is lost; A sends nothing further
        expectEquals (static_cast<int> (input.counters().panicDropped), 1);
        expectEquals (static_cast<int> (input.counters().dropped), 0);
        flush();
        expectEquals (ordinaryFaults, 1); expectEquals (panicFaults, 1);
        expect (input.devices()[0].status == MidiInputService::Status::waiting);
        expect (! h.service->activations().anyHeld());
        expectEquals (h.goTarget.releases, 1);
        input.drain (backend.now); input.drain (backend.now);
        expectEquals (ordinaryFaults, 1); expectEquals (panicFaults, 1);
        expectEquals (h.goTarget.releases, 1, "idle drains must not repeat the release");
        h.document.cues.setPlayheadIndex (0);
        backend.send ("B", off()); backend.now = 1050; backend.send ("B", on()); flush();
        expect (h.goTarget.results == std::vector { CueController::GoResult::started, CueController::GoResult::started });
    }
    void testCaptureRelease()
    {
        for (bool midiFirst : { false, true })
        {
            beginTest (juce::String ("capture defers keyboard key-up but releases the real GO controller once: ")
                + (midiFirst ? "MIDI first" : "keyboard first"));
            ControllerHarness h;
            expect (h.service->setMidiTriggers ("transport.go", { note() }).wasOk());
            bool down = true;
            ShortcutRouter::Callbacks callbacks;
            callbacks.keyDown = [&] (int code) { return down && code == juce::KeyPress::spaceKey; };
            callbacks.nativeKeyDown = [] (int) { return false; };
            callbacks.applicationActive = [] { return true; };
            callbacks.requireGoKeyUp = [] { return true; };
            ShortcutRouter keyboard (*h.service, h.manager, callbacks);
            keyboard.prepareNativeEvent (32, 0, true, false, 1000);
            juce::Component origin; keyboard.attach (origin, ShortcutKeyContext::Window::main);
            keyboard.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), &origin);
            h.send (off()); h.now += 25; h.send (on());
            expect (h.goTarget.results == std::vector { CueController::GoResult::started });
            int capture = 0; h.service->beginCapture (&capture);
            if (midiFirst) h.router->connectionChanged (1, 1, false);
            down = false; keyboard.prepareNativeEvent (32, 0, false, false, 1075); keyboard.keyStateChanged (false, &origin);
            expectEquals (h.goTarget.releases, 0);
            if (! midiFirst) h.router->connectionChanged (1, 1, false);
            expectEquals (h.goTarget.releases, midiFirst ? 0 : 1);
            h.service->endCapture (&capture); keyboard.pollKeyState();
            expectEquals (h.goTarget.releases, 1, "deferred key-up must not repeat the MIDI release");
            int keyboardUps = 0;
            for (size_t i = 0; i < h.goTarget.inputs.size(); ++i)
                if (h.goTarget.inputs[i].kind == InputKind::keyboard && ! h.goTarget.inputs[i].active)
                {
                    ++keyboardUps;
                    expectEquals (h.goTarget.inputs[i].token.control, -32);
                    expectEquals (h.goTarget.inputs[i].observedTimeMs, 1075.0);
                    expect (h.goTarget.invocations[i].keyPress == juce::KeyPress (juce::KeyPress::spaceKey));
                    expect (h.goTarget.invocations[i].invocationMethod == juce::ApplicationCommandTarget::InvocationInfo::fromKeyPress);
                }
            expectEquals (keyboardUps, 1, "the original keyboard key-up is still delivered");
            h.document.cues.setPlayheadIndex (0);
            down = true;
            keyboard.prepareNativeEvent (32, 0, true, false, 1100); keyboard.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), &origin);
            expect (h.goTarget.results == std::vector { CueController::GoResult::started, CueController::GoResult::started });
            down = false; keyboard.prepareNativeEvent (32, 0, false, false, 1150); keyboard.keyStateChanged (false, &origin);
            expectEquals (h.goTarget.releases, 2, "the next physical group has its own release");
        }
    }
    void testAliases()
    {
        for (bool pulseFirst : { true, false })
        {
            beginTest (pulseFirst ? "GO pulse before gate retains the hold and executes once"
                                  : "GO gate before pulse retains the hold and executes once");
            for (bool requireUp : { true, false })
            {
                Harness h; h.requireKeyUp = requireUp;
                const auto pulse = cc (MidiTrigger::Edge::rising, MidiTrigger::Behavior::pulse);
                expect (h.service->setMidiTriggers ("transport.go", pulseFirst ? MidiTriggers { pulse, cc(), note() }
                                                                               : MidiTriggers { cc(), pulse, note() }).wasOk());
                h.send (control (0)); h.send (off()); h.now += 25; h.send (control (127));
                expectEquals (h.downs (CommandIDs::go), 1);
                expect (h.service->activations().anyHeld(), "gate must survive execution deduplication");
                h.tap();
                expectEquals (h.downs (CommandIDs::go), requireUp ? 1 : 2);
                expect (h.service->activations().anyHeld());
                h.send (control (0)); expect (! h.service->activations().anyHeld());
                h.tap(); expectEquals (h.downs (CommandIDs::go), requireUp ? 2 : 3);
            }
        }
        beginTest ("GO gates with different thresholds release only after the last gate");
        for (bool highFirst : { true, false })
        {
            Harness h; h.requireKeyUp = true;
            auto high = cc(); high.highThreshold = 100; high.lowThreshold = 90;
            expect (h.service->setMidiTriggers ("transport.go", highFirst ? MidiTriggers { high, cc(), note() }
                                                                       : MidiTriggers { cc(), high, note() }).wasOk());
            h.send (off()); h.send (control (0)); h.now += 25; h.send (control (127));
            expectEquals (h.downs (CommandIDs::go), 1);
            h.now += 25; h.send (control (80));
            expect (h.service->activations().anyHeld(), "the lower gate is still held");
            h.tap(); expectEquals (h.downs (CommandIDs::go), 1);
            h.send (control (0)); expect (! h.service->activations().anyHeld());
            h.tap(); expectEquals (h.downs (CommandIDs::go), 2);
        }
        beginTest ("retired GO aliases track only existing holds across ports");
        {
            Harness h; h.requireKeyUp = true;
            expect (h.service->setMidiTriggers ("transport.go", { note() }).wasOk());
            h.send (off(), 1); h.send (off(), 2); h.now += 25;
            h.send (on(), 1); h.send (on(), 2);
            expect (h.service->setMidiTriggers ("transport.go", {}).wasOk());
            h.send (off(), 1); h.now += 25; h.send (on(), 1);
            h.send (off(), 2);
            expect (! h.service->activations().anyHeld());
            expectEquals (h.downs (CommandIDs::go), 1);
        }
    }
    void testProjectRelease()
    {
        beginTest ("MIDI GO down - project replacement - off - first GO reaches the real controller");
        for (bool adopt : { false, true })
        {
            ControllerHarness h;
            expect (h.service->setMidiTriggers ("transport.go", { note() }).wasOk());
            h.send (off()); h.now += 25; h.send (on());
            expect (h.goTarget.results == std::vector { CueController::GoResult::started });
            h.replaceProject (adopt);
            expect (! h.service->activations().anyHeld());
            expectEquals (h.goTarget.releases, 1);
            h.send (off()); h.now += 25; h.send (on());
            expect (h.goTarget.results == std::vector { CueController::GoResult::started, CueController::GoResult::started });
            expect (! h.controller.getRunningWaits().empty());
        }
        beginTest ("project replacement preserves keyboard GO until its real key up, including mixed MIDI holds");
        for (bool midiHeld : { false, true })
        {
            ControllerHarness h;
            expect (h.service->setMidiTriggers ("transport.go", { note() }).wasOk());
            bool down = true;
            ShortcutRouter::Callbacks callbacks;
            callbacks.keyDown = [&] (int code) { return down && code == juce::KeyPress::spaceKey; };
            callbacks.nativeKeyDown = [] (int) { return false; };
            callbacks.requireGoKeyUp = [] { return true; };
            ShortcutRouter keyboard (*h.service, h.manager, callbacks);
            keyboard.route (juce::KeyPress (juce::KeyPress::spaceKey), nullptr, {}, h.now);
            if (midiHeld) { h.send (off()); h.now += 25; h.send (on()); }
            h.replaceProject (true);
            expect (h.service->activations().anyHeld());
            expectEquals (h.goTarget.releases, 0);
            h.tap();
            expectEquals (static_cast<int> (h.goTarget.results.size()), 1);
            expect (h.controller.go (false, h.now * 0.001) == CueController::GoResult::rejectedKeyUp);
            down = false; keyboard.pollKeyState();
            expect (! h.service->activations().anyHeld());
            expectEquals (h.goTarget.releases, 1);
            h.tap();
            expect (h.goTarget.results == std::vector { CueController::GoResult::started, CueController::GoResult::started });
        }
    }
    void testMappingCarryOver()
    {
        for (const auto* boundary : { "add", "import", "restore" })
            for (bool gate : { false, true })
            {
                beginTest (juce::String ("held GO ") + (gate ? "CC gate" : "Note 61") + " carries into mapping " + boundary + " without execution");
                Harness h; h.requireKeyUp = true;
                expect (h.service->setMidiTriggers ("transport.go", { note() }).wasOk());
                h.send (off()); h.now += 25; h.send (on());
                h.send (gate ? control (127) : on (61));
                const MidiTriggers triggers { note(), gate ? cc() : note (61) };
                if (juce::String (boundary) == "add")
                    expect (h.service->setMidiTriggers ("transport.go", triggers).wasOk());
                else
                {
                    MidiShortcutProfile profile; profile.overrides["transport.go"] = triggers;
                    juce::String xml;
                    if (juce::String (boundary) == "import")
                    {
                        expect (h.service->getProfile().serialiseExchange (profile, xml).wasOk());
                        expect (h.service->importProfile (xml).wasOk());
                    }
                    else
                    {
                        expect (profile.serialise (xml).wasOk());
                        expect (h.service->restoreMidi (juce::String ("invalid"), xml).source == ShortcutService::RestoreReport::Source::lastGood);
                    }
                }
                expectEquals (h.downs (CommandIDs::go), 1, "mapping changes never execute GO");
                h.send (off());
                expect (h.service->activations().anyHeld());
                h.tap(); expectEquals (h.downs (CommandIDs::go), 1);
                h.send (gate ? control (0) : off (61));
                expect (! h.service->activations().anyHeld());
                h.tap(); expectEquals (h.downs (CommandIDs::go), 2);
            }
    }
    void testInputRemoval()
    {
        for (const auto* boundary : { "disable", "disconnect", "mapping removal" })
        {
            beginTest (juce::String ("last MIDI GO release reaches the real controller after ") + boundary);
            ControllerHarness h; FakeDevices backend; backend.list = { { "A", "A" } };
            MidiInputSettings settings; settings.selected["A"] = "A";
            expect (h.service->setMidiInputSettings (settings).wasOk());
            expect (h.service->setMidiTriggers ("transport.go", { note(), note (61) }).wasOk());
            MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
            backend.send ("A", off()); backend.now = 1025; backend.send ("A", on()); drain (input, backend.now);
            expect (h.goTarget.results == std::vector { CueController::GoResult::started });
            if (juce::String (boundary) == "disable")
            {
                expect (h.service->setMidiInputSettings ({}).wasOk());
                expect (h.service->setMidiInputSettings (settings).wasOk());
            }
            else if (juce::String (boundary) == "disconnect")
            {
                backend.list.clear(); input.refresh();
                backend.list = { { "A", "A" } }; input.refresh();
            }
            else
            {
                expect (h.service->setMidiTriggers ("transport.go", { note (61) }).wasOk());
                expect (h.service->activations().anyHeld());
                expectEquals (h.goTarget.releases, 0);
                backend.send ("A", off()); drain (input, backend.now);
            }
            expect (! h.service->activations().anyHeld());
            expectEquals (h.goTarget.releases, 1);
            backend.send ("A", off (61)); backend.now = 1050; backend.send ("A", on (61)); drain (input, backend.now);
            expect (h.goTarget.results == std::vector { CueController::GoResult::started, CueController::GoResult::started });
        }
    }
};
static MidiGoTests midiGoTests;
}
