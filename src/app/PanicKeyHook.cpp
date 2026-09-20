#include "app/PanicKeyHook.h"
#include "app/ShortcutService.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue
{
PanicKeyHook::Conversion PanicKeyHook::convert (const juce::KeyPress& key, OemResolver oem)
{
    using K = juce::KeyPress;
    using M = juce::ModifierKeys;
    const auto unsupported = []
    {
        return Conversion { {}, juce::String::fromUTF8 ("패닉용 등록 불가(플러그인 창에서도 동작하는 전체 정지 키로 쓸 수 없음)") };
    };
    constexpr int allowed = M::ctrlModifier | M::altModifier | M::shiftModifier;
    const int mods = key.getModifiers().getRawFlags();
    if ((mods & ~allowed) != 0)
        return unsupported();
    const int rawCode = key.getKeyCode();
    // Do not pass tagged JUCE codes to a Unicode/Win32 character function: its
    // wchar_t conversion can silently turn F1 (0x10070) into the letter P.
    const int code = rawCode >= 'a' && rawCode <= 'z' ? rawCode - 'a' + 'A' : rawCode;
    const auto mapped = [mods] (int vk) { return Conversion { PanicKeyBinding { vk, mods }, {} }; };
    // ASCII letter/digit codes are explicitly the corresponding Windows VKs.
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
        return mapped (code);
    struct Pair { int juceCode, vk; };
    const Pair table[] {
        { K::spaceKey, 0x20 }, { K::escapeKey, 0x1b }, { K::returnKey, 0x0d }, { K::tabKey, 0x09 },
        { K::backspaceKey, 0x08 }, { K::deleteKey, 0x2e }, { K::insertKey, 0x2d },
        { K::leftKey, 0x25 }, { K::upKey, 0x26 }, { K::rightKey, 0x27 }, { K::downKey, 0x28 },
        { K::homeKey, 0x24 }, { K::endKey, 0x23 }, { K::pageUpKey, 0x21 }, { K::pageDownKey, 0x22 },
        { K::F1Key, 0x70 }, { K::F2Key, 0x71 }, { K::F3Key, 0x72 }, { K::F4Key, 0x73 },
        { K::F5Key, 0x74 }, { K::F6Key, 0x75 }, { K::F7Key, 0x76 }, { K::F8Key, 0x77 },
        { K::F9Key, 0x78 }, { K::F10Key, 0x79 }, { K::F11Key, 0x7a }, { K::F12Key, 0x7b },
        { K::F13Key, 0x7c }, { K::F14Key, 0x7d }, { K::F15Key, 0x7e }, { K::F16Key, 0x7f },
        { K::F17Key, 0x80 }, { K::F18Key, 0x81 }, { K::F19Key, 0x82 }, { K::F20Key, 0x83 },
        { K::F21Key, 0x84 }, { K::F22Key, 0x85 }, { K::F23Key, 0x86 }, { K::F24Key, 0x87 },
        { K::numberPad0, 0x60 }, { K::numberPad1, 0x61 }, { K::numberPad2, 0x62 }, { K::numberPad3, 0x63 },
        { K::numberPad4, 0x64 }, { K::numberPad5, 0x65 }, { K::numberPad6, 0x66 }, { K::numberPad7, 0x67 },
        { K::numberPad8, 0x68 }, { K::numberPad9, 0x69 }, { K::numberPadMultiply, 0x6a },
        { K::numberPadAdd, 0x6b }, { K::numberPadSeparator, 0x6c }, { K::numberPadSubtract, 0x6d },
        { K::numberPadDecimalPoint, 0x6e }, { K::numberPadDivide, 0x6f }, { K::numberPadEquals, 0x92 }
    };
    for (const auto& pair : table)
        if (key.getKeyCode() == pair.juceCode)
            return mapped (pair.vk);

    if (! juce::String (",./\\;'[]-=`+").containsChar (static_cast<juce::juce_wchar> (code)))
        return unsupported();
   #if JUCE_WINDOWS
    if (! oem)
        oem = [] (juce::juce_wchar ch) { return static_cast<int> (VkKeyScanExW (static_cast<WCHAR> (ch), GetKeyboardLayout (0))); };
   #endif
    const int scan = oem ? oem (static_cast<juce::juce_wchar> (code)) : -1;
    if (scan < 0)
        return unsupported();
    // These are VkKeyScan's documented packed fields, not truncated JUCE codes.
    const int vk = scan % 256, required = scan / 256;
    const int registered = (key.getModifiers().isShiftDown() ? 1 : 0)
                         | (key.getModifiers().isCtrlDown() ? 2 : 0)
                         | (key.getModifiers().isAltDown() ? 4 : 0);
    if (vk == 0 || (required & ~7) != 0 || (required & registered) != required)
        return unsupported();
    return mapped (vk);
}

double PanicKeyHook::messageTimeToHiRes (uint32_t messageTime, uint32_t tickCount, double nowMs)
{
    // Unsigned subtraction handles both LONG's sign boundary and the 32-bit wrap.
    const uint32_t ageMs = tickCount - messageTime;
    return nowMs - static_cast<double> (ageMs);
}

struct PanicKeyHook::State : ShortcutService::Listener
{
    State (ShortcutService& s, Handler h) : service (s), handler (std::move (h)) { service.addListener (this); }
    ~State() override { service.removeListener (this); }
    void shortcutsChanged() override { service.panicGestures().invalidate(); }
    void captureStateChanged() override { service.panicGestures().invalidate(); }
    std::optional<Event> observe (int vk, int mods, bool down, bool repeat, double time, bool active)
    {
        if (! active || ! down || repeat || service.isCapturing())
            return {};
        for (const auto& binding : service.getPanicBindings())
            if (vk == binding.virtualKey && mods == binding.modifiers)
                return Event { time, service.getInputGeneration(), InputInvocation::nextEventID() };
        return {};
    }
    void dispatch (const Event& event)
    {
        if (event.generation != service.getInputGeneration() || service.isCapturing())
            return;
        if (const auto hard = service.panicGestures().activate (event.serial, event.timeMs); hard && handler)
            handler (event.timeMs, *hard);
    }
    ShortcutService& service;
    Handler handler;
};

#if JUCE_WINDOWS
namespace
{
PanicKeyHook* threadHookOwner = nullptr; // installed on the single JUCE message thread
LRESULT CALLBACK keyboardProc (int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && threadHookOwner != nullptr)
    {
        const auto bits = static_cast<uintptr_t> (lParam);
        const bool down = (bits & (uintptr_t (1) << 31)) == 0;
        const bool repeat = (bits & (uintptr_t (1) << 30)) != 0;
        int mods = 0;
        if ((GetKeyState (VK_CONTROL) & 0x8000) != 0) mods |= juce::ModifierKeys::ctrlModifier;
        if ((GetKeyState (VK_MENU) & 0x8000) != 0) mods |= juce::ModifierKeys::altModifier;
        if ((GetKeyState (VK_SHIFT) & 0x8000) != 0) mods |= juce::ModifierKeys::shiftModifier;
        // Win chords must not accidentally match a modifier-free panic binding.
        if (((GetKeyState (VK_LWIN) | GetKeyState (VK_RWIN)) & 0x8000) != 0) mods |= 0x100000;
        auto* owner = threadHookOwner;
        const auto messageTime = static_cast<uint32_t> (GetMessageTime());
        const auto tickCount = static_cast<uint32_t> (GetTickCount());
        const auto timeMs = PanicKeyHook::messageTimeToHiRes (messageTime, tickCount,
                                                           juce::Time::getMillisecondCounterHiRes());
        // Capture/generation/modifiers/time are all observed before any JUCE callback.
        if (const auto event = owner->observeWindowsEvent (static_cast<int> (wParam), mods, bits,
                                                          timeMs, juce::Process::isForegroundProcess()))
            owner->dispatch (*event); // dispatch below queues native events through the lifetime-safe state
        if (owner->beforeDispatch)
            owner->beforeDispatch (static_cast<int> (wParam), mods, down, repeat);
        if (owner->beforeTimedDispatch)
            owner->beforeTimedDispatch (static_cast<int> (wParam), mods, down, repeat, timeMs);
    }
    return CallNextHookEx (nullptr, code, wParam, lParam);
}
}
#endif

