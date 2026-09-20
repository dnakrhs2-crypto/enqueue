#include "app/MidiTriggerRules.h"

namespace gocue
{
bool MidiInputEvent::copyMessage (const juce::MidiMessage& message, MidiInputEvent& e) noexcept
{
    // Only short MIDI 1.0 messages are copied, so SysEx can never allocate here.
    if (message.getRawDataSize() != 3 || message.getRawData()[1] > 127 || message.getRawData()[2] > 127) return false;
    if (message.isNoteOnOrOff())
    {
        e.kind = MidiTrigger::Kind::note;
        e.number = message.getNoteNumber();
        e.value = message.getVelocity();
        e.noteOn = message.isNoteOn();
    }
    else if (message.isController())
    {
        e.kind = MidiTrigger::Kind::cc;
        e.number = message.getControllerNumber();
        e.value = message.getControllerValue();
        e.noteOn = false;
    }
    else return false;
    e.channel = message.getChannel();
    return true;
}
bool MidiTriggerRules::intersects (const MidiTrigger& a, const MidiTrigger& b)
{
    if (a.kind != b.kind || a.number != b.number || (a.source != "any" && b.source != "any" && a.source != b.source)
        || (a.channel != 0 && b.channel != 0 && a.channel != b.channel)) return false;
    return a.kind == MidiTrigger::Kind::note || a.edge == b.edge || a.edge == MidiTrigger::Edge::both || b.edge == MidiTrigger::Edge::both;
}
bool MidiTriggerRules::matchesAddress (const MidiTrigger& t, const MidiInputEvent& e, const juce::String& identifier)
{
    return t.kind == e.kind && t.number == e.number && (t.channel == 0 || t.channel == e.channel) && (t.source == "any" || t.source == identifier);
}
MidiTriggerRules::Transition MidiTriggerRules::observe (const MidiTrigger& t, const MidiInputEvent& e, bool allow)
{
    auto& s = states[e.token()];
    s.pulse = t.kind == MidiTrigger::Kind::cc && t.behavior == MidiTrigger::Behavior::pulse;
    Transition r;
    const bool previousDown = s.down;
    bool candidate = false;
    if (t.kind == MidiTrigger::Kind::note)
    {
        const bool down = e.noteOn && e.value != 0;
        r.changed = down != s.down;
        if (! down) { s.ready = true; s.quarantined = false; }
        candidate = down && ! s.down && s.ready && e.value >= t.minVelocity;
        s.down = down; // includes on below minimum velocity, and blocked/captured input
        s.raw = e.value;
    }
    else
    {
        r.changed = s.raw != e.value;
        const bool settled = s.quarantined && e.observedTimeMs - s.lastChange >= 200.0;
        if (settled && s.pulse) s.quarantined = false;
        if (! r.changed) return { false, false, s.down, s.ready && ! s.quarantined, false };
        s.lastChange = e.observedTimeMs;
        const int next = e.value >= t.highThreshold ? 1 : e.value <= t.lowThreshold ? 0 : s.region;
        const bool first = s.raw < 0 || s.region < 0;
        candidate = ! first && next != s.region && (t.edge == MidiTrigger::Edge::both
            || (next == 1 && t.edge == MidiTrigger::Edge::rising) || (next == 0 && t.edge == MidiTrigger::Edge::falling));
        s.region = next;
        s.raw = e.value;
        s.ready = next >= 0;
        s.down = t.behavior == MidiTrigger::Behavior::gate && next >= 0
            && (t.edge == MidiTrigger::Edge::rising ? next == 1 : next == 0);
        if (! s.down && t.behavior == MidiTrigger::Behavior::gate) s.quarantined = false;
    }
    r.held = s.down;
    r.released = previousDown && ! s.down;
    r.ready = s.ready && ! s.quarantined;
    r.activated = candidate && allow && r.ready && e.observedTimeMs - s.lastFire >= t.debounceMs;
    if (r.activated) s.lastFire = e.observedTimeMs;
    return r;
}
void MidiTriggerRules::quarantine (double time)
{
    for (auto& [token, s] : states)
    {
        juce::ignoreUnused (token);
        s.quarantined = s.pulse || s.down;
        s.lastChange = time;
    }
}
void MidiTriggerRules::forgetInput (uint64_t input)
{
    for (auto i = states.begin(); i != states.end();)
        if (i->first.source == input) i = states.erase (i); else ++i;
}
}
