#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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
    void toggle();
    bool isActive() const noexcept { return active; }
    void refit();
    juce::String restorableState();

private:
    void releaseKiosk();
    void markFullscreen (bool fullscreen);

    juce::ResizableWindow& window;
    juce::String saved;
    bool active = false, changing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FullScreenMode)
};

} // namespace gocue
