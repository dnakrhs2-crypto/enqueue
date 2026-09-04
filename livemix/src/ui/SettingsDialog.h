#pragma once

#include "LiveMixSettings.h"
#include "MixEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::livemix
{

/** 설정: ASIO device and buffer size (the driver's own panel), tray behaviour, start with Windows, the mute-group
    hotkeys, the online backup note. Non-modal, single instance. */
namespace SettingsDialog
{
    void show (MixEngine& engine, LiveMixSettings& settings, juce::Component* centreAround, std::function<void()> onDeviceChanged,
               std::function<void()> onHotkeysChanged, std::function<void (bool capturing)> onHotkeyCapture);
    void closeIfOpen();

    /** The Windows "start with Windows" Run entry for this exe. */
    void setStartWithWindows (bool on);
}

} // namespace gocue::livemix
