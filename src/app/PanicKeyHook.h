#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <optional>

namespace gocue
{
class ShortcutService;

/** VK and modifiers are independent of JUCE's extended key-code tag and the
    Windows event's extended bit. NumLock-off navigation follows JUCE navigation. */
struct PanicKeyBinding
{
    int virtualKey = 0, modifiers = 0;
};

/** One UI-thread WH_KEYBOARD observer. Never consumes a Windows event. */
class PanicKeyHook
{
public:
    struct Conversion
    {
        std::optional<PanicKeyBinding> binding;
        juce::String reason;
    };
    // Same packed result as VkKeyScanExW, or -1. Injectable for layout tests.
    using OemResolver = std::function<int (juce::juce_wchar)>;
    static Conversion convert (const juce::KeyPress&, OemResolver = {});
    /** Convert GetMessageTime's wrapping boot-clock milliseconds onto the same
        high-resolution time axis as JUCE/menu panic input. Injectable clock samples. */
    static double messageTimeToHiRes (uint32_t messageTime, uint32_t tickCount, double nowMs);

    struct Event
    {
        double timeMs = 0;
        uint64_t generation = 0, serial = 0;
    };
    using Handler = std::function<void (double timeMs, bool hardStop)>;
    PanicKeyHook (ShortcutService&, Handler);
    ~PanicKeyHook();
    PanicKeyHook (const PanicKeyHook&) = delete;
    PanicKeyHook& operator= (const PanicKeyHook&) = delete;
    juce::Result install();
    bool isInstalled() const;

    /** Event-time observation, shared with fake native events in tests. */
    std::optional<Event> observe (int virtualKey, int modifiers, bool down, bool repeat,
                                 double timeMs, bool applicationActive = true);
    std::optional<Event> observeWindowsEvent (int virtualKey, int modifiers, uintptr_t flags,
                                             double timeMs, bool applicationActive = true);
    void dispatch (const Event&);
    void fromJuce (double timeMs); // only active when install failed / non-Windows
    void fromUi (double timeMs);   // menu/button: same gesture, independent of hook availability

    /** Runs before JUCE dispatch, so even a newly-created native/modal window's
        focused JUCE component has its router listener before its first key. */
    std::function<void (int virtualKey, int modifiers, bool down, bool repeat)> beforeDispatch;

private:
    struct State;
    std::shared_ptr<State> state;
    void* nativeHook = nullptr;
};
}
