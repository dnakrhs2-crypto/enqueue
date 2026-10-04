#include "ui/FullScreenMode.h"
#include "app/UiScale.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <shobjidl.h>
#endif

namespace gocue
{

#if JUCE_WINDOWS
namespace
{
struct PhysicalPixels
{
    PhysicalPixels() : previous (SetThreadDpiAwarenessContext (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixels() { if (previous != nullptr) SetThreadDpiAwarenessContext (previous); }
    DPI_AWARENESS_CONTEXT previous;
};
}
#endif

FullScreenMode::~FullScreenMode()
{
    shutdown();
}

void FullScreenMode::shutdown()
{
    const bool needsRelease = active || isRestorePending();
    pendingRestore.reset();
    if (needsRelease)
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
    if (active || changing || isRestorePending() || window.getPeer() == nullptr)
        return;

    auto& desktop = juce::Desktop::getInstance();
    if (desktop.getKioskModeComponent() != nullptr)
        return; // never take another window's application-wide kiosk slot

    {
        const juce::ScopedValueSetter<bool> guard (changing, true);
        if (window.isMinimised())
            window.setMinimised (false);
        saved = window.getWindowStateAsString();
        juce::Rectangle<int> initialMonitor;
       #if JUCE_WINDOWS
        {
            // Capture before unmaximising or entering JUCE's logical kiosk fit:
            // either can move the HWND away from the monitor it currently occupies.
            const PhysicalPixels pixels;
            MONITORINFO monitor { sizeof (MONITORINFO), {}, {}, 0 };
            if (GetMonitorInfoW (MonitorFromWindow (static_cast<HWND> (window.getPeer()->getNativeHandle()),
                                                   MONITOR_DEFAULTTONEAREST), &monitor))
            {
                const auto& area = monitor.rcMonitor;
                initialMonitor = { area.left, area.top, area.right - area.left, area.bottom - area.top };
            }
        }
       #endif
        // Clear WS_MAXIMIZE before JUCE changes the same HWND to WS_POPUP.
        if (window.isFullScreen())
            window.setFullScreen (false);
        active = true;
        window.getProperties().set (keepKioskModeWhenAppInactive, true);
        desktop.setKioskModeComponent (&window, false);
        if (window.isKioskMode())
        {
            markFullscreen (true);
            fitToMonitor (initialMonitor);
        }
    }
    refit(); // also reconcile immediately if JUCE's reentrancy guard refused entry
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
    return active || isRestorePending() ? saved : window.getWindowStateAsString();
}

void FullScreenMode::releaseKiosk()
{
    active = false;
    markFullscreen (false);
    window.getProperties().remove (keepKioskModeWhenAppInactive);
    auto& desktop = juce::Desktop::getInstance();
    if (desktop.getKioskModeComponent() == &window)
        desktop.setKioskModeComponent (nullptr);
}

void FullScreenMode::reconcileExternalExit()
{
    {
        const juce::ScopedValueSetter<bool> guard (changing, true);
        releaseKiosk();
        pendingRestore = std::make_shared<bool> (true);
        // JUCE may still be restoring its own kiosk bounds in a native message.
        // Do not capture this: either the mode or the window may die before dispatch.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::ResizableWindow> (&window),
                                          pending = std::weak_ptr<bool> (pendingRestore), state = saved]
        {
            if (const auto restore = pending.lock())
            {
                if (safe != nullptr && ! safe->isKioskMode())
                {
                    safe->restoreWindowStateFromString (state);
                    if (safe != nullptr)
                        UiScale::fitWindowIntoDisplay (*safe);
                }
                *restore = false;
            }
        });
    }
    if (onExternalExit)
        onExternalExit();
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
    if (! active || changing)
        return;
    if (juce::Desktop::getInstance().getKioskModeComponent() != &window)
    {
        reconcileExternalExit();
        return;
    }
    if (window.isMinimised())
        return;

    const juce::ScopedValueSetter<bool> guard (changing, true);
    fitToMonitor();
}

void FullScreenMode::fitToMonitor (juce::Rectangle<int> initialMonitor)
{
   #if JUCE_WINDOWS
    if (auto* peer = window.getPeer())
    {
        // Both Win32 reads and the final write use physical pixels, independent
        // of JUCE's global scale and the target monitor's Windows DPI scale.
        const PhysicalPixels pixels;
        const auto hwnd = static_cast<HWND> (peer->getNativeHandle());
        RECT bounds {};
        MONITORINFO monitor { sizeof (MONITORINFO), {}, {}, 0 };
        if (initialMonitor.isEmpty()
            && GetMonitorInfoW (MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            const auto& area = monitor.rcMonitor;
            initialMonitor = { area.left, area.top, area.right - area.left, area.bottom - area.top };
        }
        const RECT target { initialMonitor.getX(), initialMonitor.getY(), initialMonitor.getRight(), initialMonitor.getBottom() };
        if (! initialMonitor.isEmpty() && GetWindowRect (hwnd, &bounds) && ! EqualRect (&bounds, &target))
        {
            SetWindowPos (hwnd, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                          SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
    }
   #else
    juce::ignoreUnused (initialMonitor);
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window.getScreenBounds()))
    {
        const auto bounds = display->logicalBounds.getSmallestIntegerContainer();
        if (window.getBounds() != bounds)
            window.setBounds (bounds);
    }
   #endif
}

} // namespace gocue
