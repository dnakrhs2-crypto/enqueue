#pragma once

#include "audio/FadeEnvelope.h"
#include "audio/PluginChain.h"
#include "audio/RegionLoopSource.h"
#include "model/Cue.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

namespace gocue
{

/** One playing instance of a cue:
    file region / loops / integrated envelope (RegionLoopSource, file time) -> disk read-ahead ->
    resampling to the device rate at the cue's playback rate -> stop-fade / pause gates -> cue gain ->
    duck / boost -> cue plugin chain.

    Created, prepared and started on the message thread; renderNextBlock() runs on the audio thread.
    Control requests (stop / fade-out / pause / rate / gain / duck / trim) are passed through atomics.
    A "loaded" player sits in the engine rendering silence until start() (QLab load).
    After the last pass ends (or a fade-out completes) the chain keeps running for its reported
    tail so reverbs and delays are not cut off; a hard stop skips the tail. */
class CuePlayer
{
public:
    /** Every stop / pause is de-clicked with a ramp of at least this length. */
    static constexpr int stopDeclickMs = 5;

    /** Opens the cue's file; check isValid() afterwards.
        @param readAheadThread    thread used for disk read-ahead; ignored when readAheadSamples == 0
                                  (synchronous reads, used by the offline tests).
        @param startOffsetSeconds where to begin, in file seconds after the region start (pass 0). */
    CuePlayer (const Cue& cue, juce::AudioFormatManager& formats,
               juce::TimeSliceThread* readAheadThread, int readAheadSamples,
               double startOffsetSeconds = 0.0);
    ~CuePlayer();

    bool isValid() const noexcept                        { return resampler != nullptr; }
    const juce::String& getErrorMessage() const noexcept { return errorMessage; }

    /** Must be called before the first render and again whenever the device settings change.
        Not audio-thread safe. */
    void prepare (double sampleRate, int blockSize);

    /** Arms playback (from the start offset given to the constructor).
        Call after prepare(); may also be called later on a loaded player to start it. */
    void start();
    /** Marks the player as "loaded": it sits in the engine rendering silence until start(). */
    void armLoaded() noexcept { loadedNotStarted.store (true, std::memory_order_relaxed); }
    bool isLoadedNotStarted() const noexcept { return loadedNotStarted.load (std::memory_order_relaxed); }

    /** The insert chain this player runs through (may be null). Any thread. */
    void setChain (PluginChain* newChain) noexcept   { chain.store (newChain, std::memory_order_release); }
    PluginChain* getChain() const noexcept           { return chain.load (std::memory_order_acquire); }

    /** De-clicked immediate stop, no plugin tail. Any thread. */
    void requestStop() noexcept;
    /** Fade to silence over 'milliseconds', then stop (plugin tail still rings out). Any thread. */
    void requestFadeOut (int milliseconds) noexcept;
    /** De-clicked pause: the position freezes; plugins keep running on silence. Any thread. */
    void requestPause() noexcept;
    void requestResume() noexcept;
    /** Finish the pass that is audible now, then end (devamp of a looping cue). Message thread. */
    void requestFinishCurrentPass() noexcept;

    /** Live trim from the inspector. The audible file position is kept (the pass / offset are
        re-derived from the new region); a new end before that position ends the cue at once (with the
        plugin tail). The read-ahead is flushed so the new region is heard after a few milliseconds.
        Message thread. */
    void setLiveRegion (double startSeconds, double endSeconds) noexcept;
    /** Live playback rate (varispeed). Any thread. */
    void setLiveRate (double rate) noexcept;
    /** Live cue gain (dB); ramps over one block so the change is click-free. Any thread. */
    void setLiveGainDb (double gainDb) noexcept;
    /** Duck / boost applied on top of the cue gain, reached over 'rampSeconds'. 0 dB = none. Any thread. */
    void setDuckDb (double duckDb, double rampSeconds) noexcept;
    double getDuckDb() const noexcept { return duckDb.load (std::memory_order_relaxed); }

