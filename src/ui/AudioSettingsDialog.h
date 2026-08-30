#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

namespace gocue::AudioSettingsDialog
{

/** Opens (or brings to front) the non-modal output device selector. */
void show (juce::AudioDeviceManager& deviceManager, juce::Component* centreAround);

} // namespace gocue::AudioSettingsDialog
