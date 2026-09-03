#pragma once

#include "LiveMixSettings.h"
#include "MixDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::livemix
{

/** 온라인 백업: asks for the creator name (and the password when it is not stored), then uploads the saved session
    to <folder>/<PC>/<creator>_<date time>.livemix. */
namespace BackupDialog
{
    void show (MixDocument& document, LiveMixSettings& settings, juce::Component* centreAround,
               std::function<void (const juce::String& message, bool error)> status);
}

} // namespace gocue::livemix
