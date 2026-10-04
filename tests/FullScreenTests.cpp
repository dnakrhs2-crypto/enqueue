#include "ui/FullScreenMode.h"
#include "app/UiScale.h"
#include "app/Commands.h"
#include "ShortcutTestHarness.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
namespace
{
using K = juce::KeyPress;
using Owner = ShortcutKeyOwner;
constexpr auto fullScreenID = "view.toggleFullScreen";

#if JUCE_WINDOWS
struct PhysicalPixels
{
    PhysicalPixels() : previous (SetThreadDpiAwarenessContext (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixels() { if (previous != nullptr) SetThreadDpiAwarenessContext (previous); }
    DPI_AWARENESS_CONTEXT previous;
};

juce::Rectangle<int> rectangle (RECT r) { return { r.left, r.top, r.right - r.left, r.bottom - r.top }; }
HWND handle (juce::Component& window) { return static_cast<HWND> (window.getPeer()->getNativeHandle()); }
juce::Rectangle<int> physicalBounds (juce::Component& window)
{
    const PhysicalPixels pixels;
    RECT r {};
    GetWindowRect (handle (window), &r);
    return rectangle (r);
}
juce::Rectangle<int> monitorBounds (juce::Component& window)
{
    const PhysicalPixels pixels;
    MONITORINFO info { sizeof (MONITORINFO), {}, {}, 0 };
    GetMonitorInfoW (MonitorFromWindow (handle (window), MONITOR_DEFAULTTONEAREST), &info);
    return rectangle (info.rcMonitor);
}
void physicalBounds (juce::Component& window, juce::Rectangle<int> r)
{
    const PhysicalPixels pixels;
    SetWindowPos (handle (window), nullptr, r.getX(), r.getY(), r.getWidth(), r.getHeight(),
                  SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}
juce::String desktopName()
{
    wchar_t name[256] {};
    DWORD bytes = 0;
    if (! GetUserObjectInformationW (GetThreadDesktop (GetCurrentThreadId()), UOI_NAME, name, sizeof (name), &bytes))
        return {};
    return juce::String (name);
}

void dispatchMessages()
{
    MSG message {};
    for (int n = 0; n < 2000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
}

class QuietWindow : public juce::DocumentWindow
{
public:
    QuietWindow() : DocumentWindow ("FullScreen test", juce::Colours::black, allButtons, false)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (860, 640, 10000, 10000);
        setBounds (100, 100, 960, 680);
        addToDesktop (getDesktopWindowStyleFlags()); // never show or activate this native window
        seedState();
    }
    ~QuietWindow() override { mode.shutdown(); }
    void seedState() { restoreWindowStateFromString (getBounds().toString()); }
    void closeButtonPressed() override {}
    void resized() override
    {
        DocumentWindow::resized();
        if (forwardRefit) mode.refit();
        UiScale::fitOnResized (*this, fitting);
    }
    void moved() override
    {
        DocumentWindow::moved();
        if (forwardRefit) mode.refit();
    }
    void parentSizeChanged() override
    {
        DocumentWindow::parentSizeChanged();
        if (forwardRefit) mode.refit();
    }
    bool forwardRefit = true, fitting = false;
    FullScreenMode mode { *this };
};

struct RestoreScale
{
    ~RestoreScale()
    {
        juce::Desktop::getInstance().setGlobalScaleFactor (scale);
        UiScale::lastRequestedPercent = requested;
    }
    float scale = juce::Desktop::getInstance().getGlobalScaleFactor();
    int requested = UiScale::lastRequestedPercent;
};

class FullScreenWindowTests : public juce::UnitTest
{
public:
    FullScreenWindowTests() : UnitTest ("FullScreen windows", "Enqueue") {}
    void runTest() override
    {
        const RestoreScale restoreScale;
        juce::Desktop::getInstance().setGlobalScaleFactor (1.0f);
        QuietWindow window;
        const auto original = window.getWindowStateAsString();
        const auto bounds = physicalBounds (window);
        const auto hwnd = handle (window);
        const auto style = GetWindowLongPtrW (hwnd, GWL_STYLE);
        const auto foreground = GetForegroundWindow();

        beginTest ("hidden window covers physical monitor with no caption/frame; saves only its original state");
        expect (! window.isVisible() && ! IsWindowVisible (hwnd));
        expect ((style & WS_CAPTION) == WS_CAPTION && (style & WS_THICKFRAME) != 0);
        expectEquals (window.mode.restorableState(), original);
        window.mode.enter();
        expect (window.mode.isActive() && window.isKioskMode());
        expect (static_cast<bool> (window.getProperties()[FullScreenMode::keepKioskModeWhenAppInactive]));
        expect (handle (window) == hwnd);
        expect (physicalBounds (window) == monitorBounds (window));
        expect ((GetWindowLongPtrW (hwnd, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZE)) == 0);
        expectEquals (window.mode.restorableState(), original);
        expect (! IsWindowVisible (hwnd));

        beginTest ("WM_ACTIVATEAPP(FALSE) retains kiosk mode, physical coverage and active state");
        SendMessageW (hwnd, WM_ACTIVATEAPP, FALSE, 0);
        dispatchMessages();
        expect (window.isKioskMode() && window.mode.isActive() && physicalBounds (window) == monitorBounds (window),
                "JUCE must include tools/juce-patches/0003-kiosk-mode-survives-app-switch.patch");
        expect (! IsWindowVisible (hwnd) && GetForegroundWindow() == foreground);

        beginTest ("repeated enter/exit are harmless and restore exact geometry, style and state");
        window.mode.enter();
        expectEquals (window.mode.restorableState(), original);
        window.mode.exit();
        window.mode.exit();
        expect (! window.mode.isActive() && ! window.isKioskMode());
        expect (! window.getProperties().contains (FullScreenMode::keepKioskModeWhenAppInactive));
        expect (physicalBounds (window) == bounds);
        expect (GetWindowLongPtrW (hwnd, GWL_STYLE) == style);
        expectEquals (window.getWindowStateAsString(), original);
        expect (GetForegroundWindow() == foreground);
        expect (! IsWindowVisible (hwnd));

        beginTest ("external kiosk release restores asynchronously, notifies once and permits re-entry");
        for (const bool deactivate : { true, false })
        {
            int notifications = 0;
            window.mode.onExternalExit = [&] { ++notifications; };
            window.mode.enter();
            if (deactivate)
            {
                window.getProperties().remove (FullScreenMode::keepKioskModeWhenAppInactive);
                SendMessageW (hwnd, WM_ACTIVATEAPP, FALSE, 0);
            }
            else
                juce::Desktop::getInstance().setKioskModeComponent (nullptr);
            expect (! window.mode.isActive(), "resized/moved detects external release without a manual refit");
            expectEquals (window.mode.restorableState(), original);
            dispatchMessages();
            window.mode.refit();
            window.parentSizeChanged();
            expectEquals (notifications, 1);
            expect (! window.isKioskMode() && ! window.mode.isActive());
            expect (! window.getProperties().contains (FullScreenMode::keepKioskModeWhenAppInactive));
            expectEquals (window.getWindowStateAsString(), original);
            expect (physicalBounds (window) == bounds && GetWindowLongPtrW (hwnd, GWL_STYLE) == style);
            window.mode.enter();
            expect (window.isKioskMode() && window.mode.isActive());
            expect (physicalBounds (window) == monitorBounds (window));
            window.mode.exit();
            expectEquals (notifications, 1);
            window.mode.onExternalExit = {};
        }

        beginTest ("JUCE reentrant entry refusal clears active state and the opt-in property immediately");
        {
            QuietWindow blocker;
            auto& desktop = juce::Desktop::getInstance();
            desktop.setKioskModeComponent (&blocker, false);
            struct TryDuringRelease : juce::ComponentListener
            {
                explicit TryDuringRelease (QuietWindow& w) : target (w) {}
                void componentMovedOrResized (juce::Component& component, bool, bool) override
                {
                    if (! attempted && juce::Desktop::getInstance().getKioskModeComponent() != &component)
                    {
                        attempted = true;
                        target.mode.enter(); // Desktop is still inside its kiosk reentrancy guard
                    }
                }
                QuietWindow& target;
                bool attempted = false;
            } attempt (window);
            blocker.addComponentListener (&attempt);
            int notifications = 0;
            window.mode.onExternalExit = [&] { ++notifications; };
            desktop.setKioskModeComponent (nullptr);
            blocker.removeComponentListener (&attempt);
            expect (attempt.attempted);
            expect (! window.mode.isActive() && ! window.isKioskMode());
            expect (! window.getProperties().contains (FullScreenMode::keepKioskModeWhenAppInactive));
            expectEquals (notifications, 1);
            dispatchMessages();
            expectEquals (window.getWindowStateAsString(), original);
            window.mode.enter();
            expect (window.mode.isActive() && window.isKioskMode());
            window.mode.exit();
            window.mode.onExternalExit = {};
        }

        beginTest ("UI fitting leaves kiosk bounds alone; refit repairs physical changes without logical rounding loops");
        window.mode.toggle();
        const auto full = monitorBounds (window);
        bool fitting = false;
        UiScale::fitWindowIntoDisplay (window);
        UiScale::fitOnResized (window, fitting);
        expect (physicalBounds (window) == full);
        window.forwardRefit = false;
        physicalBounds (window, full.reduced (13, 17));
        expect (physicalBounds (window) != full);
        window.mode.refit();
        expect (physicalBounds (window) == full);
        window.forwardRefit = true;
        physicalBounds (window, full.reduced (1));
        expect (physicalBounds (window) == full, "resized/moved forwards refit synchronously");
        for (int i = 0; i < 10; ++i) window.mode.refit();
        expect (physicalBounds (window) == full);

        beginTest ("110/125/150 percent round trips retain exact physical monitor coverage");
        for (const int percent : { 110, 125, 150, 125, 110, 100 })
        {
            const int applied = UiScale::apply (percent, &window);
            expect (applied == UiScale::fitPercent (percent, UiScale::workAreaAt100 (&window)));
            expect (physicalBounds (window) == monitorBounds (window), "scale " + juce::String (applied));
            expectEquals (window.mode.restorableState(), original);
            expect (! IsWindowVisible (hwnd));
        }
        window.mode.exit();
        expectEquals (window.getWindowStateAsString(), original);
        expect (physicalBounds (window) == bounds);

        beginTest ("a window on each secondary monitor covers that monitor, including negative coordinates");
        std::vector<MONITORINFO> monitors;
        {
            const PhysicalPixels pixels;
            EnumDisplayMonitors (nullptr, nullptr, [] (HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL
            {
                MONITORINFO info { sizeof (MONITORINFO), {}, {}, 0 };
                if (GetMonitorInfoW (monitor, &info))
                    reinterpret_cast<std::vector<MONITORINFO>*> (data)->push_back (info);
                return TRUE;
            }, reinterpret_cast<LPARAM> (&monitors));
        }
        if (monitors.size() < 2) logMessage ("SKIP: no secondary monitor attached");
        for (const auto& monitor : monitors)
        {
            if ((monitor.dwFlags & MONITORINFOF_PRIMARY) != 0) continue;
            const auto target = rectangle (monitor.rcMonitor);
            physicalBounds (window, { target.getX() + 100, target.getY() + 100, 960, 680 });
            window.seedState();
            window.mode.enter();
            expect (physicalBounds (window) == target);
            logMessage ("Secondary monitor: " + target.toString() + ", DPI " + juce::String (GetDpiForWindow (hwnd)));
            for (const int percent : { 110, 125, 150, 100 })
            {
                UiScale::apply (percent, &window);
                expect (physicalBounds (window) == target);
            }
            // Simulate Windows moving the fullscreen window to a different monitor.
            const auto other = rectangle (monitors.front().rcMonitor) == target
                ? rectangle (monitors.back().rcMonitor) : rectangle (monitors.front().rcMonitor);
            physicalBounds (window, other.reduced (10));
            expect (physicalBounds (window) == other);
            window.mode.exit();
        }

        beginTest ("shutdown explicitly releases the kiosk and property without activation; destructor is a fallback");
        {
            juce::DocumentWindow closing ("FullScreen shutdown", juce::Colours::black, 0, false);
            closing.setBounds (100, 100, 960, 680);
            closing.addToDesktop();
            {
                FullScreenMode mode (closing);
                mode.enter();
                expect (closing.isKioskMode());
                mode.shutdown();
                mode.shutdown();
                expect (! mode.isActive() && ! closing.isKioskMode());
                expect (! closing.getProperties().contains (FullScreenMode::keepKioskModeWhenAppInactive));
                expect (! IsWindowVisible (handle (closing)) && GetForegroundWindow() == foreground);
                mode.enter(); // leave the fallback destructor a kiosk to release
            }
            expect (! closing.isKioskMode());
            expect (! closing.getProperties().contains (FullScreenMode::keepKioskModeWhenAppInactive));
            expect (! IsWindowVisible (handle (closing)));
            expect (juce::Desktop::getInstance().getKioskModeComponent() == nullptr);
        }

        beginTest ("shutdown cancels a pending external restore and queued restoration survives window deletion");
        {
            auto closing = std::make_unique<QuietWindow>();
            closing->mode.enter();
            juce::Desktop::getInstance().setKioskModeComponent (nullptr);
            closing->mode.shutdown();
            physicalBounds (*closing, bounds.translated (20, 20));
            const auto afterShutdown = physicalBounds (*closing);
            dispatchMessages();
            expect (physicalBounds (*closing) == afterShutdown, "a cancelled restore must not change the window");
            expect (! IsWindowVisible (handle (*closing)) && GetForegroundWindow() == foreground);
            closing->mode.enter();
            juce::Desktop::getInstance().setKioskModeComponent (nullptr);
            closing.reset();
            dispatchMessages();
            expect (juce::Desktop::getInstance().getKioskModeComponent() == nullptr);
        }

        beginTest ("maximised normal placement and minimised entry (hidden desktop only)");
        const auto desktop = desktopName();
        if (desktop.isEmpty() || desktop.equalsIgnoreCase ("Default"))
        {
            logMessage ("SKIP on Default/unknown desktop: maximise -> enter/exit -> normal placement; "
                        "minimise -> enter; refit while minimised; external release from maximised fullscreen; "
                        "maximised primary -> secondary monitor entry; shutdown from maximised fullscreen. "
                        "These Win32 operations can show a window. No activation test runs here.");
            return;
        }
        logMessage ("Hidden desktop: " + desktop);
        window.restoreWindowStateFromString (original);
        window.seedState(); // a never-shown JUCE window does not update its remembered normal bounds on resize
        window.setFullScreen (true); // only the hidden desktop may show it
        const auto maximised = window.getWindowStateAsString();
        expect (maximised.startsWith ("fs "));
        const auto normalState = maximised.substring (3); // the pre-entry fs string is the restoration contract
        window.mode.enter();
        expect (! window.isFullScreen());
        expect ((GetWindowLongPtrW (hwnd, GWL_STYLE) & WS_MAXIMIZE) == 0);
        expect (physicalBounds (window) == monitorBounds (window));
        expectEquals (window.mode.restorableState(), maximised);
        window.mode.exit();
        expect (window.isFullScreen());
        expectEquals (window.getWindowStateAsString(), maximised);
        window.setFullScreen (false);
        expectEquals (window.getWindowStateAsString(), normalState);
        expect (window.getBounds() == juce::Rectangle<int>::fromString (normalState));

        beginTest ("external release restores maximised state and normal placement after message dispatch (hidden desktop only)");
        window.setFullScreen (true);
        const auto beforeExternalExit = window.getWindowStateAsString();
        int notifications = 0;
        window.mode.onExternalExit = [&] { ++notifications; };
        window.mode.enter();
        window.getProperties().remove (FullScreenMode::keepKioskModeWhenAppInactive);
        SendMessageW (hwnd, WM_ACTIVATEAPP, FALSE, 0);
        expect (! window.mode.isActive() && ! window.isFullScreen(), "maximisation must wait until after the native message");
        expectEquals (window.mode.restorableState(), beforeExternalExit);
        dispatchMessages();
        expect (window.isFullScreen());
        expectEquals (window.getWindowStateAsString(), beforeExternalExit);
        expectEquals (notifications, 1);
        window.mode.onExternalExit = {};
        window.setFullScreen (false);

        beginTest ("maximised window moved from primary to secondary enters on its current physical monitor (hidden desktop only)");
        const auto primary = std::find_if (monitors.begin(), monitors.end(), [] (const auto& info)
            { return (info.dwFlags & MONITORINFOF_PRIMARY) != 0; });
        if (monitors.size() < 2 || primary == monitors.end())
            logMessage ("SKIP: primary and secondary monitors are required");
        else
            for (const auto& monitor : monitors)
            {
                if ((monitor.dwFlags & MONITORINFOF_PRIMARY) != 0) continue;
                const auto primaryArea = rectangle (primary->rcWork);
                physicalBounds (window, { primaryArea.getX() + 100, primaryArea.getY() + 100, 960, 680 });
                window.seedState();
                window.setFullScreen (true);
                WINDOWPLACEMENT placement { sizeof (WINDOWPLACEMENT) };
                GetWindowPlacement (hwnd, &placement);
                const auto primaryPlacement = rectangle (placement.rcNormalPosition);
                physicalBounds (window, rectangle (monitor.rcWork)); // retain WS_MAXIMIZE and the old normal placement
                GetWindowPlacement (hwnd, &placement);
                expect (window.isFullScreen() && rectangle (placement.rcNormalPosition) == primaryPlacement);
                expect (monitorBounds (window) == rectangle (monitor.rcMonitor));
                const auto savedOnSecondary = window.getWindowStateAsString();
                window.mode.enter();
                expect (window.mode.isActive() && window.isKioskMode());
                expect (physicalBounds (window) == rectangle (monitor.rcMonitor));
                expectEquals (window.mode.restorableState(), savedOnSecondary);
                window.mode.exit();
                window.setFullScreen (false);
            }

        beginTest ("minimised entry and refit (hidden desktop only)");
        window.setMinimised (true);
        window.mode.enter();
        expect (! window.isMinimised() && window.isKioskMode());
        expect (physicalBounds (window) == monitorBounds (window));
        window.setMinimised (true);
        const auto minimisedBounds = physicalBounds (window);
        window.mode.refit();
        expect (window.isMinimised() && physicalBounds (window) == minimisedBounds);
        window.setMinimised (false);
        window.mode.exit();

        beginTest ("shutdown from maximised fullscreen hides without restoring or activating (hidden desktop only)");
        const auto foregroundBeforeShutdown = GetForegroundWindow();
        window.setFullScreen (true);
        window.mode.enter();
        expect (window.mode.restorableState().startsWith ("fs "));
        window.mode.shutdown();
        dispatchMessages();
        expect (! window.mode.isActive() && ! window.isFullScreen());
        expect ((GetWindowLongPtrW (hwnd, GWL_STYLE) & WS_MAXIMIZE) == 0);
        expect (! window.getProperties().contains (FullScreenMode::keepKioskModeWhenAppInactive));
        expect (GetForegroundWindow() == foregroundBeforeShutdown);
        expect (! IsWindowVisible (hwnd));
        expect (! window.isKioskMode());
    }
};
static FullScreenWindowTests fullScreenWindowTests;
#endif

class FullScreenShortcutTests : public juce::UnitTest
{
public:
    FullScreenShortcutTests() : UnitTest ("FullScreen shortcuts", "Enqueue") {}
    void runTest() override
    {
        shortcut_test::Harness h;
        const K f11 (K::F11Key);
        ShortcutKeyContext context;
        auto resolve = [&] { return h.service->resolveKeyOwner (f11, context); };

        beginTest ("catalog contract: only fullscreen's keyboard shortcuts yield to project cues");
        const auto* definition = ShortcutCatalog::get().find (CommandIDs::toggleFullScreen);
        expect (definition != nullptr);
        if (definition == nullptr) return;
        expectEquals (definition->id, juce::String (fullScreenID));
        expectEquals (definition->name, juce::String::fromUTF8 ("전체 화면"));
        expectEquals (definition->description, juce::String::fromUTF8 ("메인 창이 작업표시줄까지 덮고 모니터를 꽉 채웁니다. 한 번 더 하면 원래 창으로"));
        expectEquals (definition->menuCategory, juce::String::fromUTF8 ("편집"));
        expect (definition->category == ShortcutCategory::view && definition->scope == ShortcutScope::mainWindow);
        expect (! definition->allowsRepeat && definition->defaultKeys == ShortcutKeys { f11 });
        for (const auto& entry : ShortcutCatalog::get().getCommands())
            expect (entry.yieldsToCueHotkeys == (entry.id == fullScreenID), entry.id);
        expect (resolve().kind == Owner::Kind::command && resolve().commandID == CommandIDs::toggleFullScreen);

        beginTest ("enabled, disabled and duplicate F11 cues retain their existing routing rules");
        context.cueHotkeys = { { "cue.f11", f11 } };
        auto owner = resolve();
        expect (owner.kind == Owner::Kind::cueHotkey && owner.id == "cue.f11" && owner.commandID == 0);
        expect (std::any_of (owner.conflicts.begin(), owner.conflicts.end(), [] (const auto& c)
            { return c.kind == Owner::Kind::command && c.commandID == CommandIDs::toggleFullScreen; }));
        context.cueHotkeys[0].enabled = false;
        expect (resolve().kind == Owner::Kind::blocked && resolve().reason == Owner::Reason::disabled && resolve().commandID == 0);
        context.cueHotkeys.push_back ({ "cue.second", f11 });
        expect (resolve().kind == Owner::Kind::conflict && resolve().reason == Owner::Reason::ambiguousCueHotkey);
        context.cueHotkeys = { { "cue.f11", f11 } };
        context.isRepeat = true;
        expect (resolve().kind == Owner::Kind::blocked && resolve().reason == Owner::Reason::repeatSuppressed);
        context.isRepeat = false;

        beginTest ("explicit F11 and ordinary-key overrides always yield, as do restored defaults");
        expect (h.service->setKeys (fullScreenID, { f11 }).wasOk());
        expect (resolve().kind == Owner::Kind::cueHotkey);
        const K letter ('J');
        expect (h.service->setKeys (fullScreenID, { letter }).wasOk());
        context.cueHotkeys = { { "cue.letter", letter } };
        owner = h.service->resolveKeyOwner (letter, context);
        expect (owner.kind == Owner::Kind::cueHotkey && owner.id == "cue.letter" && owner.commandID == 0);
        expect (h.service->resolveKeyOwner (letter, {}).commandID == CommandIDs::toggleFullScreen);
        expect (h.service->restoreCommandDefaults (fullScreenID).wasOk());
        context.cueHotkeys = { { "cue.f11", f11 } };
        expect (resolve().kind == Owner::Kind::cueHotkey);

        beginTest ("v1 and v2 export/import round trips preserve F11 cue priority despite frozen default overrides");
        for (const auto& xml : { h.service->exportProfile(), h.service->exportCombinedProfile() })
        {
            shortcut_test::Harness imported;
            imported.service->setInputStorage ([] (const auto&) { return juce::Result::ok(); });
            expect (imported.service->importProfile (xml).wasOk());
            expect (imported.service->getProfile().overrides.count (fullScreenID) == 1);
            expect (imported.service->getKeys (fullScreenID) == ShortcutKeys { f11 });
            owner = imported.service->resolveKeyOwner (f11, context);
            expect (owner.kind == Owner::Kind::cueHotkey && owner.id == "cue.f11" && owner.commandID == 0);
        }

        beginTest ("text editing and auxiliary windows block F11 cues instead of toggling fullscreen");
        context.textEditing = true;
        expect (resolve().kind == Owner::Kind::blocked && resolve().commandID == 0);
        context.textEditing = false;
        context.window = ShortcutKeyContext::Window::auxiliary;
        expect (resolve().kind == Owner::Kind::blocked && resolve().commandID == 0);
        context.window = ShortcutKeyContext::Window::main;
        context.cueHotkeys[0].inActiveContainer = false;
        expect (resolve().commandID == 0); // an out-of-container conflict still cannot toggle the view
        context.cueHotkeys = { { "cue.shift", K (K::F11Key, juce::ModifierKeys::shiftModifier, 0) } };
        expect (resolve().kind == Owner::Kind::command && resolve().commandID == CommandIDs::toggleFullScreen);
        expect (h.service->resolveKeyOwner (context.cueHotkeys[0].key, context).kind == Owner::Kind::cueHotkey);

        beginTest ("other default command keys and application panic keep command-over-cue priority");
        for (const int code : { K::spaceKey, int ('P'), int ('F'), int ('V'), int ('L'), K::insertKey, K::deleteKey, K::F3Key, K::escapeKey })
        {
            const K key (code);
            context.cueHotkeys = { { "legacy.cue", key } };
            owner = h.service->resolveKeyOwner (key, context);
            expect (owner.kind == Owner::Kind::command && owner.reason == Owner::Reason::commandOverCue, key.getTextDescription());
        }
        context.textEditing = true;
        context.window = ShortcutKeyContext::Window::auxiliary;
        owner = h.service->resolveKeyOwner (K (K::escapeKey), context);
        expect (owner.kind == Owner::Kind::command && owner.commandID == CommandIDs::panicAll);
    }
};
static FullScreenShortcutTests fullScreenShortcutTests;
} // namespace
} // namespace gocue::tests
