#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace gocue
{

/** Monitor-sized kiosk mode, separate from JUCE's maximised window state.
    The owner forwards resized/moved/parentSizeChanged to refit(). No activation
    is performed here; only the main window should own an instance in the app. */
class FullScreenMode
{
public:
    explicit FullScreenMode (juce::ResizableWindow& target) : window (target) {}
    ~FullScreenMode();

    void enter();
    void exit();
    void shutdown(); // hide and release, without restoring or maximising
    void toggle();
    bool isActive() const noexcept { return active; }
    void refit();
    juce::String restorableState();

    // Must match tools/juce-patches/0003-kiosk-mode-survives-app-switch.patch.
    inline static const juce::Identifier keepKioskModeWhenAppInactive { "keepKioskModeWhenAppInactive" };
    std::function<void()> onExternalExit;

private:
    void releaseKiosk();
    void markFullscreen (bool fullscreen);
    void reconcileExternalExit();
    void fitToMonitor (juce::Rectangle<int> initialMonitor = {});
    bool isRestorePending() const { return pendingRestore != nullptr && *pendingRestore; }

    juce::ResizableWindow& window;
    juce::String saved;
    // Weakly captured by the queued restore; shutdown/destruction cancels it.
    std::shared_ptr<bool> pendingRestore;
    bool active = false, changing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FullScreenMode)
};

} // namespace gocue
