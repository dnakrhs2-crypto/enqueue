#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace gocue::livemix::AudioBackends
{
juce::StringArray availableTypes (juce::AudioDeviceManager& manager);
juce::String label (const juce::String& typeName);
/** Message thread only. Unknown names/properties conservatively select the split monitor. */
bool sameContainer (const juce::String& inputName, const juce::String& outputName);
bool isWindows (const juce::String& typeName) noexcept;
}
