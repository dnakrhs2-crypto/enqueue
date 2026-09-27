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

    /** The Windows "start with Windows" Run entry for this exe. */
    void setStartWithWindows (bool on);
}

} // namespace gocue::livemix
