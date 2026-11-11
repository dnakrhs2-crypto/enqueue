#pragma once
#include <juce_core/juce_core.h>
#include <optional>
#include <cstdint>

namespace gocue::livemix
{
std::optional<std::uint32_t> restartAfterPid (const juce::String& commandLine);
void waitForRestartParent (const juce::String& commandLine);
bool launchRestart();
}
