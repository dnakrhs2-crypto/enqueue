#include "ui/ShortcutRouter.h"
#include "ui/PluginWindows.h"

#include "ui/UiUtils.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue
{

namespace
{
   #if JUCE_WINDOWS
    class ScopedForegroundWindow
    {
    public:
        explicit ScopedForegroundWindow (bool activate)
        {
            if (activate)
            {
                window = CreateWindowExW (WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                                          0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
                if (window != nullptr)
                    SetForegroundWindow (window);
            }
        }

        ~ScopedForegroundWindow()
        {
            if (window != nullptr)
                DestroyWindow (window);
        }

    private:
        HWND window = nullptr;
        JUCE_DECLARE_NON_COPYABLE (ScopedForegroundWindow)
    };
   #endif

    void claimForeground (PluginEditorWindow& window)
    {
       #if JUCE_WINDOWS
        if (auto* peer = window.getPeer())
            SetForegroundWindow ((HWND) peer->getNativeHandle());
       #else
        juce::ignoreUnused (window);
       #endif
    }

    bool hasReachableTitleBar (PluginEditorWindow& window, juce::Point<int> position)
    {
        const auto frame = window.getPeer()->getFrameSize();
        const int height = juce::jmax (1, frame.getTop());
        // Leave the native caption buttons out: a visible close button alone is not a draggable title bar.
        const juce::Rectangle<int> title (position.x, position.y - height, juce::jmax (1, window.getWidth() - 100), height);
        for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
        {
            const auto visible = display.userBounds.getLargestIntegerWithin().getIntersection (title);
            if (visible.getWidth() >= juce::jmin (80, title.getWidth())
                && visible.getHeight() >= juce::jmin (16, height))
                return true;
        }
        return false;
    }
}

PluginEditorWindow::PluginEditorWindow (juce::AudioPluginInstance& p, const juce::String& title,
                                        std::function<void (PluginEditorWindow&)> closeCallback,
                                        bool activateFromHotkey,
                                        std::function<void (PluginEditorWindow&)> placeWindow)
    : DocumentWindow (title, Palette::background, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      plugin (p),
      onClose (std::move (closeCallback))
{
    ShortcutRouter::setWindowScope (*this, ShortcutKeyContext::Window::nativePlugin);
    setUsingNativeTitleBar (true);

   #if JUCE_WINDOWS
    // setResizable can replace the editor's peer. Keep the foreground on a separate hidden window
    // until the final peer is ready, so it never falls back to another app or the main window.
    const ScopedForegroundWindow foreground (activateFromHotkey);
   #endif

    juce::AudioProcessorEditor* editor = nullptr;

    try
    {
        if (plugin.hasEditor())
            editor = plugin.createEditorAndMakeActive();
    }
    catch (...)
    {
        editor = nullptr;   // a plugin whose editor throws still gets a generic one
    }

    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (plugin);

    const bool canResize = editor->isResizable();
    setContentOwned (editor, true);
    setResizable (canResize, false);
    centreAroundComponent (nullptr, getWidth(), getHeight());   // on the active window's display, inside it (a GUI taller than a portrait screen keeps its title bar)
    if (placeWindow)
        placeWindow (*this);
    setVisible (true);
    if (activateFromHotkey)
        claimForeground (*this);
    toFront (true);
}

PluginEditorWindow::~PluginEditorWindow()
{
    clearContentComponent();   // deletes the editor, which detaches itself from the processor
}

void PluginEditorWindow::closeButtonPressed()
{
    if (onClose)
        onClose (*this);
}

//==============================================================================
PluginWindowManager::~PluginWindowManager()
{
    closeAll();
}

void PluginWindowManager::open (juce::AudioPluginInstance& plugin, const juce::String& title)
{
    openWindow (plugin, title, false);
}

void PluginWindowManager::openInForeground (juce::AudioPluginInstance& plugin, const juce::String& title)
{
    openWindow (plugin, title, true);
}

PluginEditorWindow* PluginWindowManager::findWindow (const juce::AudioPluginInstance& plugin) const
{
    for (const auto& w : windows)
        if (&w->getPlugin() == &plugin)
            return w.get();
    return nullptr;
}

bool PluginWindowManager::isOpen (const juce::AudioPluginInstance& plugin) const
{
    return findWindow (plugin) != nullptr;
}

bool PluginWindowManager::isInUse (const juce::AudioPluginInstance& plugin) const
{
    const auto* window = findWindow (plugin);
    if (window == nullptr)
        return false;
    if (isWindowInUse)
        return isWindowInUse (*window);

   #if JUCE_WINDOWS
    const auto handle = (HWND) window->getPeer()->getNativeHandle();
    for (auto foreground = GetForegroundWindow(); foreground != nullptr; foreground = GetWindow (foreground, GW_OWNER))
    {
        if (foreground == handle || IsChild (handle, foreground))
            return true;
    }
    return false;
   #else
    return window->isActiveWindow();
   #endif
}

bool PluginWindowManager::closeByUser (juce::AudioPluginInstance& plugin)
{
    if (auto* window = findWindow (plugin))
    {
        closeWindow (*window);
        return true;
    }
    return false;
}

void PluginWindowManager::setRememberWindowPositions (bool remember)
{
    rememberWindowPositions = remember;
    if (! remember)
        positions.clear();
}

void PluginWindowManager::openWindow (juce::AudioPluginInstance& plugin, const juce::String& title, bool activateFromHotkey)
{
    if (auto* window = findWindow (plugin))
    {
        window->setName (title);
        if (activateFromHotkey)
            claimForeground (*window);
        window->toFront (true);
        return;
    }

    windows.push_back (std::make_unique<PluginEditorWindow> (plugin, title,
        [this] (PluginEditorWindow& w) { closeWindow (w); }, activateFromHotkey,
        [this] (PluginEditorWindow& w) { if (rememberWindowPositions) placeWindow (w); }));
}

void PluginWindowManager::placeWindow (PluginEditorWindow& window)
{
    if (const auto found = positions.find (&window.getPlugin()); found != positions.end())
    {
        if (hasReachableTitleBar (window, found->second))
            window.setTopLeftPosition (found->second);
        return;   // an unreachable saved position keeps the normal centred placement
    }

    // Cascade from the most recently opened overlapping editor, including differently sized editors.
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if (! (*it)->isMinimised() && (*it)->getBounds().intersects (window.getBounds()))
        {
            const auto position = (*it)->getPosition() + juce::Point<int> (32, 32);
            if (hasReachableTitleBar (window, position))
                window.setTopLeftPosition (position);
            break;
        }
}

void PluginWindowManager::closeFor (juce::AudioPluginInstance* plugin)
{
    positions.erase (plugin);   // also forget closed editors before an instance's address can be reused
    for (auto it = windows.begin(); it != windows.end();)
    {
        if (&(*it)->getPlugin() == plugin)
            it = windows.erase (it);
        else
            ++it;
    }
}

void PluginWindowManager::closeAll()
{
    windows.clear();
    positions.clear();
}

void PluginWindowManager::pluginAboutToBeRemoved (PluginChain&, juce::AudioPluginInstance& plugin)
{
    closeFor (&plugin);
}

void PluginWindowManager::chainChanged (PluginChain& chain)
{
    if (onChainChanged)
        onChainChanged (chain);
}

void PluginWindowManager::closeWindow (PluginEditorWindow& window)
{
    for (auto it = windows.begin(); it != windows.end(); ++it)
    {
        if (it->get() == &window)
        {
            if (rememberWindowPositions)
                positions[&window.getPlugin()] = window.getPosition();
            if (onWindowClosed)
                onWindowClosed (window.getPlugin());
            windows.erase (it);
            return;
        }
    }
}

} // namespace gocue
