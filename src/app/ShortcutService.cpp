#include "app/ShortcutService.h"
#include "app/ShortcutKeyInput.h"
#include "app/Commands.h"

namespace gocue
{
namespace
{
ShortcutOperationResult failure (const juce::String& message)
{
    return { juce::Result::fail (message), {} };
}

bool inScope (ShortcutScope scope, ShortcutKeyContext::Window window)
{
    using Window = ShortcutKeyContext::Window;
    if (window == Window::outsideApp)
        return false;
    if (scope == ShortcutScope::application)
        return true;
    if (window == Window::nativePlugin)
        return false;
    if (scope == ShortcutScope::playback)
        return window != Window::modal;
    return window == Window::main;
}

bool containsKey (const ShortcutKeys& keys, const juce::KeyPress& key)
{
    return std::any_of (keys.begin(), keys.end(), [&] (const auto& binding)
    { return ShortcutKeyInput::keysOverlap (binding, key); });
}

} // namespace

ShortcutService::ShortcutService (juce::ApplicationCommandManager& m, SaveFunction s, const ShortcutCatalog& c)
    : manager (m), save (std::move (s)), catalog (c), mapping (calculateMapping (catalog, profile))
{
    jassert (mapping.wasOk());
    synchroniseMappings();
}

ShortcutMappingResult ShortcutService::calculateMapping (const ShortcutCatalog& catalog, const ShortcutProfile& candidate)
{
    ShortcutMappingResult result;
    if (auto checked = candidate.validate(); checked.failed())
    {
        result.status = checked;
        result.diagnostics.push_back ({ ShortcutDiagnostic::Code::invalidProfile, {}, {}, {}, checked.getErrorMessage() });
        return result;
    }

    struct Binding { juce::KeyPress key; juce::String id; bool user; };
    std::vector<Binding> assigned;
    for (const auto& entry : catalog.getCommands())
        result.keys.emplace (entry.id, ShortcutKeys());

    for (const auto& [id, keys] : candidate.overrides)
    {
        const auto* entry = catalog.find (id);
        if (entry == nullptr)
        {
            result.diagnostics.push_back ({ ShortcutDiagnostic::Code::unknownAction, id, {}, {}, "Unknown action retained without execution" });
            continue;
        }
        if (! entry->isCommand())
        {
            result.status = juce::Result::fail ("Component keys are read-only: " + id);
            result.diagnostics.push_back ({ ShortcutDiagnostic::Code::readOnlyAction, id, {}, {}, result.status.getErrorMessage() });
            return result;
        }
        for (const auto& key : keys)
        {
            const auto other = std::find_if (assigned.begin(), assigned.end(), [&] (const Binding& b)
            { return b.id != id && ShortcutKeyInput::keysOverlap (b.key, key); });
            if (other != assigned.end())
            {
                result.status = juce::Result::fail ("Key assigned to both " + other->id + " and " + id);
                result.diagnostics.push_back ({ ShortcutDiagnostic::Code::commandConflict, id, other->id, key, result.status.getErrorMessage() });
                return result;
            }
            const auto normalised = ShortcutKeyCodec::normalise (key);
            result.keys[id].add (normalised);
            assigned.push_back ({ normalised, id, true });
        }
    }

    for (const auto& entry : catalog.getCommands())
    {
        if (candidate.overrides.count (entry.id) != 0)
            continue; // even an empty override replaces ALL present and future defaults
        for (const auto& key : entry.defaultKeys)
        {
            if (auto checked = ShortcutKeyCodec::validate (key); checked.failed())
            {
                result.status = checked;
                return result;
            }
            const auto other = std::find_if (assigned.begin(), assigned.end(), [&] (const Binding& b)
            { return b.id != entry.id && ShortcutKeyInput::keysOverlap (b.key, key); });
            if (other != assigned.end())
            {
                if (! other->user)
                {
                    result.status = juce::Result::fail ("Conflicting catalog defaults: " + entry.id + " and " + other->id);
                    result.diagnostics.push_back ({ ShortcutDiagnostic::Code::commandConflict, entry.id, other->id, key, result.status.getErrorMessage() });
                    return result;
                }
                result.diagnostics.push_back ({ ShortcutDiagnostic::Code::defaultSuppressed, entry.id, other->id, key,
                                                "Default disabled to preserve the user's key assignment" });
                continue;
            }
            const auto normalised = ShortcutKeyCodec::normalise (key);
            result.keys[entry.id].add (normalised);
            assigned.push_back ({ normalised, entry.id, false });
        }
    }
   #if JUCE_WINDOWS
    for (const auto& entry : catalog.getCommands())
        if (entry.scope == ShortcutScope::application)
            for (const auto& key : result.keys[entry.id])
            {
                const auto converted = PanicKeyHook::convert (key);
                if (! converted.binding)
                {
                    result.status = juce::Result::fail (converted.reason);
                    result.diagnostics.push_back ({ ShortcutDiagnostic::Code::panicKeyUnsupported, entry.id, {}, key, converted.reason });
                    return result;
                }
                result.panicBindings.push_back (*converted.binding);
            }
   #endif
    return result;
}

ShortcutService::RestoreReport ShortcutService::restore (const std::optional<juce::String>& currentXml,
                                                        const std::optional<juce::String>& lastGoodXml)
{
    RestoreReport report;
    if (inTransaction)
    {
        report.message = "A shortcut transaction is in progress";
        return report;
    }
    const juce::ScopedValueSetter<bool> guard (inTransaction, true);
    const auto load = [this, &report] (const juce::String& xml, RestoreReport::Source source, juce::String& rejected)
    {
        const auto parsed = ShortcutProfile::parse (xml);
        auto resolved = parsed.wasOk() ? calculateMapping (catalog, parsed.profile) : ShortcutMappingResult();
        if (! parsed.wasOk() || resolved.failed())
        {
            rejected = xml;
            report.message += (report.message.isEmpty() ? "" : "\n")
                            + (parsed.wasOk() ? resolved.getErrorMessage() : parsed.message);
            report.diagnostics.insert (report.diagnostics.end(), resolved.diagnostics.begin(), resolved.diagnostics.end());
            return false;
        }
        profile = parsed.profile;
        mapping = std::move (resolved);
        report.source = source;
        return true;
    };
    const bool restored = currentXml.has_value() && load (*currentXml, RestoreReport::Source::current, report.rejectedXml);
    if (! restored && (! currentXml.has_value() || ! lastGoodXml.has_value()
                       || ! load (*lastGoodXml, RestoreReport::Source::lastGood, report.rejectedLastGoodXml)))
    {
        profile = {};
        mapping = calculateMapping (catalog, profile);
    }
    report.diagnostics.insert (report.diagnostics.end(), mapping.diagnostics.begin(), mapping.diagnostics.end());
    synchroniseMappings();
    listeners.call ([] (Listener& listener) { listener.shortcutsChanged(); });
    return report;
}

const ShortcutKeys& ShortcutService::getKeys (const juce::String& id) const
{
    const auto found = mapping.keys.find (id);
    static const ShortcutKeys empty;
    return found != mapping.keys.end() ? found->second : empty;
}

const ShortcutKeys& ShortcutService::getKeys (juce::CommandID commandID) const
{
    const auto* entry = catalog.find (commandID);
    return getKeys (entry != nullptr ? entry->id : juce::String());
}

ShortcutKeyOwner ShortcutService::resolveKeyOwner (const juce::KeyPress& key, const ShortcutKeyContext& context, bool preview) const
{
    using Kind = ShortcutKeyOwner::Kind;
    using Reason = ShortcutKeyOwner::Reason;
    ShortcutKeyOwner result;
    if (! context.applicationActive || context.window == ShortcutKeyContext::Window::outsideApp)
        return { Kind::blocked, Reason::inactiveApp, {}, 0, {} };
    if (! preview && (context.captureActive || isCapturing()))
        return { Kind::capture, Reason::captureActive, {}, 0, {} };

    const ShortcutDefinition* command = nullptr;
    const bool nativePanic = context.nativePanicOwned || (context.nativeKey && std::any_of (mapping.panicBindings.begin(), mapping.panicBindings.end(),
        [&] (const auto& binding) { return binding.virtualKey == context.nativeKey->virtualKey && binding.modifiers == context.nativeKey->modifiers; }));
    for (const auto& entry : catalog.getCommands())
        if (entry.scope == ShortcutScope::application && context.nativeKey ? nativePanic
            : (! nativePanic && containsKey (getKeys (entry.id), key)))
        {
            command = &entry;
            break; // calculateMapping guarantees global command-key uniqueness
        }
    const ShortcutDefinition* component = nullptr;
    for (const auto& entry : catalog.getFixedComponents())
        if (entry.scope == context.focus && entry.matchesKey (key))
        {
            component = &entry;
            break;
        }
    for (const auto& cue : context.cueHotkeys)
        if (ShortcutKeyInput::keysOverlap (cue.key, key))
            result.conflicts.push_back ({ Kind::cueHotkey, cue.id, 0 });

    const auto commandOwner = [&]
    {
        result.kind = Kind::command;
        result.reason = std::any_of (result.conflicts.begin(), result.conflicts.end(), [] (const auto& c) { return c.kind == Kind::cueHotkey; })
                            ? Reason::commandOverCue : Reason::commandBinding;
        result.id = command->id;
        result.commandID = command->commandID;
        if (! inScope (command->scope, context.window)
            || (containsKey (command->cueTableOnlyKeys, key) && context.focus != ShortcutScope::cueTable))
        {
            result.kind = Kind::blocked;
            result.reason = Reason::outsideScope;
        }
        else if (context.commandEnabled && ! context.commandEnabled (command->commandID))
        {
            result.kind = Kind::blocked;
            result.reason = Reason::disabled;
        }
        else if (context.isRepeat && ! command->allowsRepeat)
        {
            result.kind = Kind::blocked;
            result.reason = Reason::repeatSuppressed;
        }
        return result;
    };

    if (command != nullptr && command->scope == ShortcutScope::application) // panic before text and component keys
    {
        if (component != nullptr)
            result.conflicts.push_back ({ Kind::fixedComponent, component->id, 0 });
        return commandOwner();
    }
    if (context.standardUiConsumesKey || (context.textEditing && ShortcutKeyInput::isStandardTextEditorKey (key)))
    {
        if (command != nullptr)
            result.conflicts.push_back ({ Kind::command, command->id, command->commandID });
        result.kind = Kind::standardUi;
        result.reason = Reason::standardUi;
        return result;
    }
    if (component != nullptr && ! context.textEditing)
    {
        result.kind = Kind::fixedComponent;
        result.reason = Reason::componentFocus;
        result.id = component->id;
        if (command != nullptr)
            result.conflicts.push_back ({ Kind::command, command->id, command->commandID });
        if (context.componentCanHandle && ! context.componentCanHandle (component->id))
        {
            result.kind = Kind::blocked;
            result.reason = Reason::disabled;
        }
        else if (context.isRepeat && ! component->allowsRepeat)
        {
            result.kind = Kind::blocked;
            result.reason = Reason::repeatSuppressed;
        }
        return result;
    }
    if (command != nullptr)
        return commandOwner(); // disabled or out-of-scope commands never fall through to cue hotkeys

    const ShortcutKeyContext::CueHotkey* selected = nullptr;
    for (const auto& cue : context.cueHotkeys)
        if (ShortcutKeyInput::keysOverlap (cue.key, key) && cue.inActiveContainer)
        {
            if (selected != nullptr)
            {
                result.kind = Kind::conflict;
                result.reason = Reason::ambiguousCueHotkey;
                return result;
            }
            selected = &cue;
        }
    if (selected != nullptr)
    {
        result.id = selected->id;
        result.kind = Kind::cueHotkey;
        result.reason = Reason::cueBinding;
        if (context.window != ShortcutKeyContext::Window::main || context.textEditing)
        {
            result.kind = Kind::blocked;
            result.reason = Reason::outsideScope;
        }
        else if (! selected->enabled || context.isRepeat)
        {
            result.kind = Kind::blocked;
            result.reason = context.isRepeat ? Reason::repeatSuppressed : Reason::disabled;
        }
        result.conflicts.erase (std::remove_if (result.conflicts.begin(), result.conflicts.end(), [&] (const auto& c) { return c.id == selected->id; }),
                                result.conflicts.end());
    }
    return result;
}

ShortcutOperationResult ShortcutService::checkEditableCommand (const juce::String& id) const
{
    if (editingLocked)
        return failure ("Shortcut editing is locked in show mode");
    if (inTransaction)
        return failure ("A shortcut transaction is in progress");
    const auto* entry = catalog.find (id);
    if (entry == nullptr || ! entry->isCommand())
        return failure ("Unknown or read-only action: " + id);
    return {};
}

ShortcutOperationResult ShortcutService::setKeys (const juce::String& id, const ShortcutKeys& keys, ConflictPolicy policy)
{
    if (auto checked = checkEditableCommand (id); checked.failed())
        return checked;
    auto candidate = profile;
    candidate.overrides[id] = keys;
    for (const auto& key : keys)
        for (const auto& entry : catalog.getCommands())
            if (entry.id != id && containsKey (getKeys (entry.id), key))
            {
                if (policy == ConflictPolicy::reject)
                    return { juce::Result::fail ("Key already assigned to " + entry.id),
                             { { ShortcutDiagnostic::Code::commandConflict, id, entry.id, key, "Move the key explicitly or choose another key" } } };
                // Make the old owner explicit, even when it previously inherited defaults.
                auto inserted = candidate.overrides.emplace (entry.id, getKeys (entry.id));
                auto& oldKeys = inserted.first->second;
                for (int i = oldKeys.size(); --i >= 0;)
                    if (ShortcutKeyInput::keysOverlap (oldKeys[i], key))
                        oldKeys.remove (i);
            }
    return commit (std::move (candidate));
}

ShortcutOperationResult ShortcutService::addKey (const juce::String& id, const juce::KeyPress& key, ConflictPolicy policy)
{
    if (auto checked = checkEditableCommand (id); checked.failed())
        return checked;
    auto keys = getKeys (id);
    if (containsKey (keys, key))
        return {}; // duplicate learning is a no-op, preserving inheritance
    keys.add (key);
    return setKeys (id, keys, policy);
}

ShortcutOperationResult ShortcutService::replaceKey (const juce::String& id, int index, const juce::KeyPress& key, ConflictPolicy policy)
{
    if (auto checked = checkEditableCommand (id); checked.failed())
        return checked;
    auto keys = getKeys (id);
    if (! juce::isPositiveAndBelow (index, keys.size()))
        return failure ("Invalid shortcut key index");
    keys.set (index, key);
    return setKeys (id, keys, policy);
}

ShortcutOperationResult ShortcutService::removeKey (const juce::String& id, int index)
{
    if (auto checked = checkEditableCommand (id); checked.failed())
        return checked;
    auto keys = getKeys (id);
    if (! juce::isPositiveAndBelow (index, keys.size()))
        return failure ("Invalid shortcut key index");
    keys.remove (index);
    return setKeys (id, keys);
}

ShortcutOperationResult ShortcutService::restoreCommandDefaults (const juce::String& id, ConflictPolicy policy)
{
    if (auto checked = checkEditableCommand (id); checked.failed())
        return checked;
    auto candidate = profile;
    if (policy == ConflictPolicy::move)
        for (const auto& key : catalog.find (id)->defaultKeys)
            for (const auto& entry : catalog.getCommands())
                if (entry.id != id && containsKey (getKeys (entry.id), key))
                {
                    auto inserted = candidate.overrides.emplace (entry.id, getKeys (entry.id));
                    auto& previousKeys = inserted.first->second;
                    for (int i = previousKeys.size(); --i >= 0;)
                        if (ShortcutKeyInput::keysOverlap (previousKeys[i], key)) previousKeys.remove (i);
                }
    candidate.overrides.erase (id);
    return commit (std::move (candidate));
}

ShortcutOperationResult ShortcutService::restoreAllDefaults() { return commit ({}); }

ShortcutOperationResult ShortcutService::importProfile (const juce::String& xml)
{
    const auto parsed = ShortcutProfile::parseExchange (xml);
    if (! parsed.wasOk())
        return failure (parsed.status.getErrorMessage());
    return parsed.replacesMidi ? commitInputs (parsed.keyboard, parsed.midi, {}) : commit (parsed.keyboard);
}

juce::String ShortcutService::exportProfile() const
{
    ShortcutProfile exported = profile; // retain unknown IDs, too
    for (const auto& [id, keys] : mapping.keys)
        exported.overrides[id] = keys;
    juce::String xml;
    const auto written = exported.serialise (xml);
    jassert (written.wasOk());
    juce::ignoreUnused (written);
    return xml;
}

ShortcutOperationResult ShortcutService::commit (ShortcutProfile candidate)
{
    if (editingLocked || inTransaction)
        return failure (editingLocked ? "Shortcut editing is locked in show mode" : "A shortcut transaction is in progress");
    const juce::ScopedValueSetter<bool> guard (inTransaction, true);
    auto resolved = calculateMapping (catalog, candidate);
    if (resolved.failed())
        return { resolved.status, resolved.diagnostics };

    juce::String currentXml, lastGoodXml;
    if (auto written = candidate.serialise (currentXml); written.failed())
        return { written, {} };
    if (auto written = profile.serialise (lastGoodXml); written.failed())
        return { written, {} };
    if (! save)
        return failure ("No shortcut storage is available");
    if (auto saved = save (currentXml, lastGoodXml); saved.failed())
        return { saved, resolved.diagnostics };

    profile = std::move (candidate);
    mapping = std::move (resolved);
    synchroniseMappings();
    listeners.call ([] (Listener& listener) { listener.shortcutsChanged(); });
    return { juce::Result::ok(), mapping.diagnostics };
}

void ShortcutService::beginCapture (CaptureToken token, std::function<void (const juce::KeyPress&)> receive,
                                    std::function<void()> cancel)
{
    if (token == nullptr || editingLocked)
        return;
    cancelCapture();
    captureToken = token;
    captureReceiver = std::move (receive);
    captureCancellation = std::move (cancel);
    ++inputGeneration;
    panicGate.invalidate();
    listeners.call ([] (Listener& l) { l.captureStateChanged(); });
}

void ShortcutService::endCapture (CaptureToken token)
{
    if (token == nullptr || captureToken != token)
        return;
    captureToken = nullptr;
    captureReceiver = {};
    captureCancellation = {};
    midiCaptureReceiver = {};
    ++inputGeneration;
    panicGate.invalidate();
    listeners.call ([] (Listener& l) { l.captureStateChanged(); });
}

void ShortcutService::cancelCapture()
{
    auto cancel = captureCancellation;
    endCapture (captureToken);
    if (cancel)
        cancel();
}

void ShortcutService::deliverCaptureKey (const juce::KeyPress& key)
{
    auto receive = captureReceiver; // callback may end capture and destroy its widget
    if (isCapturing() && receive)
        receive (key);
}

void ShortcutService::setEditingLocked (bool locked)
{
    if (editingLocked == locked)
        return;
    editingLocked = locked;
    if (locked)
        cancelCapture();
    listeners.call ([] (Listener& l) { l.shortcutEditingLockChanged(); });
}

void ShortcutService::synchroniseMappings()
{
    ++inputGeneration;
    panicGate.invalidate();
    auto* juceMappings = manager.getKeyMappings();
    // Two passes are essential: JUCE 8.0.15 addKeyPress does NOT remove another command's binding.
    for (const auto& entry : catalog.getCommands())
        juceMappings->clearAllKeyPresses (entry.commandID);
    for (const auto& entry : catalog.getCommands())
        for (const auto& key : getKeys (entry.id))
            juceMappings->addKeyPress (entry.commandID, key);
    manager.commandStatusChanged();
}

void ShortcutService::invalidateInputRouting()
{
    ++inputGeneration;
    panicGate.invalidate();
    listeners.call ([] (Listener& l) { l.captureStateChanged(); });
}

bool ShortcutService::invokeInput (const InputInvocation& input, const juce::ApplicationCommandTarget::InvocationInfo* keyboardInfo)
{
    const juce::ScopedValueSetter<const InputInvocation*> guard (invocation, &input);
    juce::ApplicationCommandTarget::InvocationInfo info (input.commandID);
    if (keyboardInfo != nullptr) info = *keyboardInfo; // preserve JUCE origin/key/repeat-duration exactly
    else info.isKeyDown = input.active;
    return manager.invoke (info, false);
}

void ShortcutService::setMidiCaptureReceiver (CaptureToken token, std::function<void (const MidiInputEvent&, const juce::String&)> receiver)
{
    if (captureToken == token && token != nullptr) midiCaptureReceiver = std::move (receiver);
}
void ShortcutService::deliverCaptureMidi (const MidiInputEvent& event, const juce::String& identifier)
{
    auto receiver = midiCaptureReceiver;
    if (isCapturing() && receiver) receiver (event, identifier);
}

juce::Result ShortcutService::validateMidiMapping (const MidiShortcutProfile& candidate) const
{
    if (auto r = candidate.validate(); r.failed()) return r;
    std::vector<std::pair<juce::String, MidiTrigger>> assigned;
    for (const auto& [id, triggers] : candidate.overrides)
    {
        const auto* definition = catalog.find (id);
        if (definition == nullptr) continue;
        if (! definition->isCommand()) return juce::Result::fail ("Component inputs are read-only: " + id);
        for (const auto& t : triggers)
        {
            for (const auto& [other, binding] : assigned)
                if (other != id && MidiTriggerRules::intersects (t, binding)) return juce::Result::fail ("MIDI conflict: " + other + " / " + id);
            assigned.emplace_back (id, t);
        }
    }
    return juce::Result::ok();
}

ShortcutService::RestoreReport ShortcutService::restoreMidi (const std::optional<juce::String>& current, const std::optional<juce::String>& good)
{
    RestoreReport report;
    if (inTransaction) { report.message = "A shortcut transaction is in progress"; return report; }
    const juce::ScopedValueSetter<bool> guard (inTransaction, true);
    const auto load = [&] (const juce::String& text, RestoreReport::Source source, juce::String& rejected)
    {
        const auto parsed = MidiShortcutProfile::parse (text);
        const auto r = parsed.wasOk() ? validateMidiMapping (parsed.profile) : parsed.status;
        if (r.failed()) { rejected = text; report.message += r.getErrorMessage() + "\n"; return false; }
        midiProfile = parsed.profile;
        report.source = source;
        return true;
    };
    if (! (current && load (*current, RestoreReport::Source::current, report.rejectedXml))
        && ! (current && good && load (*good, RestoreReport::Source::lastGood, report.rejectedLastGoodXml))) midiProfile = {};
    ++inputGeneration;
    panicGate.invalidate();
    listeners.call ([] (Listener& l) { l.shortcutsChanged(); });
    return report;
}
ShortcutService::RestoreReport ShortcutService::restoreMidiInputs (const std::optional<juce::String>& current, const std::optional<juce::String>& good)
{
    RestoreReport report;
    if (inTransaction) { report.message = "A shortcut transaction is in progress"; return report; }
    const juce::ScopedValueSetter<bool> guard (inTransaction, true);
    const auto load = [&] (const juce::String& text, RestoreReport::Source source, juce::String& rejected)
    {
        MidiInputSettings candidate;
        const auto r = MidiInputSettings::parse (text, candidate);
        if (r.failed()) { rejected = text; report.message += r.getErrorMessage() + "\n"; return false; }
        midiInputs = std::move (candidate);
        report.source = source;
        return true;
    };
    if (! (current && load (*current, RestoreReport::Source::current, report.rejectedXml))
        && ! (current && good && load (*good, RestoreReport::Source::lastGood, report.rejectedLastGoodXml))) midiInputs = {};
    invalidateInputRouting();
    listeners.call ([] (Listener& l) { l.midiInputSettingsChanged(); });
    return report;
}
const MidiTriggers& ShortcutService::getMidiTriggers (const juce::String& id) const
{
    static const MidiTriggers empty;
    const auto found = midiProfile.overrides.find (id);
    return found == midiProfile.overrides.end() ? empty : found->second;
}
std::vector<MidiBinding> ShortcutService::midiCommandBindings() const
{
    std::vector<MidiBinding> bindings;
    for (const auto& entry : catalog.getCommands())
        for (const auto& t : getMidiTriggers (entry.id)) bindings.push_back ({ entry.id, t, entry.commandID, true });
    return bindings;
}

MidiOwner ShortcutService::resolveMidiOwner (const MidiBinding& binding, const MidiRoutingContext& context, bool preview) const
{
    using Kind = MidiOwner::Kind;
    if (! preview && isCapturing()) return { Kind::capture, {}, "capture", 0, {} };
    MidiOwner owner { Kind::none, binding.id, {}, binding.commandID, {} };
    if (binding.commandID != 0)
    {
        const auto* definition = catalog.find (binding.id);
        if (definition == nullptr || ! definition->isCommand() || definition->commandID != binding.commandID) return owner;
        if (binding.commandID == CommandIDs::panicAll)
        {
            owner.kind = Kind::panic;
            if (context.commandEnabled && ! context.commandEnabled (binding.commandID)) { owner.kind = Kind::blocked; owner.reason = "disabled"; }
            return owner;
        }
        owner.kind = Kind::command;
        const bool playback = definition->scope == ShortcutScope::playback;
        const bool allowed = ! context.modal && context.window != ShortcutKeyContext::Window::modal
            && (playback ? (context.applicationActive || context.allowBackgroundPlayback)
                         : (context.applicationActive && context.window == ShortcutKeyContext::Window::main && ! context.textEditing));
        if (! allowed) { owner.kind = Kind::blocked; owner.reason = "outside scope"; }
        else if (context.commandEnabled && ! context.commandEnabled (binding.commandID)) { owner.kind = Kind::blocked; owner.reason = "disabled"; }
        return owner;
    }
    for (const auto& command : midiCommandBindings())
        if (MidiTriggerRules::intersects (binding.trigger, command.trigger)) owner.conflicts.push_back (command.id);
    for (const auto& cue : context.cues)
        if (cue.id != binding.id && MidiTriggerRules::intersects (binding.trigger, cue.trigger)) owner.conflicts.push_back (cue.id);
    owner.kind = Kind::cue;
    if (! owner.conflicts.empty()) { owner.kind = Kind::blocked; owner.reason = "conflicting binding"; }
    else if (! binding.enabled) { owner.kind = Kind::blocked; owner.reason = "disarmed"; }
    else if (context.modal || context.window == ShortcutKeyContext::Window::modal || (! context.applicationActive && ! context.allowBackgroundPlayback))
    { owner.kind = Kind::blocked; owner.reason = "outside scope"; }
    return owner;
}

ShortcutOperationResult ShortcutService::setMidiTriggers (const juce::String& id, MidiTriggers triggers, ConflictPolicy policy)
{
    if (auto r = checkEditableCommand (id); r.failed()) return r;
    MidiTriggers unique;
    for (auto& t : triggers) if (std::find (unique.begin(), unique.end(), t) == unique.end()) unique.push_back (std::move (t));
    auto candidate = midiProfile;
    if (policy == ConflictPolicy::move)
        for (auto& [other, list] : candidate.overrides)
            if (other != id && catalog.find (other) != nullptr)
                list.erase (std::remove_if (list.begin(), list.end(), [&] (const auto& b)
                { return std::any_of (unique.begin(), unique.end(), [&] (const auto& t) { return MidiTriggerRules::intersects (b, t); }); }), list.end());
    candidate.overrides[id] = std::move (unique);
    if (candidate == midiProfile) return {};
    return replaceMidiProfile (std::move (candidate));
}
ShortcutOperationResult ShortcutService::replaceMidiProfile (MidiShortcutProfile candidate) { return commitInputs ({}, std::move (candidate), {}); }
ShortcutOperationResult ShortcutService::setMidiInputSettings (MidiInputSettings candidate) { return commitInputs ({}, {}, std::move (candidate)); }

ShortcutOperationResult ShortcutService::commitInputs (std::optional<ShortcutProfile> keyboard, std::optional<MidiShortcutProfile> midi,
                                                     std::optional<MidiInputSettings> devices)
{
    if (editingLocked || inTransaction) return failure ("Input settings are locked or a transaction is in progress");
    const juce::ScopedValueSetter<bool> guard (inTransaction, true);
    ShortcutMappingResult keys;
    if (keyboard) { keys = calculateMapping (catalog, *keyboard); if (keys.failed()) return keys; }
    if (midi) if (auto r = validateMidiMapping (*midi); r.failed()) return { r, {} };
    if (devices) if (auto r = devices->validate(); r.failed()) return { r, {} };
    InputSettingsTransaction transaction;
    const auto serialise = [] (const auto& candidate, const auto& previous, auto& pair)
    {
        pair.emplace();
        auto r = candidate.serialise (pair->current);
        return r.failed() ? r : previous.serialise (pair->lastGood);
    };
    if (keyboard) if (auto r = serialise (*keyboard, profile, transaction.keyboard); r.failed()) return { r, {} };
    if (midi) if (auto r = serialise (*midi, midiProfile, transaction.midi); r.failed()) return { r, {} };
    if (devices) if (auto r = serialise (*devices, midiInputs, transaction.devices); r.failed()) return { r, {} };
    if (! saveInputs) return failure ("No input settings transaction storage is available");
    if (auto r = saveInputs (transaction); r.failed()) return { r, {} };
    if (keyboard) { profile = std::move (*keyboard); mapping = std::move (keys); }
    if (midi) midiProfile = std::move (*midi);
    if (devices) midiInputs = std::move (*devices);
    if (keyboard) synchroniseMappings(); else { ++inputGeneration; panicGate.invalidate(); }
    listeners.call ([] (Listener& l) { l.shortcutsChanged(); });
    if (devices) listeners.call ([] (Listener& l) { l.midiInputSettingsChanged(); });
    return {};
}
juce::String ShortcutService::exportCombinedProfile() const
{
    auto keys = profile;
    auto midi = midiProfile;
    for (const auto& [id, list] : mapping.keys) { keys.overrides[id] = list; midi.overrides.try_emplace (id); }
    juce::String xml;
    const auto r = keys.serialiseExchange (midi, xml);
    jassert (r.wasOk());
    juce::ignoreUnused (r);
    return xml;
}

} // namespace gocue
