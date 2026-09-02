#pragma once

#include "model/Cue.h"
#include "model/LevelMatrix.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace gocue
{

/** QLab-style audio patch: K cue outputs (the columns of every cue's level matrix) routed to the
    device's M outputs. Cue-output inserts (per output, or per stereo pair) run before the routing,
    device-output inserts after it; the legacy master chain stays on device outputs 1-2.
    Routing rows are sized by numCueOutputs; columns grow lazily to the device (missing = default diagonal). */
struct AudioPatch
{
    static constexpr int maxCueOutputs = LevelMatrix::maxOutputs;   // 128
    static constexpr int defaultCueOutputs = 16;

    juce::Uuid id;
    juce::String name;
    int numCueOutputs = defaultCueOutputs;
    juce::StringArray cueOutputNames;                          // "" = default "출력 n"
    std::vector<std::vector<double>> routingDb;                // [cueOutput][deviceOutput], dB or silentDb
    double mainDb = 0.0;                                       // patch main level (after the routing)
    std::vector<std::vector<PluginSlotState>> cueOutputInserts;     // [cueOutput]; a stereo pair's chain lives on the first of the pair
    std::vector<std::vector<PluginSlotState>> deviceOutputInserts;  // [deviceOutput]
    std::vector<char> cueOutputStereoWithNext;                 // [cueOutput]: this output and the next form a stereo pair for inserts

    static AudioPatch makeDefault (const juce::String& name = {});

    /** Default routing: cue output n -> device output n at 0 dB. */
    static double defaultRoutingDb (int cueOutput, int deviceOutput) noexcept;
    /** Routing level, with the default for columns that were never stored. */
    double routing (int cueOutput, int deviceOutput) const noexcept;
    void setRouting (int cueOutput, int deviceOutput, double db);
    /** Linear gain cue output -> device output including the patch main; 0 when silent / out of range. */
    float routingGain (int cueOutput, int deviceOutput) const noexcept;
    /** Makes the routing table at least numDeviceOutputs wide (default values for new columns). */
    void ensureDeviceOutputs (int numDeviceOutputs);
    int numStoredDeviceOutputs() const noexcept;

    juce::String cueOutputName (int cueOutput) const;
    /** True when 'cueOutput' is the second half of a stereo pair (its inserts live on the previous output). */
    bool isSecondOfPair (int cueOutput) const noexcept;
    bool isFirstOfPair (int cueOutput) const noexcept;

    /** Clamps the output count, resizes every per-output table to it, clamps levels. */
    void sanitise();
    bool operator== (const AudioPatch& other) const noexcept;
    bool operator!= (const AudioPatch& other) const noexcept { return ! (*this == other); }

    juce::var toVar() const;
    static AudioPatch fromVar (const juce::var& v);
};

} // namespace gocue
