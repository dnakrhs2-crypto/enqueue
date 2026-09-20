#include "MidiTestHarness.h"

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
            if (info.isKeyDown)
                results.push_back (harness.controller.go (false, input != nullptr ? input->observedTimeMs * 0.001 : -1.0));
            else if (! harness.service->activations().anyHeld())
            {
                harness.controller.goKeyReleased();
                ++releases;
            }
            return true;
        }
        ControllerHarness& harness;
        std::vector<CueController::GoResult> results;
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
    }
private:
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