PanicKeyHook::PanicKeyHook (ShortcutService& service, Handler handler)
    : state (std::make_shared<State> (service, std::move (handler))) {}
PanicKeyHook::~PanicKeyHook()
{
   #if JUCE_WINDOWS
    if (nativeHook != nullptr)
        UnhookWindowsHookEx (static_cast<HHOOK> (nativeHook));
    if (threadHookOwner == this)
        threadHookOwner = nullptr;
   #endif
}
juce::Result PanicKeyHook::install()
{
   #if JUCE_WINDOWS
    if (nativeHook != nullptr)
        return juce::Result::ok();
    if (threadHookOwner != nullptr)
        return juce::Result::fail ("A panic hook is already installed");
    nativeHook = SetWindowsHookExW (WH_KEYBOARD, keyboardProc, nullptr, GetCurrentThreadId());
    if (nativeHook == nullptr)
        return juce::Result::fail ("SetWindowsHookExW: " + juce::String (static_cast<int> (GetLastError())));
    threadHookOwner = this;
   #endif
    return juce::Result::ok();
}
bool PanicKeyHook::isInstalled() const { return nativeHook != nullptr; }
std::optional<PanicKeyHook::Event> PanicKeyHook::observe (int vk, int modifiers, bool down, bool repeat, double time, bool active)
{
    return state->observe (vk, modifiers, down, repeat, time, active);
}
std::optional<PanicKeyHook::Event> PanicKeyHook::observeWindowsEvent (int vk, int modifiers, uintptr_t flags, double time, bool active)
{
    return observe (vk, modifiers, (flags & (uintptr_t (1) << 31)) == 0,
                    (flags & (uintptr_t (1) << 30)) != 0, time, active);
}
void PanicKeyHook::dispatch (const Event& event)
{
    const std::weak_ptr<State> weak = state;
    juce::MessageManager::callAsync ([weak, event] { if (const auto live = weak.lock()) live->dispatch (event); });
}
void PanicKeyHook::fromJuce (double timeMs) { if (! isInstalled()) fromUi (timeMs); }
void PanicKeyHook::fromUi (double timeMs)
{
    state->dispatch ({ timeMs, state->service.getInputGeneration(), InputInvocation::nextEventID() });
}
}
