#pragma once

#include "model/Envelope.h"

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
    juce::String descriptionXml;     // juce::PluginDescription::createXml(), exact reload key
    bool bypassed = false;
};

/** Playback settings of an audio cue (QLab "Time & Loops"). */
struct AudioCueData
{
    double startSeconds = 0.0;      // trim in
    double endSeconds = -1.0;       // trim out; -1 = end of file
    int playCount = 1;              // ignored while infiniteLoop is set
    bool infiniteLoop = false;
    double rate = 1.0;              // playback speed, minRate .. maxRate
    bool preservePitch = false;     // time-stretch instead of varispeed
    Envelope envelope;              // integrated fade drawn over the waveform

    static constexpr double minRate = 0.03;
    static constexpr double maxRate = 33.0;
    static constexpr int maxPlayCount = 9999;
};

/** One audio cue. Plain data: the audio engine takes a copy when the cue is fired. */
struct Cue
{
    juce::Uuid id;                   // stable identity (survives reorder / rename)
    juce::String name;
    juce::File file;
    int fadeOutMs = 0;               // "stop fade": length of fade-out-and-stop (F); 0 = de-click only
    double gainDb = 0.0;
    double durationSeconds = 0.0;    // cached from the file header; 0 = unknown
    bool fileMissing = false;        // runtime only, not serialised
    std::vector<PluginSlotState> plugins;
    AudioCueData audio;

    static constexpr int maxFadeMs = 600000;     // 10 minutes
    static constexpr double minGainDb = -60.0;   // treated as silence
    static constexpr double maxGainDb = 12.0;

    /** Linear gain for gainDb, clamped to [minGainDb, maxGainDb]; minGainDb gives 0. */
    float gainLinear() const noexcept;

    /** A copy of this cue with a fresh id (used by "duplicate"). */
    Cue duplicated() const;

    /** Clamps fades / gain / duration / trim / loops / rate / envelope into their valid ranges. */
    void sanitise() noexcept;

    /** Trim-in, seconds into the file. */
    double regionStart() const noexcept { return audio.startSeconds; }
    /** Trim-out: the explicit end clamped to the file length when that is known, else the file length. */
    double regionEnd() const noexcept;
    /** regionEnd - regionStart, never negative (0 while the file length is unknown and no end is set). */
    double regionLength() const noexcept;
    /** Wall-clock length of one pass through the region at the cue's rate. */
    double passLength() const noexcept;
    /** Total length including loops, or -1 for an infinite loop. */
    double effectiveLength() const noexcept;
};

} // namespace gocue