    /** Audio thread. Overwrites channels 0-1 of buffer[0, numSamples) with this player's output.
        Returns false once the player has finished; the block still contains its final audio. */
    bool renderNextBlock (juce::AudioBuffer<float>& buffer, int numSamples);

    const juce::Uuid& getCueId() const noexcept   { return cue.id; }
    const Cue& getCue() const noexcept            { return cue; }
    bool hasFinished() const noexcept             { return finished.load (std::memory_order_relaxed); }
    bool isFadingOut() const noexcept             { return fadingOut.load (std::memory_order_relaxed); }
    bool isPaused() const noexcept                { return pausedFlag.load (std::memory_order_relaxed); }
    /** Elapsed wall-clock seconds since the start (paused time excluded) plus the start offset. */
    double getPositionSeconds() const noexcept    { return positionSeconds.load (std::memory_order_relaxed); }
    /** Total wall-clock length of the cue at the current rate and trim; -1 while looping forever. */
    double getLengthSeconds() const noexcept;
    /** Wall-clock seconds left at the current rate; -1 while looping forever. */
    double getRemainingSeconds() const noexcept;
    /** Absolute position inside the file (for the waveform playhead). */
    double getFilePositionSeconds() const noexcept { return filePositionSeconds.load (std::memory_order_relaxed); }
    int getPassIndex() const noexcept             { return passIndex.load (std::memory_order_relaxed); }

    juce::int64 getStartOrder() const noexcept    { return startOrder; }
    void setStartOrder (juce::int64 order) noexcept { startOrder = order; }

private:
    void updatePositionInfo (double rate) noexcept;
    double ratioFor (double rate) const noexcept { return fileSampleRate * rate / currentSampleRate; }

    Cue cue;
    juce::String errorMessage;
    std::unique_ptr<RegionLoopSource> source;
    std::unique_ptr<juce::BufferingAudioSource> readAhead;     // null on the synchronous (test) path
    std::unique_ptr<juce::ResamplingAudioSource> resampler;
    FadeEnvelope envelope;        // stop fades and de-clicks
    FadeEnvelope pauseGate;       // pause / resume ramps
    float gainLinear = 1.0f;
    float duckLevel = 1.0f;       // audio thread
    double fileSampleRate = 44100.0;
    double currentSampleRate = 44100.0;
    double startOffsetSeconds = 0.0;
    juce::int64 startOffsetSamples = 0;
    juce::int64 startOrder = 0;

    std::atomic<PluginChain*> chain { nullptr };
    std::atomic<int> pendingFadeOutMs { -1 };
    std::atomic<bool> hardStopRequested { false };
    std::atomic<bool> stopRequested { false };
    std::atomic<bool> finished { false };
    std::atomic<bool> fadingOut { false };
    std::atomic<bool> pauseRequested { false };
    std::atomic<bool> resumeRequested { false };
    std::atomic<bool> pausedFlag { false };
    std::atomic<bool> loadedNotStarted { false };
    std::atomic<double> liveRate { 1.0 };
    std::atomic<float> targetGain { 1.0f };
    std::atomic<float> duckTarget { 1.0f };
    std::atomic<double> duckRampSeconds { 0.0 };
    std::atomic<double> duckDb { 0.0 };
    std::atomic<double> positionSeconds { 0.0 };
    std::atomic<double> filePositionSeconds { 0.0 };
    std::atomic<int> passIndex { 0 };
    std::atomic<double> virtualPosition { 0.0 };        // file samples on the source's virtual timeline (audible); written by the audio thread
    std::atomic<juce::int64> pendingVirtualPosition { -1 };   // set by setLiveRegion, adopted by the audio thread

    // audio-thread state
    double elapsedOutputSamples = 0.0;
    double lastRatio = 0.0;
    bool pausing = false;
    bool paused = false;
    bool inTail = false;
    juce::int64 tailSamplesLeft = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CuePlayer)
};

} // namespace gocue
