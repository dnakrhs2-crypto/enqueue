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
        expect (handle (window) == hwnd);
        expect (physicalBounds (window) == monitorBounds (window));
        expect ((GetWindowLongPtrW (hwnd, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZE)) == 0);
        expectEquals (window.mode.restorableState(), original);
        expect (! IsWindowVisible (hwnd));

        beginTest ("repeated enter/exit are harmless and restore exact geometry, style and state");
        window.mode.enter();
        expectEquals (window.mode.restorableState(), original);
        window.mode.exit();
        window.mode.exit();
        expect (! window.mode.isActive() && ! window.isKioskMode());
        expect (physicalBounds (window) == bounds);
        expect (GetWindowLongPtrW (hwnd, GWL_STYLE) == style);
        expectEquals (window.getWindowStateAsString(), original);
        expect (GetForegroundWindow() == foreground);
        expect (! IsWindowVisible (hwnd));

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

        beginTest ("destruction hides and releases the kiosk without restoring/activating a window");
        {
            juce::DocumentWindow closing ("FullScreen shutdown", juce::Colours::black, 0, false);
            closing.setBounds (100, 100, 960, 680);
            closing.addToDesktop();
            {
                FullScreenMode mode (closing);
                mode.enter();
                expect (closing.isKioskMode());
            }
            expect (! closing.isKioskMode());
            expect (! IsWindowVisible (handle (closing)));
            expect (juce::Desktop::getInstance().getKioskModeComponent() == nullptr);
        }

        beginTest ("maximised normal placement and minimised entry (hidden desktop only)");
        const auto desktop = desktopName();
        if (desktop.isEmpty() || desktop.equalsIgnoreCase ("Default"))
        {
            logMessage ("SKIP on Default/unknown desktop: maximise -> enter/exit -> normal placement; "
                        "minimise -> enter; refit while minimised; shutdown from maximised fullscreen. "
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
        window.setFullScreen (true);
        {
            FullScreenMode closingMode (window);
            closingMode.enter();
        }
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

        beginTest ("catalog contract: only fullscreen's unmodified default F11 yields to project cues");
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
            expect (entry.defaultKeysYieldToCueHotkeys == (entry.id == fullScreenID), entry.id);
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

        beginTest ("explicit override wins, and restoring defaults resumes yielding");
        expect (h.service->setKeys (fullScreenID, { f11 }).wasOk());
        expect (resolve().kind == Owner::Kind::command && resolve().reason == Owner::Reason::commandOverCue);
        expect (h.service->restoreCommandDefaults (fullScreenID).wasOk());
        expect (resolve().kind == Owner::Kind::cueHotkey);

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
