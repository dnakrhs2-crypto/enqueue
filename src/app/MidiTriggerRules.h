#pragma once

#include "model/MidiTrigger.h"
#include "app/InputActivationTracker.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <map>

namespace gocue
{
/** Fixed-size callback payload. No String/MidiMessage allocation in the queue. */
struct MidiInputEvent
{
    uint64_t input = 0, connection = 0, routing = 0, eventID = 0;
    uint64_t faultEpoch = 0;
    uint64_t ordinaryFaultEpoch = 0;
    double observedTimeMs = 0;
    MidiTrigger::Kind kind = MidiTrigger::Kind::note;
    int channel = 1, number = 0, value = 0;
    bool noteOn = false;
    bool panicReserved = false;
    bool ordinaryAllowed = true, ordinaryStateValid = true; // set by the message-thread consumer
    InputToken token() const { return { InputKind::midi, input, connection, (kind == MidiTrigger::Kind::cc ? 2048 : 0) + (channel - 1) * 128 + number }; }
    static bool copyMessage (const juce::MidiMessage&, MidiInputEvent&) noexcept;
};
class MidiTriggerRules
{
public:
    struct Transition
    {
        bool activated = false, released = false, held = false, ready = false, changed = false;
    };
    static bool intersects (const MidiTrigger&, const MidiTrigger&);
    static bool matchesAddress (const MidiTrigger&, const MidiInputEvent&, const juce::String& identifier);
    Transition observe (const MidiTrigger&, const MidiInputEvent&, bool allowActivation = true);
    void quarantine (double timeMs); // capture/mapping boundary; pulse must settle for 200ms
    void forgetInput (uint64_t input);
    bool anyHeld() const { return std::any_of (states.begin(), states.end(), [] (const auto& pair) { return pair.second.down; }); }
    void clear() { states.clear(); }
private:
    struct State
    {
        bool ready = false, down = false, quarantined = false, pulse = false;
        int raw = -1, region = -1;
        double lastFire = -1.0e30, lastChange = -1.0e30;
    };
    std::map<InputToken, State> states;
};
}
