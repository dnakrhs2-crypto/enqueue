#pragma once

#include "LiveMixSettings.h"
#include "ControlServer.h"
#include "MixEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

namespace gocue::livemix
{

/** 설정: audio backend, input/monitor, rate/buffer (ASIO also has its driver's panel), tray behaviour, the global
    hotkeys, external control, the online backup note. Non-modal, single instance. */
namespace SettingsDialog
{
    using AcceptedFormatsQuery = std::function<std::pair<int, int>()>; // optional capability seam for non-WASAPI devices
    void show (MixEngine& engine, LiveMixSettings& settings, juce::Component* centreAround, std::function<void()> onDeviceChanged,
               std::function<void()> onHotkeysChanged, std::function<void (bool capturing)> onHotkeyCapture,
               std::function<ControlServer::Status()> controlStatus, std::function<void (bool)> controlEnabled,
               AcceptedFormatsQuery acceptedFormats = {});
    void closeIfOpen();

    /** Where the window opens and how far its inside resizes, for the screen it opens on. */
    struct Placement
    {
        juce::Rectangle<int> bounds;                    // without the native frame: no taller than the content and 640 px (70% of
                                                        // a smaller screen) - the wheel scrolls the rest - centred with its frame
        int minWidth, maxWidth, minHeight, maxHeight;   // the inside's resize limits (ClientLimits adds the frame)
    };
    Placement placement (int width, int contentHeight, juce::Point<int> centre, juce::Rectangle<int> screen, juce::BorderSize<int> frame);

    /** Resize limits for a window's inside. JUCE holds the whole window, frame included, to a constrainer's limits, so each
        check adds the frame the window has then: limits made once with the opening frame squeezed the settings under their
        width on a monitor whose scale rounds the borders wider. */
    class ClientLimits : public juce::ComponentBoundsConstrainer
    {
    public:
        ClientLimits();
        void setClientLimits (int minWidth, int maxWidth, int minHeight, int maxHeight);
        void checkBounds (juce::Rectangle<int>& bounds, const juce::Rectangle<int>& previous, const juce::Rectangle<int>& limits,
                          bool stretchingTop, bool stretchingLeft, bool stretchingBottom, bool stretchingRight) override;

        std::function<juce::BorderSize<int>()> frameNow;   // the window's native frame at this moment

    private:
        int minW = 0, maxW = 0, minH = 0, maxH = 0;
    };

    /** The Windows "start with Windows" Run entry for this exe. */
    void setStartWithWindows (bool on);
}

} // namespace gocue::livemix
