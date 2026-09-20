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
juce::String MidiTriggerRouter::bindingStatus (const MidiBinding& binding) const
{
    for (const auto& runtime : bindings)
        if (runtime.live && runtime.binding.id == binding.id && runtime.binding.trigger == binding.trigger)
        {
            using R = MidiTriggerRules::Readiness;
            const auto state = runtime.rules.readiness (binding.trigger, callbacks.clockMs());
            return juce::String::fromUTF8 (state == R::motion ? "움직임 종료 대기" : state == R::release ? "준비 대기: 입력을 놓으세요"
                : state == R::baseline ? "준비 대기: CC 기준값 미수신" : "실행 가능");
        }
    return {};
}
int MidiTriggerRouter::waitingBindings() const
{
    return static_cast<int> (std::count_if (bindings.begin(), bindings.end(), [this] (const auto& runtime)
        { return runtime.live && runtime.rules.readiness (runtime.binding.trigger, callbacks.clockMs()) != MidiTriggerRules::Readiness::ready; }));
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
    for (auto it = captureActivationInputs.begin(); it != captureActivationInputs.end();)
        if (it->source == input) it = captureActivationInputs.erase (it); else ++it;
    for (auto& gate : captureActivationGates)
        for (auto it = gate.held.begin(); it != gate.held.end();)
            if (it->source == input) it = gate.held.erase (it); else ++it;
    releaseGoSource (input);
    for (auto& runtime : bindings)
    {
        runtime.rules.forgetInput (input);
        for (auto it = runtime.retiredGoHolds.begin(); it != runtime.retiredGoHolds.end();)
            if (it->source == input) it = runtime.retiredGoHolds.erase (it); else ++it;
    }
    for (auto it = lastObserved.begin(); it != lastObserved.end();)
        if (it->first.source == input) it = lastObserved.erase (it); else ++it;
    if (connected) connections[input] = connection; else connections.erase (input);
    shortcuts.invalidateInputRouting();
    // Every observed connection participates in capture release waiting, even
    // when it did not provide the candidate. Its release can no longer be proven.
    if (! connected) shortcuts.cancelCapture();
}
void MidiTriggerRouter::inputFault (uint64_t input, bool panic)
{
    // Ordinary loss preserves panic. Reserved packets also carry ordinary rules
    // (for example, falling GO on a rising-panic CC), so their loss resets both.
    for (auto& runtime : bindings)
        if (panic || runtime.binding.commandID != CommandIDs::panicAll)
        {
            runtime.rules.forgetInput (input);
            for (auto it = runtime.retiredGoHolds.begin(); it != runtime.retiredGoHolds.end();)
                if (it->source == input) it = runtime.retiredGoHolds.erase (it); else ++it;
        }
    for (auto it = lastObserved.begin(); it != lastObserved.end();)
    {
        // A held panic Note survived ordinary loss. Keep its observation for
        // quarantine if a project/mapping boundary occurs before its next packet.
        const bool heldPanicNote = ! panic && it->second.event.kind == MidiTrigger::Kind::note
            && std::any_of (bindings.begin(), bindings.end(), [&] (const auto& runtime)
                { return runtime.binding.commandID == CommandIDs::panicAll && runtime.rules.isHeld (it->first); });
        if (it->first.source == input && ! heldPanicNote) it = lastObserved.erase (it); else ++it;
    }
    if (panic) shortcuts.panicGestures().invalidate();
    releaseGoSource (input);
    shortcuts.cancelCapture();
}
void MidiTriggerRouter::projectReplaced()
{
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(), [] (const auto& r) { return ! r.live; }), bindings.end());
    // Preserve only physically held Notes as quarantine, including addresses that
    // may acquire a different cue/command owner in the next project. Old GO holds
    // and all CC baselines still end here; a fresh Note can fire immediately.
    for (auto it = lastObserved.begin(); it != lastObserved.end();)
        if (it->second.event.kind == MidiTrigger::Kind::note && it->second.event.noteOn && it->second.event.value != 0)
        {
            it->second.carryGoHold = false;
            ++it;
        }
        else it = lastObserved.erase (it);
    for (auto& runtime : bindings)
    {
        runtime.rules.clear();
        runtime.retiredGoHolds.clear();
        synchroniseObserved (runtime);
    }
    for (const auto& p : connections) releaseGoSource (p.first);
    shortcuts.invalidateInputRouting();
}
void MidiTriggerRouter::captureStateChanged()
{
    const bool active = shortcuts.isCapturing();
    if (! active) { captureActivationInputs.clear(); captureActivationGates.clear(); }
    else if (! captureWasActive)
    {
        for (const auto& [token, observed] : lastObserved)
        {
            const bool noteHeld = observed.event.kind == MidiTrigger::Kind::note && observed.event.noteOn && observed.event.value > 0;
            if (noteHeld) captureActivationInputs.insert (token);
        }
        // Snapshot only the rules held now. An opposite gate pressing later
        // must not inherit another rule's release wait on the same CC address.
        for (const auto& runtime : bindings)
            if (runtime.binding.trigger.kind == MidiTrigger::Kind::cc && runtime.binding.trigger.behavior == MidiTrigger::Behavior::gate)
            {
                CaptureGate gate { runtime.binding.trigger, {} };
                for (const auto& [token, observed] : lastObserved)
                {
                    juce::ignoreUnused (observed);
                    if (runtime.rules.isHeld (token) && (runtime.live || runtime.retiredGoHolds.count (token) != 0)) gate.held.insert (token);
                }
                if (! gate.held.empty()) captureActivationGates.push_back (std::move (gate));
            }
    }
    captureWasActive = active;
    for (auto& runtime : bindings) runtime.rules.quarantine (callbacks.clockMs());
}
void MidiTriggerRouter::shortcutsChanged() { refreshBindings(); captureStateChanged(); }
void MidiTriggerRouter::discardRetiredBindings()
{
    // A released retired GO gate still needs to observe future ports/edges for
    // restoration. Keep its bounded per-address hysteresis until project replace;
    // retiredGoHolds alone decides whether it may retain an existing GO token.
    bindings.erase (std::remove_if (bindings.begin(), bindings.end(), [] (const auto& r)
    {
        const bool restoreGate = r.binding.commandID == CommandIDs::go && r.binding.trigger.kind == MidiTrigger::Kind::cc
            && r.binding.trigger.behavior == MidiTrigger::Behavior::gate;
        return ! r.live && ! restoreGate && (r.binding.commandID != CommandIDs::go || ! r.rules.anyHeld());
    }), bindings.end());
}
void MidiTriggerRouter::synchroniseObserved (Runtime& runtime)
{
    for (const auto& [token, observed] : lastObserved)
        if (MidiTriggerRules::matchesAddress (runtime.binding.trigger, observed.event, observed.identifier))
        {
            const auto state = runtime.rules.observe (runtime.binding.trigger, observed.event, false);
            if (runtime.binding.commandID == CommandIDs::go && state.held && observed.carryGoHold)
                shortcuts.activations().hold (token);
        }
}
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
    for (auto& runtime : bindings)
        if (runtime.live && std::none_of (next.begin(), next.end(), [&] (const auto& b) { return same (runtime.binding, b); }))
        {
            if (runtime.binding.commandID == CommandIDs::go)
                for (const auto& [token, observed] : lastObserved)
                {
                    juce::ignoreUnused (observed);
                    if (runtime.rules.isHeld (token)) runtime.retiredGoHolds.insert (token);
                }
            runtime.live = false;
        }
    for (const auto& b : next)
    {
        auto found = std::find_if (bindings.begin(), bindings.end(), [&] (const auto& r) { return same (r.binding, b); });
        if (found == bindings.end())
        {
            Runtime runtime { b, {}, true, {} };
            synchroniseObserved (runtime);
            bindings.push_back (std::move (runtime));
        }
        else
        {
            if (! found->live) synchroniseObserved (*found);
            found->retiredGoHolds.clear();
            found->live = true;
            found->binding = b;
        }
    }
    discardRetiredBindings();
    shortcuts.invalidateInputRouting();
}
bool MidiTriggerRouter::route (const MidiInputEvent& event, const juce::String& identifier, bool execute)
{
    const auto connection = connections.find (event.input);
    if (connection == connections.end() || connection->second != event.connection) return false;
    // Reserved Notes remain physically reliable across ordinary loss, including
    // a queued off. CC observations still require the ordinary epoch because an
    // opposite ordinary edge can share a reserved CC address.
    if (event.ordinaryStateValid || (event.panicReserved && event.kind == MidiTrigger::Kind::note))
        lastObserved[event.token()] = { event, identifier };
    const auto generation = shortcuts.getInputGeneration();
    const bool current = event.routing == generation;
    const bool capturePrepared = captureActivationInputs.empty()
        && std::all_of (captureActivationGates.begin(), captureActivationGates.end(), [] (const auto& gate) { return gate.held.empty(); });
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
        const bool panic = b.commandID == CommandIDs::panicAll;
        if (! panic && ! event.ordinaryStateValid) continue;
        const auto owner = runtime.live ? shortcuts.resolveMidiOwner (b, context) : MidiOwner();
        const bool allowed = execute && current && runtime.live && ! shortcuts.isCapturing() && (panic || event.ordinaryAllowed);
        if (! execute || ! current || (! panic && ! event.ordinaryAllowed)) runtime.rules.quarantine (event.observedTimeMs);
        const auto transition = runtime.rules.observe (b.trigger, event, allowed);
        ready |= runtime.live && transition.ready;
        if (b.commandID == CommandIDs::go)
        {
            // Observe every port for restoration, but a retired mapping may
            // only retain an existing token until that physical hold releases.
            if (! transition.held) runtime.retiredGoHolds.erase (event.token());
            goHeld |= transition.held && (runtime.live || runtime.retiredGoHolds.count (event.token()) != 0);
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
    if (event.kind == MidiTrigger::Kind::note && (! event.noteOn || event.value == 0)) captureActivationInputs.erase (event.token());
    if (event.kind == MidiTrigger::Kind::cc && event.ordinaryStateValid)
        for (auto& gate : captureActivationGates)
            if (gate.trigger.edge == MidiTrigger::Edge::rising ? event.value <= gate.trigger.lowThreshold : event.value >= gate.trigger.highThreshold)
                gate.held.erase (event.token());
    if (current && shortcuts.isCapturing() && capturePrepared) shortcuts.deliverCaptureMidi (event, identifier);
    discardRetiredBindings();
    return ready;
}
}
