#pragma once
#include "ShortcutTestHarness.h"
#include "app/Commands.h"
#include "app/MidiTriggerRouter.h"
#include "app/CueController.h"
#include "ui/ShortcutRouter.h"

namespace gocue::midi_test
{
inline MidiTrigger note (int number = 60, int channel = 1, juce::String source = "any")
{
    MidiTrigger t;
    t.number = number; t.channel = channel; t.source = std::move (source);
    return t;
}
inline MidiTrigger cc (MidiTrigger::Edge edge = MidiTrigger::Edge::rising, MidiTrigger::Behavior behavior = MidiTrigger::Behavior::gate, int number = 64)
{
    auto t = note (number);
    t.kind = MidiTrigger::Kind::cc; t.edge = edge; t.behavior = behavior; t.lowThreshold = 60;
    return t;
}
inline MidiInputEvent message (const juce::MidiMessage& m, double time = 1000, uint64_t port = 1, uint64_t connection = 1, uint64_t routing = 0)
{
    MidiInputEvent e;
    MidiInputEvent::copyMessage (m, e);
    e.input = port; e.connection = connection; e.routing = routing; e.observedTimeMs = time; e.eventID = InputInvocation::nextEventID();
    return e;
}
inline juce::MidiMessage on (int n = 60, int velocity = 100, int ch = 1) { return juce::MidiMessage::noteOn (ch, n, static_cast<juce::uint8> (velocity)); }
inline juce::MidiMessage off (int n = 60, int ch = 1) { return juce::MidiMessage::noteOff (ch, n); }
inline juce::MidiMessage control (int value, int n = 64, int ch = 1) { return juce::MidiMessage::controllerEvent (ch, n, value); }

struct Harness : shortcut_test::Harness
{
    Harness()
    {
        service->setInputStorage ([this] (const InputSettingsTransaction& transaction)
        {
            ++inputSaves;
            if (onInputSave) onInputSave();
            if (failSave) return juce::Result::fail ("injected input storage failure");
            lastTransaction = transaction;
            return juce::Result::ok();
        });
        MidiTriggerRouter::Callbacks callbacks;
        callbacks.clockMs = [this] { return now; };
        callbacks.context = [this] { return context; };
        callbacks.requireGoKeyUp = [this] { return requireKeyUp; };
        callbacks.cue = [this] (const juce::Uuid& id, const InputInvocation& input) { cues.push_back (id); if (onCue) onCue (id, input); };
        callbacks.panic = [this] (double time, bool hard) { panicTimes.push_back (time); panics.push_back (hard); };
        router = std::make_unique<MidiTriggerRouter> (*service, manager, document, std::move (callbacks));
        router->connectionChanged (1, 1, true);
        router->connectionChanged (2, 1, true);
    }
    MidiInputEvent event (const juce::MidiMessage& m, uint64_t port = 1, uint64_t generation = 0)
    { return message (m, now, port, 1, generation != 0 ? generation : service->getInputGeneration()); }
    bool send (const juce::MidiMessage& m, uint64_t port = 1, const juce::String& identifier = "A")
    { return router->route (event (m, port), identifier); }
    void tap (int n = 60, uint64_t port = 1) { send (off (n), port); now += 25; send (on (n), port); now += 25; send (off (n), port); }
    int downs (juce::CommandID id) const
    { return static_cast<int> (std::count_if (target.invocations.begin(), target.invocations.end(), [id] (const auto& info) { return info.commandID == id && info.isKeyDown; })); }
    ProjectDocument document;
    std::unique_ptr<MidiTriggerRouter> router;
    double now = 1000;
    bool requireKeyUp = false;
    MidiRoutingContext context;
    std::vector<juce::Uuid> cues;
    std::vector<bool> panics;
    std::vector<double> panicTimes;
    int inputSaves = 0;
    InputSettingsTransaction lastTransaction;
    std::function<void()> onInputSave;
    std::function<void (const juce::Uuid&, const InputInvocation&)> onCue;
};
struct FakeDevices
{
    struct Port { juce::MidiInputCallback* callback = nullptr; int opens = 0, starts = 0, stops = 0; bool fail = false; std::function<void()> onStop; };
    struct Handle : MidiInputService::Handle
    {
        explicit Handle (Port& p) : port (p) {}
        void start() override { ++port.starts; }
        void stop() override { ++port.stops; if (port.onStop) port.onStop(); port.callback = nullptr; }
        Port& port;
    };
    MidiInputService::Backend backend (bool fakeClock = true)
    {
        MidiInputService::Backend b;
        b.nativeNotifications = false;
        b.enumerate = [this] { ++enumerations; return list; };
        b.open = [this] (const juce::String& id, juce::MidiInputCallback* callback) -> std::unique_ptr<MidiInputService::Handle>
        {
            auto& p = ports[id]; ++p.opens;
            if (p.fail) return {};
            p.callback = callback;
            return std::make_unique<Handle> (p);
        };
        if (fakeClock) b.clockMs = [this] { return now.load(); };
        return b;
    }
    void send (const juce::String& id, const juce::MidiMessage& m) { if (auto* cb = ports[id].callback) cb->handleIncomingMidiMessage (nullptr, m); }
    std::vector<juce::MidiDeviceInfo> list;
    std::map<juce::String, Port> ports;
    std::atomic<double> now { 1000 };
    int enumerations = 0;
};
inline void drain (MidiInputService& input, double now = -1)
{
    while (input.hasPending()) input.drain (now);
}
}
