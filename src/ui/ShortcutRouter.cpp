#include "ui/ShortcutRouter.h"
#include "app/Commands.h"
#include "app/ShortcutKeyInput.h"

namespace gocue
{
namespace
{
ShortcutRouter* desktopRouter = nullptr;
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
    // WM_CHAR can follow key-up in an already-populated message queue. Keep the
    // last down observation until the next down, and match it to the JUCE key.
    if (down)
    {
        nativeVK = vk;
        nativeModifiers = modifiers;
        nativeRepeating = repeat;
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
    auto context = contextFor (origin, key);
    const auto converted = PanicKeyHook::convert (juce::KeyPress (key.getKeyCode(),
        juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier, 0));
    const int code = key.getKeyCode();
    const bool padCharacter = (nativeVK == 0x6a && code == '*') || (nativeVK == 0x6b && code == '+')
        || (nativeVK == 0x6c && code == ',') || (nativeVK == 0x6d && code == '-')
        || (nativeVK == 0x6e && (code == '.' || code == ',')) || (nativeVK == 0x6f && code == '/');
    const bool matchedNative = nativeVK != 0 && nativeModifiers == key.getModifiers().getRawFlags()
        && (padCharacter || (converted.binding && converted.binding->virtualKey == nativeVK));
    if (matchedNative)
        context.nativeKey = PanicKeyBinding { nativeVK, nativeModifiers };
    return route (key, origin, context, juce::Time::getMillisecondCounterHiRes(), matchedNative && nativeRepeating);
}
bool ShortcutRouter::keyStateChanged (bool, juce::Component*)
{
    pollKeyState();
    // Key-state must not reach default buttons/legacy shortcut listeners during
    // capture, nor while the captured key is still down after registration.
    return service.isCapturing() || std::any_of (held.begin(), held.end(), [] (const auto& p) { return p.second.quarantined; });
}

bool ShortcutRouter::route (const juce::KeyPress& key, juce::Component* origin, ShortcutKeyContext context,
                            double timeMs, bool nativeRepeat)
{
    pollKeyState();
    if (! context.applicationActive || context.window == Window::outsideApp)
        return false;
    const int code = key.getKeyCode();
    const bool repeat = held.count (code) != 0 || nativeRepeat;
    auto [it, inserted] = held.emplace (code, Press { key, 0, false, nativeRepeat, timeMs });
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
            service.deliverCaptureKey (key);
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
    updateHeldKeys();
    flushReleases();
}
void ShortcutRouter::updateHeldKeys()
{
    for (auto it = held.begin(); it != held.end();)
    {
        if (! callbacks.keyDown (it->first))
        {
            if (it->second.releaseCommand != 0)
                pendingReleases.push_back (it->second);
            captureActivationKeys.erase (it->first);
            it = held.erase (it);
        }
        else
            ++it;
    }
    if (! anyGoKeyHeld())
        goLatched = false;
}
void ShortcutRouter::quarantineDownKeys()
{
    updateHeldKeys();
    for (const auto& named : ShortcutKeyCodec::allowedKeys())
        if (callbacks.keyDown (named.code))
        {
            auto& press = held[named.code];
            if (! press.key.isValid())
                press.key = juce::KeyPress (named.code);
            press.quarantined = true;
        }
    for (auto& [code, press] : held)
    {
        press.quarantined = true;
        // Mapping/capture transitions can introduce another held GO alias before
        // its first command down. It must keep an existing GO group closed too.
        if (std::any_of (service.getKeys (CommandIDs::go).begin(), service.getKeys (CommandIDs::go).end(),
                         [code] (const auto& key) { return key.getKeyCode() == code; }))
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
    refreshFocus();
    applicationActiveChanged (callbacks.applicationActive());
    pollKeyState();
    if (! active)
        quarantineDownKeys();
}
}
