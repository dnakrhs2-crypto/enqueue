#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace gocue
{

/** Serialised state of one plugin insert (VST3). Filled in by the plugin host. */
struct PluginSlotState
{
    juce::String format { "VST3" };
    juce::String name;
    juce::String fileOrIdentifier;   // juce::PluginDescription::fileOrIdentifier
    int uniqueId = 0;                // juce::PluginDescription::uniqueId
    juce::String stateBase64;        // AudioProcessor::getStateInformation(), base64
    bool bypassed = false;
};

/** One audio cue. Plain data: the audio engine takes a copy when the cue is fired. */
struct Cue
{
    juce::Uuid id;                   // stable identity (survives reorder / rename)
    juce::String name;
    juce::File file;
    int fadeInMs = 0;
    int fadeOutMs = 0;
    double gainDb = 0.0;
    double durationSeconds = 0.0;    // cached from the file header; 0 = unknown
    bool fileMissing = false;        // runtime only, not serialised
    std::vector<PluginSlotState> plugins;

    static constexpr int maxFadeMs = 600000;     // 10 minutes
    static constexpr double minGainDb = -60.0;   // treated as silence
    static constexpr double maxGainDb = 12.0;

    /** Linear gain for gainDb, clamped to [minGainDb, maxGainDb]; minGainDb gives 0. */
    float gainLinear() const noexcept;

    /** A copy of this cue with a fresh id (used by "duplicate"). */
    Cue duplicated() const;

    /** Clamps fade / gain / duration into their valid ranges. */
    void sanitise() noexcept;
};

} // namespace gocue
