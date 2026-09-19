#include "app/ShortcutService.h"
#include "app/ShortcutKeyInput.h"

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
    const auto parsed = ShortcutProfile::parse (xml);
    if (! parsed.wasOk())
        return failure (parsed.message);
    return commit (parsed.profile);
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
    listeners.call ([] (Listener& l) { l.captureStateChanged(); });
}

void ShortcutService::endCapture (CaptureToken token)
{
    if (token == nullptr || captureToken != token)
        return;
    captureToken = nullptr;
    captureReceiver = {};
    captureCancellation = {};
    ++inputGeneration;
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
    auto* juceMappings = manager.getKeyMappings();
    // Two passes are essential: JUCE 8.0.15 addKeyPress does NOT remove another command's binding.
    for (const auto& entry : catalog.getCommands())
        juceMappings->clearAllKeyPresses (entry.commandID);
    for (const auto& entry : catalog.getCommands())
        for (const auto& key : getKeys (entry.id))
            juceMappings->addKeyPress (entry.commandID, key);
    manager.commandStatusChanged();
}

} // namespace gocue
