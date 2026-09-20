#include "app/MidiTriggerRouter.h"
#include "app/Commands.h"
#include <set>

namespace gocue
{
MidiTriggerRouter::MidiTriggerRouter (ShortcutService& s, juce::ApplicationCommandManager& m, ProjectDocument& d, Callbacks c)
    : shortcuts (s), manager (m), document (d), callbacks (std::move (c))
{
    if (! callbacks.clockMs) callbacks.clockMs = [] { return juce::Time::getMillisecondCounterHiRes(); };
    shortcuts.addListener (this);
    document.addListener (this);
    refreshBindings();
}
MidiTriggerRouter::~MidiTriggerRouter()
{
    document.removeListener (this);
    shortcuts.removeListener (this);
    for (const auto& p : connections) releaseGoSource (p.first);
}
MidiInputService::Callbacks MidiTriggerRouter::inputCallbacks()
{
    return { [this] (const MidiInputEvent& e, const juce::String& id, bool execute) { return route (e, id, execute); },
             [this] (uint64_t input, uint64_t connection, bool connected) { connectionChanged (input, connection, connected); },
             [this] (uint64_t input, bool panic) { inputFault (input, panic); }, {} };
}
void MidiTriggerRouter::releaseGo (const InputToken& token, const MidiInputEvent* e)
{
    const bool wasHeld = shortcuts.activations().anyHeld();
    shortcuts.activations().release (token);
    if (wasHeld && ! shortcuts.activations().anyHeld())
    {
        InputInvocation input { InputKind::midi, "transport.go", CommandIDs::go, false, token,
                                e != nullptr ? e->observedTimeMs : callbacks.clockMs(), e != nullptr ? e->eventID : InputInvocation::nextEventID() };
        shortcuts.invokeInput (input);
    }
}
void MidiTriggerRouter::releaseGoSource (uint64_t input)
{
    const bool wasHeld = shortcuts.activations().anyHeld();
    shortcuts.activations().releaseSource (InputKind::midi, input);
    if (wasHeld && ! shortcuts.activations().anyHeld())
    {
        InputInvocation release { InputKind::midi, "transport.go", CommandIDs::go, false, {}, callbacks.clockMs(), InputInvocation::nextEventID() };
        shortcuts.invokeInput (release);
    }
}
void MidiTriggerRouter::connectionChanged (uint64_t input, uint64_t connection, bool connected)
{
    releaseGoSource (input);
    for (auto& runtime : bindings) runtime.rules.forgetInput (input);
    for (auto it = lastObserved.begin(); it != lastObserved.end();)
        if (it->first.source == input) it = lastObserved.erase (it); else ++it;
    if (connected) connections[input] = connection; else connections.erase (input);
    shortcuts.invalidateInputRouting();
}
void MidiTriggerRouter::inputFault (uint64_t input, bool panic)
{
    // Ordinary loss preserves panic. Reserved packets also carry ordinary rules
    // (for example, falling GO on a rising-panic CC), so their loss resets both.
    for (auto& runtime : bindings)
        if (panic || runtime.binding.commandID != CommandIDs::panicAll) runtime.rules.forgetInput (input);
    for (auto it = lastObserved.begin(); it != lastObserved.end();)
        if (it->first.source == input) it = lastObserved.erase (it); else ++it;
    if (panic) shortcuts.panicGestures().invalidate();
    releaseGoSource (input);
}
void MidiTriggerRouter::projectReplaced()
{
    for (auto& runtime : bindings) runtime.rules.clear();
    lastObserved.clear();
    for (const auto& p : connections) releaseGoSource (p.first);
    shortcuts.invalidateInputRouting();
}
void MidiTriggerRouter::captureStateChanged()
{
    for (auto& runtime : bindings) runtime.rules.quarantine (callbacks.clockMs());
}
void MidiTriggerRouter::shortcutsChanged() { refreshBindings(); captureStateChanged(); }
void MidiTriggerRouter::refreshBindings()
{
    if (rebuilding) return;
    const juce::ScopedValueSetter<bool> guard (rebuilding, true);
    auto next = shortcuts.midiCommandBindings();
    std::vector<MidiBinding> nextCues;
    for (const auto& cue : document.getMidiTriggers()) nextCues.push_back ({ cue.id.toString(), cue.trigger, 0, cue.armed });
    next.insert (next.end(), nextCues.begin(), nextCues.end());
    const auto same = [] (const MidiBinding& a, const MidiBinding& b)
    { return a.id == b.id && a.commandID == b.commandID && a.trigger == b.trigger; };
    std::vector<MidiBinding> unique;
    for (const auto& binding : next)
        if (std::none_of (unique.begin(), unique.end(), [&] (const auto& b) { return same (binding, b); })) unique.push_back (binding);
    next = std::move (unique);
    bool changed = next.size() != static_cast<size_t> (std::count_if (bindings.begin(), bindings.end(), [] (const auto& r) { return r.live; }));
    for (const auto& b : next)
        if (std::none_of (bindings.begin(), bindings.end(), [&] (const auto& r) { return r.live && same (r.binding, b) && r.binding.enabled == b.enabled; })) changed = true;
    cues = std::move (nextCues);
    if (! changed) return;
    for (auto& runtime : bindings) runtime.live = false;
    for (const auto& b : next)
    {
        auto found = std::find_if (bindings.begin(), bindings.end(), [&] (const auto& r) { return same (r.binding, b); });
        if (found == bindings.end())
        {
            Runtime runtime { b, {}, true };
            for (const auto& [token, observed] : lastObserved)
            {
                if (MidiTriggerRules::matchesAddress (b.trigger, observed.first, observed.second))
                {
                    const auto state = runtime.rules.observe (b.trigger, observed.first, false);
                    if (b.commandID == CommandIDs::go && state.held) shortcuts.activations().hold (token);
                }
            }
            bindings.push_back (std::move (runtime));
        }
        else { found->live = true; found->binding = b; }
    }
    // Retired GO gates retain their release interpretation across a mapping edit.
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(), [] (const auto& r)
        { return ! r.live && (r.binding.commandID != CommandIDs::go || ! r.rules.anyHeld()); }), bindings.end());
    shortcuts.invalidateInputRouting();
}
bool MidiTriggerRouter::route (const MidiInputEvent& event, const juce::String& identifier, bool execute)
{
    const auto connection = connections.find (event.input);
    if (connection == connections.end() || connection->second != event.connection) return false;
    if (event.ordinaryStateValid) lastObserved[event.token()] = { event, identifier };
    const auto generation = shortcuts.getInputGeneration();
    const bool current = event.routing == generation;
    auto context = callbacks.context ? callbacks.context() : MidiRoutingContext();
    context.allowBackgroundPlayback = shortcuts.getMidiInputSettings().allowBackgroundPlayback;
    context.cues = cues;
    if (! context.commandEnabled) context.commandEnabled = [this] (juce::CommandID id)
    {
        juce::ApplicationCommandInfo info (id);
        return manager.getTargetForCommand (id, info) != nullptr && (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
    };
    struct Pending { MidiBinding binding; MidiOwner owner; MidiTriggerRules::Transition transition; };
    std::vector<Pending> pending;
    bool ready = false, goHeld = false, goEligible = false;
    for (auto& runtime : bindings)
    {
        const auto& b = runtime.binding;
        if (! MidiTriggerRules::matchesAddress (b.trigger, event, identifier)) continue;
        // A retired gate can only finish a hold that existed at the edit. Other
        // ports/presses on that address must not acquire new retired GO holds.
        if (! runtime.live && ! runtime.rules.isHeld (event.token())) continue;
        const bool panic = b.commandID == CommandIDs::panicAll;
        if (! panic && ! event.ordinaryStateValid) continue;
        const auto owner = runtime.live ? shortcuts.resolveMidiOwner (b, context) : MidiOwner();
        const bool allowed = execute && current && runtime.live && ! shortcuts.isCapturing() && (panic || event.ordinaryAllowed);
        if (! execute || ! current || (! panic && ! event.ordinaryAllowed)) runtime.rules.quarantine (event.observedTimeMs);
        const auto transition = runtime.rules.observe (b.trigger, event, allowed);
        ready |= runtime.live && transition.ready;
        if (b.commandID == CommandIDs::go)
        {
            goHeld |= transition.held;
            goEligible |= transition.activated && owner.kind == MidiOwner::Kind::command;
        }
        pending.push_back ({ b, owner, transition });
    }
    // Always finish all physical state updates/releases before invoking code that
    // may edit a mapping, replace the project or enter a nested modal loop.
    // Aggregate every gate on this physical token before touching the GO group.
    // Dispatch deduplication must neither omit a held alias nor release one when
    // only a different threshold has returned to its inactive region.
    if (event.ordinaryStateValid && ! goHeld) releaseGo (event.token(), &event);
    auto goToken = event.token();
    const bool goPulse = goEligible && ! goHeld;
    if (goPulse) goToken.control += 4096;
    const bool activateGo = (goHeld || goPulse) && shortcuts.activations().press (goToken, goEligible,
        callbacks.requireGoKeyUp ? callbacks.requireGoKeyUp() : document.settings.requireKeyUp);
    std::set<juce::String> fired;
    for (const auto& p : pending)
    {
        if (generation != shortcuts.getInputGeneration()) break;
        const bool eligible = p.transition.activated && (p.owner.kind == MidiOwner::Kind::command || p.owner.kind == MidiOwner::Kind::panic || p.owner.kind == MidiOwner::Kind::cue);
        const bool go = p.binding.commandID == CommandIDs::go;
        const bool activate = eligible && (! go || activateGo);
        if (activate && fired.insert (p.binding.id).second)
        {
            auto token = go ? goToken : event.token();
            if (! go && p.binding.trigger.kind == MidiTrigger::Kind::cc && p.binding.trigger.behavior == MidiTrigger::Behavior::pulse)
                token.control += 4096;
            InputInvocation input { InputKind::midi, p.binding.id, p.binding.commandID, true, token, event.observedTimeMs, event.eventID };
            if (p.owner.kind == MidiOwner::Kind::panic)
            {
                if (const auto hard = shortcuts.panicGestures().activate (event.eventID, event.observedTimeMs); hard && callbacks.panic)
                    callbacks.panic (event.observedTimeMs, *hard);
            }
            else if (p.owner.kind == MidiOwner::Kind::command) shortcuts.invokeInput (input);
            else if (p.owner.kind == MidiOwner::Kind::cue && callbacks.cue) callbacks.cue (juce::Uuid (p.binding.id), input);
        }
    }
    if (goPulse) releaseGo (goToken, &event);
    if (current && shortcuts.isCapturing()) shortcuts.deliverCaptureMidi (event, identifier);
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(), [] (const auto& r) { return ! r.live && ! r.rules.anyHeld(); }), bindings.end());
    return ready;
}
}
