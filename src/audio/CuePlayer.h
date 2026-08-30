#pragma once

#include "audio/FadeEnvelope.h"
#include "audio/PluginChain.h"
#include "model/Cue.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

namespace gocue
{

/** One playing instance of a cue:
    file streaming -> resampling to the device rate -> fade envelope -> cue gain -> cue plugin chain.

    Created, prepared and started on the message thread; renderNextBlock() runs on the
    audio thread. Control requests (stop / fade-out) are passed through atomics.
    After the file ends (or a fade-out completes) the chain keeps running for its reported
    tail so reverbs and delays are not cut off; a hard stop skips the tail. */
class CuePlayer
{
public:
    /** Every stop is de-clicked with a ramp of at least this length. */
    static constexpr int stopDeclickMs = 5;

    /** Opens the cue's file; check isValid() afterwards.
        @param readAheadThread   thread used for disk read-ahead; ignored when readAheadSamples == 0
                                 (synchronous reads, used by the offline tests). */
    CuePlayer (const Cue& cue, juce::AudioFormatManager& formats,
               juce::TimeSliceThread* readAheadThread, int readAheadSamples);
    ~CuePlayer();

    bool isValid() const noexcept                      { return readerSource != nullptr; }
    const juce::String& getErrorMessage() const noexcept { return errorMessage; }

    /** Must be called before the first render and again whenever the device settings change.
        Not audio-thread safe. */
    void prepare (double sampleRate, int blockSize);

    /** Arms playback from the beginning with the cue's fade-in.
        Call after prepare() and before the player is visible to the audio thread. */
    void start();

    /** The insert chain this player runs through (may be null). Any thread. */
    void setChain (PluginChain* newChain) noexcept   { chain.store (newChain, std::memory_order_release); }
    PluginChain* getChain() const noexcept           { return chain.load (std::memory_order_acquire); }

    /** De-clicked immediate stop, no plugin tail. Any thread. */
    void requestStop() noexcept;

    /** Fade to silence over 'milliseconds', then stop (plugin tail still rings out). Any thread. */
    void requestFadeOut (int milliseconds) noexcept;

    /** Audio thread. Overwrites channels 0-1 of buffer[0, numSamples) with this player's output.
        Returns false once the player has finished; the block still contains its final audio. */
    bool renderNextBlock (juce::AudioBuffer<float>& buffer, int numSamples);

    const juce::Uuid& getCueId() const noexcept   { return cue.id; }
    const Cue& getCue() const noexcept            { return cue; }
    bool hasFinished() const noexcept             { return finished.load (std::memory_order_relaxed); }
    bool isFadingOut() const noexcept             { return fadingOut.load (std::memory_order_relaxed); }
    double getPositionSeconds() const noexcept    { return positionSeconds.load (std::memory_order_relaxed); }
    double getLengthSeconds() const noexcept      { return lengthSeconds; }

    juce::int64 getStartOrder() const noexcept    { return startOrder; }
    void setStartOrder (juce::int64 order) noexcept { startOrder = order; }

private:
    Cue cue;
    juce::String errorMessage;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transport;
    FadeEnvelope envelope;
    float gainLinear = 1.0f;
    double lengthSeconds = 0.0;
    double currentSampleRate = 44100.0;
    juce::int64 startOrder = 0;

    std::atomic<PluginChain*> chain { nullptr };
    std::atomic<int> pendingFadeOutMs { -1 };
    std::atomic<bool> hardStopRequested { false };
    std::atomic<bool> stopRequested { false };
    std::atomic<bool> finished { false };
    std::atomic<bool> fadingOut { false };
    std::atomic<double> positionSeconds { 0.0 };

    // audio-thread state
    bool inTail = false;
    juce::int64 tailSamplesLeft = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CuePlayer)
};

} // namespace gocue
