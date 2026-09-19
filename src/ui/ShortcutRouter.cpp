#include "ui/ShortcutRouter.h"
#include "app/Commands.h"
#include "app/ShortcutKeyInput.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue
{
namespace
{
ShortcutRouter* desktopRouter = nullptr;
bool matchesNativeKey (int code, int vk)
{
    for (const auto& alias : ShortcutKeyInput::numberPadAliases())
        if (alias.virtualKey == vk && (code == alias.keyCode
            || juce::String (alias.characters).containsChar (static_cast<juce::juce_wchar> (code))))
            return true;
    const auto converted = PanicKeyHook::convert (juce::KeyPress (code,
        juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier, 0));
    return converted.binding && converted.binding->virtualKey == vk;
}

bool nativeMessagesPending()
{
   #if JUCE_WINDOWS
    MSG message {};
    return PeekMessageW (&message, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) != 0;
   #else
    return false;
   #endif
}

juce::Component* inputTarget()
{
    auto* target = juce::Component::getCurrentlyFocusedComponent();
    if (target == nullptr || target->isCurrentlyBlockedByAnotherModalComponent())
        if (auto* modal = juce::Component::getCurrentlyModalComponent())
            return modal;
    return target;
}
}
ShortcutRouter::ShortcutRouter (ShortcutService& s, juce::ApplicationCommandManager& m, Callbacks c)
    : service (s), manager (m), callbacks (std::move (c))
{
    if (! callbacks.keyDown)
        callbacks.keyDown = [] (int code) { return juce::KeyPress::isKeyCurrentlyDown (code); };
    if (! callbacks.nativeKeyDown)
        callbacks.nativeKeyDown = [] (int vk)
        {
           #if JUCE_WINDOWS
            return (GetAsyncKeyState (vk) & 0x8000) != 0;
           #else
            juce::ignoreUnused (vk);
            return false;
           #endif
        };
    if (! callbacks.applicationActive)
        callbacks.applicationActive = [] { return juce::Process::isForegroundProcess(); };
    service.addListener (this);
    juce::Desktop::getInstance().addFocusChangeListener (this);
    startTimerHz (30);
}

ShortcutRouter::~ShortcutRouter()
{
    if (desktopRouter == this)
        desktopRouter = nullptr;
    stopTimer();
    service.removeListener (this);
    juce::Desktop::getInstance().removeFocusChangeListener (this);
    for (const auto& [pointer, c] : watched)
    {
        juce::ignoreUnused (pointer);
        if (c != nullptr)
        {
            c->removeKeyListener (this);
            c->removeComponentListener (this);
        }
    }
}

void ShortcutRouter::watchTree (juce::Component& c)
{
    if (watched.count (&c) != 0)
        return;
    watched.emplace (&c, &c);
    c.addKeyListener (this);
    c.addComponentListener (this);
    for (auto* child : c.getChildren())
        watchTree (*child);
}
void ShortcutRouter::attach (juce::Component& c, Window window)
{
    setWindowScope (c, window);
    watchTree (c);
}
void ShortcutRouter::activateDesktopRouting()
{
    jassert (desktopRouter == nullptr || desktopRouter == this);
    desktopRouter = this;
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        watchWindow (desktop.getComponent (i));
}
void ShortcutRouter::watchWindow (juce::Component* window)
{
    if (desktopRouter != nullptr && window != nullptr)
    {
        desktopRouter->watchTree (*window);
        window->removeKeyListener (desktopRouter);
        window->addKeyListener (desktopRouter);
    }
}
void ShortcutRouter::setWindowScope (juce::Component& c, Window window)
{
    c.getProperties().set ("shortcutWindow", static_cast<int> (window));
    watchWindow (&c);
}
void ShortcutRouter::componentChildrenChanged (juce::Component& c)
{
    for (auto* child : c.getChildren())
        watchTree (*child);
}
void ShortcutRouter::componentBeingDeleted (juce::Component& c)
{
    watched.erase (&c);
}
void ShortcutRouter::refreshFocus()
{
    if (auto* focused = inputTarget())
    {
        watchTree (*focused->getTopLevelComponent());
        // Buttons can add their default-key listeners after a child was attached.
        // The router must precede those listeners, including on key-state delivery.
        for (auto* c = focused; c != nullptr; c = c->getParentComponent())
        {
            c->removeKeyListener (this);
            c->addKeyListener (this);
        }
    }
}
void ShortcutRouter::globalFocusChanged (juce::Component*)
{
    refreshFocus();
    applicationActiveChanged (callbacks.applicationActive());
    pollKeyState(); // window changes inside this app retain every physically-held key
}
void ShortcutRouter::prepareNativeEvent (int vk, int modifiers, bool down, bool repeat)
{
    refreshFocus();
    // Queue order is authoritative even when the device has already pressed the
    // same key again. Keep released observations for delayed WM_CHAR delivery.
    if (! down || ! repeat)
    {
        releaseKey (-vk);
        for (auto& press : nativePresses)
            if (press.key.virtualKey == vk)
                press.released = true;
    }
    if (down)
    {
        const bool panic = std::any_of (service.getPanicBindings().begin(), service.getPanicBindings().end(),
            [&] (const auto& binding) { return binding.virtualKey == vk && binding.modifiers == modifiers; });
        nativePresses.push_back ({ { vk, modifiers }, repeat, false, panic,
                                  service.isCapturing(), service.getInputGeneration() });
    }
}

juce::Component* ShortcutRouter::componentOwner (juce::Component* origin) const
{
    for (auto* c = origin; c != nullptr; c = c->getParentComponent())
        if (c->getProperties().contains ("shortcutScope"))
            return c;
    return nullptr;
}

ShortcutKeyContext ShortcutRouter::contextFor (juce::Component* origin, const juce::KeyPress& key) const
{
    ShortcutKeyContext context;
    context.window = Window::modal; // unclassified dialogs never acquire playback/edit shortcuts
    context.applicationActive = active;
    for (auto* c = origin; c != nullptr; c = c->getParentComponent())
    {
        if (auto* text = dynamic_cast<juce::TextEditor*> (c))
            context.textEditing = ! text->isReadOnly();
        if (c->isCurrentlyModal() || dynamic_cast<juce::DialogWindow*> (c) != nullptr || dynamic_cast<juce::AlertWindow*> (c) != nullptr)
            break;
        if (c->getProperties().contains ("shortcutWindow"))
        {
            context.window = static_cast<Window> (static_cast<int> (c->getProperties()["shortcutWindow"]));
            break;
        }
    }
    if (auto* component = componentOwner (origin))
        context.focus = static_cast<ShortcutScope> (static_cast<int> (component->getProperties()["shortcutScope"]));

    // Standard dialog/control operations have priority over commands. Read-only
    // manual text is not editing; its unused letters/Space can control playback.
    context.standardUiConsumesKey = key.isKeyCode (juce::KeyPress::tabKey)
        || (context.window != Window::main && key.isKeyCode (juce::KeyPress::escapeKey))
        || (context.window == Window::modal && ShortcutKeyInput::isStandardTextEditorKey (key));
    for (auto* c = origin; c != nullptr; c = c->getParentComponent())
    {
        if (auto* text = dynamic_cast<juce::TextEditor*> (c); text != nullptr && text->isReadOnly())
            context.standardUiConsumesKey |= key == juce::KeyPress ('C', juce::ModifierKeys::commandModifier, 0)
                || key == juce::KeyPress ('A', juce::ModifierKeys::commandModifier, 0);
        if (auto* button = dynamic_cast<juce::Button*> (c))
            context.standardUiConsumesKey |= key.isKeyCode (juce::KeyPress::returnKey) || button->isRegisteredForShortcut (key);
        if (dynamic_cast<juce::ComboBox*> (c) != nullptr)
            context.standardUiConsumesKey |= key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey
                || key == juce::KeyPress::upKey || key == juce::KeyPress::downKey;
        if (context.window == Window::auxiliary && dynamic_cast<juce::ListBox*> (c) != nullptr)
            context.standardUiConsumesKey |= key.isKeyCode (juce::KeyPress::upKey) || key.isKeyCode (juce::KeyPress::downKey)
                || key.isKeyCode (juce::KeyPress::pageUpKey) || key.isKeyCode (juce::KeyPress::pageDownKey)
                || key.isKeyCode (juce::KeyPress::homeKey) || key.isKeyCode (juce::KeyPress::endKey)
                || key.isKeyCode (juce::KeyPress::returnKey) || key.isKeyCode (juce::KeyPress::deleteKey)
                || key.isKeyCode (juce::KeyPress::backspaceKey);
    }
    if (callbacks.context)
        callbacks.context (context);
    return context;
}

bool ShortcutRouter::keyPressed (const juce::KeyPress& key, juce::Component* origin)
{
    auto* focus = inputTarget();
    if (focus != nullptr && origin != focus)
        return false; // standard UI event bubbling: already resolved at its focus owner
    applicationActiveChanged (callbacks.applicationActive());
    const auto found = std::find_if (nativePresses.begin(), nativePresses.end(), [&] (const auto& press)
    { return matchesNativeKey (key.getKeyCode(), press.key.virtualKey); });
    const std::optional<NativePress> native = found != nativePresses.end() ? std::optional<NativePress> (*found) : std::nullopt;
    if (found != nativePresses.end())
        nativePresses.erase (found);
    // A translated character can arrive after its native up and after capture or
    // mapping changed. Its original owner/generation survives physical release.
    if (native && (native->generation != service.getInputGeneration()
                   || (native->captureOwned && ! service.isCapturing())))
        return true;
    // JUCE queries asynchronous modifiers; use the modifiers of the matched down
    // instead. A panic-owned event must never turn into an unmodified GO.
    const auto observedKey = native ? juce::KeyPress (key.getKeyCode(), native->key.modifiers, key.getTextCharacter()) : key;
    auto context = contextFor (origin, observedKey);
    if (native)
    {
        context.nativeKey = native->key;
        context.nativePanicOwned = native->panicOwned;
    }
    const bool consumed = route (observedKey, origin, context, juce::Time::getMillisecondCounterHiRes(), native && native->repeat);
    if (native && native->released)
    {
        releaseKey (-native->key.virtualKey);
        flushReleases();
    }
    return consumed;
}
bool ShortcutRouter::keyStateChanged (bool isKeyDown, juce::Component*)
{
    if (! isKeyDown)
        updateHeldKeys (false); // native releases were already observed in queue order
    flushReleases();
    // Key-state must not reach default buttons/legacy shortcut listeners during
    // capture, nor while the captured key is still down after registration.
    return service.isCapturing() || std::any_of (held.begin(), held.end(), [] (const auto& p) { return p.second.quarantined; });
}

bool ShortcutRouter::route (const juce::KeyPress& key, juce::Component* origin, ShortcutKeyContext context,
                            double timeMs, bool nativeRepeat)
{
    updateHeldKeys (false);
    flushReleases();
    if (! context.applicationActive || context.window == Window::outsideApp)
        return false;
    const int code = key.getKeyCode();
    int vk = context.nativeKey ? context.nativeKey->virtualKey : 0;
    if (vk == 0)
        for (const auto& alias : ShortcutKeyInput::numberPadAliases())
            if (matchesNativeKey (code, alias.virtualKey) && callbacks.nativeKeyDown (alias.virtualKey))
            {
                vk = alias.virtualKey;
                break;
            }
    const int identity = vk != 0 ? -vk : code;
    const bool repeat = held.count (identity) != 0 || nativeRepeat;
    auto [it, inserted] = held.emplace (identity, Press { key, 0, false, nativeRepeat, timeMs, vk, context.nativeKey.has_value() });
    juce::ignoreUnused (inserted);
    context.captureActive = service.isCapturing();
    context.isRepeat = repeat;
    if (! context.commandEnabled)
        context.commandEnabled = [this] (juce::CommandID id)
        {
            juce::ApplicationCommandInfo info (id);
            return manager.getTargetForCommand (id, info) != nullptr && (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
        };
    const auto owner = service.resolveKeyOwner (key, context);
    using Kind = ShortcutKeyOwner::Kind;
    if (owner.kind == Kind::capture)
    {
        it->second.quarantined = true;
        if (! repeat && captureActivationKeys.empty() && code != 0)
        {
            auto captured = key;
            for (const auto& alias : ShortcutKeyInput::numberPadAliases())
                if (vk == alias.virtualKey)
                {
                    captured = juce::KeyPress (alias.keyCode, key.getModifiers(), 0);
                    break;
                }
            service.deliverCaptureKey (captured);
        }
        return true;
    }
    if (it->second.quarantined)
        return true;

    // Panic remains one path on Windows: callbacks.panic enters the hook's JUCE
    // fallback only when native installation failed. Standard cancellation survives.
    if (owner.commandID == CommandIDs::panicAll)
    {
        if (owner.kind == Kind::command && callbacks.panic)
            callbacks.panic (timeMs);
        return ! (context.standardUiConsumesKey || (context.textEditing && ShortcutKeyInput::isStandardTextEditorKey (key))
                  || key.isKeyCode (juce::KeyPress::escapeKey));
    }
    if (context.window == Window::nativePlugin)
        return false; // plugin-owned shortcuts stay with the plugin
    if (owner.kind == Kind::standardUi || owner.kind == Kind::none)
        return false;
    if (owner.kind == Kind::fixedComponent)
    {
        if (auto* component = componentOwner (origin))
            component->keyPressed (key); // existing behavior; false still consumes this owner's key
        return true;
    }
    if (owner.commandID == CommandIDs::go)
    {
        it->second.go = true;
        // A second alias can be out of scope after moving to a settings dialog.
        // It still belongs to the held GO group until its physical release.
        if (goLatched || owner.kind == Kind::command)
            it->second.releaseCommand = owner.commandID;
    }
    if (owner.commandID == CommandIDs::go && owner.kind == Kind::command)
    {
        if (goLatched && callbacks.requireGoKeyUp && callbacks.requireGoKeyUp())
            return true;
        goLatched = true;
    }
    if (owner.kind == Kind::command)
    {
        const auto* definition = ShortcutCatalog::get().find (owner.commandID);
        if (definition != nullptr && (definition->commandFlags & juce::ApplicationCommandInfo::wantsKeyUpDownCallbacks) != 0)
            it->second.releaseCommand = owner.commandID;
        invoke (owner.commandID, key, true, origin);
        return true;
    }
    if (callbacks.cueHotkey && (owner.kind == Kind::cueHotkey
        || (owner.kind == Kind::blocked && owner.commandID == 0 && owner.reason == ShortcutKeyOwner::Reason::repeatSuppressed
            && ! context.textEditing && context.window == Window::main
            && std::any_of (context.cueHotkeys.begin(), context.cueHotkeys.end(), [&] (const auto& cue) { return cue.id == owner.id; }))))
        callbacks.cueHotkey (key, repeat);
    return owner.shouldConsume();
}

void ShortcutRouter::invoke (juce::CommandID id, const juce::KeyPress& key, bool down, juce::Component* origin, double durationMs)
{
    juce::ApplicationCommandTarget::InvocationInfo info (id);
    info.invocationMethod = juce::ApplicationCommandTarget::InvocationInfo::fromKeyPress;
    info.keyPress = key;
    info.isKeyDown = down;
    info.originatingComponent = origin;
    info.millisecsSinceKeyPressed = static_cast<int> (juce::jlimit (0.0, 2147483647.0, durationMs));
    manager.invoke (info, false);
}
bool ShortcutRouter::anyGoKeyHeld() const
{
    return std::any_of (held.begin(), held.end(), [] (const auto& p) { return p.second.go; });
}
void ShortcutRouter::flushReleases()
{
    if (service.isCapturing())
        return;
    auto releases = std::move (pendingReleases);
    pendingReleases.clear();
    for (const auto& press : releases)
        invoke (press.releaseCommand, press.key, false, nullptr, juce::Time::getMillisecondCounterHiRes() - press.timeMs);
}
void ShortcutRouter::pollKeyState()
{
    updateHeldKeys (! nativeMessagesPending());
    flushReleases();
}
void ShortcutRouter::releaseKey (int identity)
{
    const auto it = held.find (identity);
    if (it == held.end())
        return;
    if (it->second.releaseCommand != 0)
        pendingReleases.push_back (it->second);
    captureActivationKeys.erase (identity);
    held.erase (it);
    if (! anyGoKeyHeld())
        goLatched = false;
}
void ShortcutRouter::updateHeldKeys (bool recoverNative)
{
    for (auto it = held.begin(); it != held.end();)
    {
        const auto& press = it->second;
        if (press.nativeObserved && ! recoverNative)
            ++it;
        else if (! (press.nativeVK != 0 ? callbacks.nativeKeyDown (press.nativeVK) : callbacks.keyDown (press.key.getKeyCode())))
            releaseKey ((it++)->first);
        else
            ++it;
    }
}
void ShortcutRouter::quarantineDownKeys()
{
    updateHeldKeys (false);
    for (const auto& named : ShortcutKeyCodec::allowedKeys())
    {
        int padVK = 0;
        for (const auto& alias : ShortcutKeyInput::numberPadAliases())
            if (named.code == alias.keyCode && callbacks.nativeKeyDown (alias.virtualKey))
                padVK = alias.virtualKey;
        if (padVK != 0 || callbacks.keyDown (named.code))
        {
            // Do not create a second, character-based identity for an observed VK.
            const auto existing = std::find_if (held.begin(), held.end(), [&] (const auto& p)
            { return p.second.nativeVK != 0 && matchesNativeKey (named.code, p.second.nativeVK); });
            auto& press = existing != held.end() ? existing->second : held[padVK != 0 ? -padVK : named.code];
            if (! press.key.isValid())
            {
                press.key = juce::KeyPress (named.code);
                press.nativeVK = padVK;
            }
            press.quarantined = true;
        }
    }
    for (auto& [code, press] : held)
    {
        juce::ignoreUnused (code);
        press.quarantined = true;
        // Mapping/capture transitions can introduce another held GO alias before
        // its first command down. It must keep an existing GO group closed too.
        if (std::any_of (service.getKeys (CommandIDs::go).begin(), service.getKeys (CommandIDs::go).end(),
                         [&press] (const auto& key) { return ShortcutKeyInput::keysOverlap (
                             juce::KeyPress (key.getKeyCode()), juce::KeyPress (press.key.getKeyCode())); }))
        {
            press.go = true;
            press.releaseCommand = CommandIDs::go;
        }
    }
    if (anyGoKeyHeld() && callbacks.requireGoKeyUp && callbacks.requireGoKeyUp())
        goLatched = true;
    flushReleases();
}
void ShortcutRouter::shortcutsChanged() { quarantineDownKeys(); }
void ShortcutRouter::captureStateChanged()
{
    quarantineDownKeys();
    captureActivationKeys.clear();
    if (service.isCapturing())
        for (const auto& [code, press] : held)
        {
            juce::ignoreUnused (press);
            captureActivationKeys.insert (code);
        }
}
void ShortcutRouter::applicationActiveChanged (bool value)
{
    if (value != active)
    {
        active = value;
        quarantineDownKeys();
    }
}
void ShortcutRouter::timerCallback()
{
    // Unmatched native/plugin downs must not be reused by a later JUCE event.
    // At idle every translated WM_CHAR has already had a chance to match.
    if (! nativeMessagesPending())
        nativePresses.clear();
    refreshFocus();
    applicationActiveChanged (callbacks.applicationActive());
    pollKeyState();
    if (! active)
        quarantineDownKeys();
}
}
