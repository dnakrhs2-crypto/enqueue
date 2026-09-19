#pragma once

#include "app/ShortcutService.h"
#include <deque>
#include <map>
#include <set>

namespace gocue
{
/** The only keyboard command/cue dispatcher. Listeners are installed on descendants,
    including newly added editors, BEFORE their own keyPressed implementation runs.
    Fixed component owners are called once and their return value never causes fallback. */
class ShortcutRouter : public juce::KeyListener,
                       private juce::ComponentListener,
                       private juce::FocusChangeListener,
                       private juce::Timer,
                       private ShortcutService::Listener
{
public:
    using Window = ShortcutKeyContext::Window;
    struct Callbacks
    {
        std::function<void (ShortcutKeyContext&)> context;
        std::function<void (const juce::KeyPress&, bool repeat)> cueHotkey;
        std::function<void (double timeMs)> panic;
        std::function<bool()> requireGoKeyUp;
        std::function<bool (int keyCode)> keyDown;
        std::function<bool (int virtualKey)> nativeKeyDown;
        std::function<bool()> applicationActive;
    };
    ShortcutRouter (ShortcutService&, juce::ApplicationCommandManager&, Callbacks);
    ~ShortcutRouter() override;

    void attach (juce::Component&, Window);
    /** The application opts in once. Window factories register synchronously so
        the JUCE-only fallback does not depend on a timer/focus notification. */
    void activateDesktopRouting();
    static void watchWindow (juce::Component*);
    void refreshFocus();
    void prepareNativeEvent (int virtualKey, int modifiers, bool down, bool repeat);
    bool keyPressed (const juce::KeyPress&, juce::Component* origin) override;
    bool keyStateChanged (bool, juce::Component*) override;

    /** Deterministic entry points also used by fake physical-key tests. No ID lookup
        in callers: ownership always goes through ShortcutService. */
    bool route (const juce::KeyPress&, juce::Component* origin, ShortcutKeyContext,
                double timeMs, bool nativeRepeat = false);
    void pollKeyState();
    void applicationActiveChanged (bool active);
    bool anyGoKeyHeld() const;
    ShortcutKeyContext contextFor (juce::Component*, const juce::KeyPress&) const;

    static void setComponentScope (juce::Component& c, ShortcutScope scope)
    { c.getProperties().set ("shortcutScope", static_cast<int> (scope)); }
    static void setWindowScope (juce::Component&, Window);

private:
    struct Press
    {
        juce::KeyPress key;
        juce::CommandID releaseCommand = 0;
        bool go = false, quarantined = false;
        double timeMs = 0;
        int nativeVK = 0;
        bool nativeObserved = false;
    };
    struct NativePress
    {
        PanicKeyBinding key;
        bool repeat = false, released = false, panicOwned = false;
        bool captureOwned = false;
        uint64_t generation = 0;
    };
    void watchTree (juce::Component&);
    void componentChildrenChanged (juce::Component&) override;
    void componentBeingDeleted (juce::Component&) override;
    void globalFocusChanged (juce::Component*) override;
    void timerCallback() override;
    void shortcutsChanged() override;
    void captureStateChanged() override;
    void quarantineDownKeys();
    void updateHeldKeys (bool recoverNative = true);
    void releaseKey (int identity);
    void invoke (juce::CommandID, const juce::KeyPress&, bool down, juce::Component*, double durationMs = 0);
    void flushReleases();
    juce::Component* componentOwner (juce::Component*) const;

    ShortcutService& service;
    juce::ApplicationCommandManager& manager;
    Callbacks callbacks;
    std::map<juce::Component*, juce::Component::SafePointer<juce::Component>> watched;
    std::map<int, Press> held;
    std::vector<Press> pendingReleases;
    std::set<int> captureActivationKeys;
    bool active = true, goLatched = false;
    std::deque<NativePress> nativePresses;
};
}
