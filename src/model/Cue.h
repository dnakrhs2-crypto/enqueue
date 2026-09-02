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

/** What a running cue does when it is triggered again (QLab "second trigger"). */
enum class SecondTriggerAction
{
    nothing,          // ignore
    panic,            // fade out over the workspace panic time, then stop
    stop,             // fade out over the cue's stop fade, then stop
    hardStop,         // stop at once (de-clicked)
    hardStopRestart,  // stop and start again from the top
    devamp            // finish the current loop pass, then end
};

/** What happens after a cue is fired (QLab "continue mode"). */
enum class ContinueMode
{
    none,           // wait for the next GO
    autoContinue,   // start the next cue postWaitSeconds after this one starts
    autoFollow      // start the next cue when this one finishes
};

enum class FadeStopScope { peers, list, all };

/** Time-of-day trigger. daysMask bit 0 = Sunday ... bit 6 = Saturday. */
struct WallClockTrigger
{
    bool enabled = false;
    int hour = 0, minute = 0, second = 0;
    int daysMask = 0x7f;
};

/** "Fade & stop others when this cue starts". */
struct FadeStopOthers
{
    bool enabled = false;
    double seconds = 2.0;
    FadeStopScope scope = FadeStopScope::list;
};

/** "Duck / boost the other cues in this list while this cue runs". */
struct DuckSettings
{
    bool enabled = false;
    double levelDb = -12.0;   // negative = duck, positive = boost
    double seconds = 1.0;
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
    juce::String number;             // free text, unique in the project when set ("" = none)
    juce::String name;
    juce::String notes;
    int color = 0;                   // CueColors index, 0 = none
    int secondColor = 0;             // shown after the cue has played once (when useSecondColor)
    bool useSecondColor = false;
    bool flagged = false;
    bool armed = true;
    bool skipIfDisarmed = false;     // a disarmed cue is skipped entirely by GO instead of just staying silent
    bool autoLoad = false;
    double preWaitSeconds = 0.0;
    double postWaitSeconds = 0.0;
    ContinueMode continueMode = ContinueMode::none;
    juce::String hotkey;             // juce::KeyPress description, "" = none
    WallClockTrigger wallClock;
    FadeStopOthers fadeStopOthers;
    DuckSettings duck;
    juce::File file;
    int fadeOutMs = 0;               // "stop fade": length of fade-out-and-stop (F); 0 = de-click only
    double gainDb = 0.0;
    double durationSeconds = 0.0;    // cached from the file header; 0 = unknown
    bool fileMissing = false;        // runtime only, not serialised
    std::vector<PluginSlotState> plugins;
    AudioCueData audio;
    SecondTriggerAction secondTrigger = SecondTriggerAction::hardStopRestart;

    static constexpr int maxFadeMs = 600000;     // 10 minutes
    static constexpr double minGainDb = -60.0;   // treated as silence
    static constexpr double maxGainDb = 12.0;
    static constexpr double maxWaitSeconds = 86400.0;

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
