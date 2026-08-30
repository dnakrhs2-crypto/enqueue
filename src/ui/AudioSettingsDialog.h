#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

namespace gocue::AudioSettingsDialog
{

/** Opens (or brings to front) the non-modal output device selector. */
void show (juce::AudioDeviceManager& deviceManager, juce::Component* centreAround);

/** Closes the selector if it is open (call before the device manager goes away). */
void closeIfOpen();

} // namespace gocue::AudioSettingsDialog
