#pragma once
#include "model/RecorderModel.h"

namespace gocue::recorder::hardening
{
juce::var scale(const juce::File& directory, std::size_t clipCount);
juce::var largeFiles(const juce::File& directory);
juce::var editProperty(std::uint64_t seed, int iterations);
juce::var cancelledSeeks(unsigned iterations);
juce::var cancelledExports(const juce::File& directory, unsigned iterations);
juce::var dubbingEpoch(const juce::File& directory);
}
