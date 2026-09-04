#include "GlobalHotkeys.h"

#include "ui/UiUtils.h"

#include <algorithm>
#include <string>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::livemix
{

#if JUCE_WINDOWS

//==============================================================================
/** A message-only window: RegisterHotKey needs an HWND on this thread to deliver WM_HOTKEY to. */
class GlobalHotkeys::Window
{
public:
    explicit Window (GlobalHotkeys& o) : owner (o)
    {
        const auto instance = (HINSTANCE) juce::Process::getCurrentModuleInstanceHandle();
        className = L"LiveMixGlobalHotkeys_" + std::to_wstring ((unsigned long long) (juce::pointer_sized_int) this);

        WNDCLASSW wc = {};
        wc.lpfnWndProc = &Window::windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = className.c_str();
        RegisterClassW (&wc);

        hwnd = CreateWindowExW (0, className.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    }

    ~Window()
    {
        if (hwnd != nullptr)
        {
            for (int id : registered)
                UnregisterHotKey (hwnd, id);

            DestroyWindow (hwnd);
        }

        UnregisterClassW (className.c_str(), (HINSTANCE) juce::Process::getCurrentModuleInstanceHandle());
    }

    bool registerKey (int id, unsigned int modifiers, unsigned int vk, juce::String& error)
    {
        if (hwnd == nullptr)
        {
            error = ko ("핫키 창을 만들지 못했습니다.");
            return false;
        }

        unregisterKey (id);

        if (! RegisterHotKey (hwnd, id, modifiers | MOD_NOREPEAT, vk))
        {
            const auto code = GetLastError();
            error = code == ERROR_HOTKEY_ALREADY_REGISTERED ? ko ("다른 프로그램이 이미 쓰는 키입니다.")
                                                             : ko ("Windows가 이 키를 핫키로 등록해 주지 않았습니다 (오류 ") + juce::String ((int) code) + ")";
            return false;
        }

        registered.push_back (id);
        return true;
    }

    void unregisterKey (int id)
    {
        if (hwnd != nullptr)
            UnregisterHotKey (hwnd, id);

        registered.erase (std::remove (registered.begin(), registered.end(), id), registered.end());
    }

private:
    static LRESULT CALLBACK windowProc (HWND h, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            auto* create = (CREATESTRUCTW*) lParam;
            SetWindowLongPtrW (h, GWLP_USERDATA, (LONG_PTR) create->lpCreateParams);
        }
        else if (message == WM_HOTKEY)
        {
            if (auto* self = (Window*) GetWindowLongPtrW (h, GWLP_USERDATA))
                if (self->owner.onHotkey)
                    self->owner.onHotkey ((int) wParam);

            return 0;
        }

        return DefWindowProcW (h, message, wParam, lParam);
    }

    GlobalHotkeys& owner;
    std::wstring className;
    HWND hwnd = nullptr;
    std::vector<int> registered;
};

#else

class GlobalHotkeys::Window
{
public:
    explicit Window (GlobalHotkeys&) {}
    bool registerKey (int, unsigned int, unsigned int, juce::String& error) { error = "not supported"; return false; }
    void unregisterKey (int) {}
};

#endif

//==============================================================================
GlobalHotkeys::GlobalHotkeys() : window (std::make_unique<Window> (*this)) {}
GlobalHotkeys::~GlobalHotkeys() = default;

bool GlobalHotkeys::set (int id, const juce::KeyPress& key, juce::String& error)
{
    error.clear();
    unsigned int modifiers = 0, vk = 0;

    if (! key.isValid() || ! toWindowsHotkey (key, modifiers, vk))
    {
        window->unregisterKey (id);

        if (key.isValid())
            error = ko ("이 키는 전역 핫키로 쓸 수 없습니다: ") + key.getTextDescription();

        return false;
    }

    return window->registerKey (id, modifiers, vk, error);
}

void GlobalHotkeys::clear (int id)
{
    window->unregisterKey (id);
}

bool GlobalHotkeys::toWindowsHotkey (const juce::KeyPress& key, unsigned int& modifiers, unsigned int& virtualKey)
{
    modifiers = 0;
    virtualKey = 0;
    const auto mods = key.getModifiers();

    if (mods.isCtrlDown())  modifiers |= 0x0002;   // MOD_CONTROL
    if (mods.isAltDown())   modifiers |= 0x0001;   // MOD_ALT
    if (mods.isShiftDown()) modifiers |= 0x0004;   // MOD_SHIFT

    int code = key.getKeyCode();

    if (code >= 'a' && code <= 'z')
        code = code - 'a' + 'A';

    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
        virtualKey = (unsigned int) code;                                                   // VK_A.. / VK_0.. are the ASCII codes
    else if (code >= juce::KeyPress::F1Key && code <= juce::KeyPress::F24Key)
        virtualKey = 0x70 + (unsigned int) (code - juce::KeyPress::F1Key);                   // VK_F1..
    else if (code >= juce::KeyPress::numberPad0 && code <= juce::KeyPress::numberPad9)
        virtualKey = 0x60 + (unsigned int) (code - juce::KeyPress::numberPad0);              // VK_NUMPAD0..
    else if (code == juce::KeyPress::spaceKey)
        virtualKey = 0x20;                                                                   // VK_SPACE
    else
        return false;

    return true;
}

juce::String GlobalHotkeys::reasonToRefuse (const juce::KeyPress& key)
{
    unsigned int modifiers = 0, vk = 0;

    if (! toWindowsHotkey (key, modifiers, vk))
        return ko ("F 키, 숫자 패드, 또는 Ctrl/Alt/Shift + 글자·숫자만 됩니다.");

    const bool plainCharacter = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == 0x20;

    if (plainCharacter && modifiers == 0)
        return ko ("글자·숫자·스페이스는 Ctrl, Alt 또는 Shift와 함께 쓰세요 (다른 프로그램에서 못 치게 됩니다).");

    return {};
}

} // namespace gocue::livemix
