#pragma once

#include "app/Commands.h"
#include "ui/CueIcons.h"

namespace gocue::CueMenuIcons
{

/** Menu-only adapter: the reusable icon registry itself knows no command IDs. */
inline std::unique_ptr<juce::Drawable> create (juce::CommandID command)
{
    using namespace CommandIDs;
    using CueIcons::Key;
    switch (command)
    {
        case addCue:        return CueIcons::create (Key { CueType::audio });
        case addFadeCue:    return CueIcons::create (Key { CueType::fade, FadeMode::fadeIn });
        case addFadeOutCue: return CueIcons::create (Key { CueType::fade, FadeMode::fadeOut });
        case addDevampCue:  return CueIcons::create (Key { CueType::devamp });
        case addGroupCue:   return CueIcons::create (Key { CueType::group });
        case addControlCue: return CueIcons::create (Key { CueType::control });
        case addWaitCue:    return CueIcons::create (Key { CueType::control, FadeMode::custom, ControlKind::wait });
        case addMemoCue:    return CueIcons::create (Key { CueType::control, FadeMode::custom, ControlKind::memo });
        case addMicCue:     return CueIcons::create (Key { CueType::mic });
        case addCueList:    return CueIcons::create (CueIcons::Container::cueList);
        case addCart:       return CueIcons::create (CueIcons::Container::cart);
        default:           return nullptr;
    }
}

} // namespace gocue::CueMenuIcons
