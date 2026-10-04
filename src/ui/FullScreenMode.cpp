#include "ui/FullScreenMode.h"
#include "app/UiScale.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <shobjidl.h>
#endif

namespace gocue
{

FullScreenMode::~FullScreenMode()
{
    if (active)
    {
        const juce::ScopedValueSetter<bool> guard (changing, true);
        window.setVisible (false);
        // Do not restore a maximised state here: Windows would show the window
        // again during shutdown. JUCE must release the kiosk before its peer dies.
        releaseKiosk();
    }
}

void FullScreenMode::enter()
{
    if (active || changing || window.getPeer() == nullptr)
        return;

    auto& desktop = juce::Desktop::getInstance();
    if (desktop.getKioskModeComponent() != nullptr)
        return; // never take another window's application-wide kiosk slot

    {
        const juce::ScopedValueSetter<bool> guard (changing, true);
        if (window.isMinimised())
            window.setMinimised (false);
        saved = window.getWindowStateAsString();
        // Clear WS_MAXIMIZE before JUCE changes the same HWND to WS_POPUP.
        if (window.isFullScreen())
            window.setFullScreen (false);
        active = true;
        desktop.setKioskModeComponent (&window, false);
        markFullscreen (true);
    }
    refit();
}

void FullScreenMode::exit()
{
    if (! active || changing)
        return;

    const juce::ScopedValueSetter<bool> guard (changing, true);
    releaseKiosk();
    // JUCE restores the normal bounds before maximising an "fs ..." state.
    window.restoreWindowStateFromString (saved);
    UiScale::fitWindowIntoDisplay (window);
}

void FullScreenMode::toggle()
{
    if (active) exit();
    else        enter();
}

juce::String FullScreenMode::restorableState()
{
    return active ? saved : window.getWindowStateAsString();
}

void FullScreenMode::releaseKiosk()
{
    active = false;
    markFullscreen (false);
    auto& desktop = juce::Desktop::getInstance();
    if (desktop.getKioskModeComponent() == &window)
        desktop.setKioskModeComponent (nullptr);
}

void FullScreenMode::markFullscreen (bool fullscreen)
{
   #if JUCE_WINDOWS
    if (auto* peer = window.getPeer())
    {
        // Shell detection based on geometry/styles alone is not reliable.
        // COM is normally initialised by JUCE; failure here is harmless.
        ITaskbarList2* taskbar = nullptr;
        if (SUCCEEDED (CoCreateInstance (CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS (&taskbar))))
        {
            if (SUCCEEDED (taskbar->HrInit()))
                taskbar->MarkFullscreenWindow (static_cast<HWND> (peer->getNativeHandle()), fullscreen ? TRUE : FALSE);
            taskbar->Release();
        }
    }
   #else
    juce::ignoreUnused (fullscreen);
   #endif
}

void FullScreenMode::refit()
{
    if (! active || changing || window.isMinimised() || ! window.isKioskMode())
        return;

    const juce::ScopedValueSetter<bool> guard (changing, true);
   #if JUCE_WINDOWS
    if (auto* peer = window.getPeer())
    {
        // Both Win32 reads and the final write use physical pixels, independent
        // of JUCE's global scale and the target monitor's Windows DPI scale.
        const auto previousDpi = SetThreadDpiAwarenessContext (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto hwnd = static_cast<HWND> (peer->getNativeHandle());
        RECT bounds {};
        MONITORINFO monitor { sizeof (MONITORINFO), {}, {}, 0 };
        if (GetWindowRect (hwnd, &bounds)
            && GetMonitorInfoW (MonitorFromRect (&bounds, MONITOR_DEFAULTTONEAREST), &monitor)
            && ! EqualRect (&bounds, &monitor.rcMonitor))
        {
            const auto& area = monitor.rcMonitor;
            SetWindowPos (hwnd, nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top,
                          SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
        if (previousDpi != nullptr)
            SetThreadDpiAwarenessContext (previousDpi);
    }
   #else
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window.getScreenBounds()))
    {
        const auto bounds = display->logicalBounds.getSmallestIntegerContainer();
        if (window.getBounds() != bounds)
            window.setBounds (bounds);
    }
   #endif
}

} // namespace gocue
