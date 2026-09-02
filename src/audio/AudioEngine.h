#pragma once

#include "audio/CuePlayer.h"
#include "audio/PluginChain.h"
#include "audio/PluginHost.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace gocue
{

/** Owns the audio output device, streams cue files, runs each cue through its own plugin
    chain, mixes everything, runs the master chain and writes the first two output channels.
    Message-thread API unless stated otherwise. */
class AudioEngine : private juce::AudioIODeviceCallback,
                    private juce::AsyncUpdater
{
public:
    struct PlayingCue
    {
        juce::Uuid id;
        double positionSeconds = 0.0;       // elapsed wall-clock seconds into the cue (all passes, paused time excluded)
        double lengthSeconds = 0.0;         // total wall-clock length at the current rate / trim; -1 = looping forever
        double remainingSeconds = 0.0;      // wall-clock seconds left at the current rate; -1 = looping forever
        double filePositionSeconds = 0.0;   // audible position inside the file (waveform playhead)
        int passIndex = 0;                  // 0-based loop pass
        bool fadingOut = false;
        bool paused = false;
        bool loaded = false;                // prepared but not started yet (QLab "loaded")
        juce::int64 startOrder = 0;         // increases with every start (ordering the active-cues list)
    };

    struct PlayOptions
    {
        double startSeconds = 0.0;          // file seconds after the region start to begin at (pass 0)
    };

    /** @param readAheadSamples  disk read-ahead per cue; 0 = synchronous reads (offline tests). */
    explicit AudioEngine (int readAheadSamples = 65536);
    ~AudioEngine() override;

    /** Opens the output device (0 in / 2 out) from a saved state or the system default.
        Returns an error message, or an empty string on success. */
    juce::String initialise (const juce::XmlElement* savedDeviceState);
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() noexcept { return formatManager; }
    PluginHost& getPluginHost() noexcept { return pluginHost; }

    //==========================================================================
    // Transport

    /** Fires a cue. If the same cue is already running it is restarted from the top.
        Returns false (with a message) when the file cannot be opened. */
    bool play (const Cue& cue, juce::String* errorMessage = nullptr) { return play (cue, PlayOptions(), errorMessage); }
    bool play (const Cue& cue, const PlayOptions& options, juce::String* errorMessage = nullptr);
    /** Prepares a player at 'startSeconds' without starting it; the next play() of this cue starts it at once.
        Replaces an earlier loaded instance. */
    bool load (const Cue& cue, double startSeconds = 0.0, juce::String* errorMessage = nullptr);
    bool isLoaded (const juce::Uuid& cueId) const;
    /** Drops loaded (not started) instances of the cue, or of every cue when null. */
    void unload (const juce::Uuid& cueId = juce::Uuid::null());
    /** De-clicked immediate stop of every instance of this cue (no plugin tail). */
    void stop (const juce::Uuid& cueId);
    void stopAll();
    /** Fades the cue out over its own fadeOutMs, then stops it. */
    void fadeOutAndStop (const juce::Uuid& cueId);
    void fadeOutAndStopAll();
    /** Fades the cue out over 'milliseconds', then stops it (panic). */
    void fadeOutAndStop (const juce::Uuid& cueId, int milliseconds);
    void fadeOutAndStopAll (int milliseconds);
    /** De-clicked pause / resume. */
    void pause (const juce::Uuid& cueId);
    void resume (const juce::Uuid& cueId);
    void pauseAll();
    void resumeAll();
    bool isPaused (const juce::Uuid& cueId) const;
    /** Lets a looping cue finish the pass that is audible now, then end (devamp). */
    void finishCurrentPass (const juce::Uuid& cueId);
    /** Live trim / rate for a running cue (the inspector while it plays). */
    void setLiveRegion (const juce::Uuid& cueId, double startSeconds, double endSeconds);
    void setLiveRate (const juce::Uuid& cueId, double rate);
    void setLiveGainDb (const juce::Uuid& cueId, double gainDb);
    /** Live level matrix / trim for a running cue (ramped over ~10 ms). */
    void setLiveLevels (const juce::Uuid& cueId, const LevelMatrix& levels, const TrimLevels& trim);
    /** Scrub: jump to a fraction (0..1) of the cue's total length. */
    void seekToFraction (const juce::Uuid& cueId, double fraction);
    /** Duck / boost (dB on top of the cue gain) reached over 'rampSeconds'; 0 = none. */
    void setDuckDb (const juce::Uuid& cueId, double duckDb, double rampSeconds);
    double getDuckDb (const juce::Uuid& cueId) const;
    /** Ids of the cues that are paused right now. */
    std::vector<juce::Uuid> getPausedCues() const;

    bool isPlaying (const juce::Uuid& cueId) const;
    std::vector<PlayingCue> getPlayingCues() const;
    /** The running cue that was started last (null Uuid if none).
        @param ignoreFadingOut  skip cues that are already fading out. */
    juce::Uuid getMostRecentlyStartedCue (bool ignoreFadingOut) const;
    int getNumPlaying() const;

    //==========================================================================
    // Plugin chains (all owned by the engine so they outlive the players that use them)

    PluginChain& getMasterChain() noexcept { return masterChain; }
    /** The cue's insert chain, created on demand. */
    PluginChain& getCueChain (const juce::Uuid& cueId);
    PluginChain* findCueChain (const juce::Uuid& cueId) const;
    /** Ids of every cue that currently owns a chain (including empty ones). */
    std::vector<juce::Uuid> getCueChainIds() const;
    /** Detaches the chain from any running player, then destroys it. */
    void removeCueChain (const juce::Uuid& cueId);
    void clearCueChains();
    /** Listener applied to the master chain and every cue chain (editor windows, dirty tracking). */
    void setChainListener (PluginChain::Listener* listener);
    /** Factory bound to the current sample rate / block size, for PluginChain::restore(). */
    PluginChain::Factory makePluginFactory();
    /** True when any chain's plugin reported a parameter / state change since the previous call. */
    bool consumePluginStateChanges();

    double getSampleRate() const noexcept { return sampleRate.load (std::memory_order_relaxed); }
    int getBlockSize() const noexcept     { return blockSize.load (std::memory_order_relaxed); }

    //==========================================================================
    // Normally driven by the device; public so the engine can be exercised offline.

    /** Sets the render sample rate / block size and re-prepares every player and chain. */
    void prepare (double newSampleRate, int newBlockSize);

    /** Renders one block into 'output' (>= 2 channels; extra channels are cleared).
        Audio thread, or the test harness. */
    void renderBlock (juce::AudioBuffer<float>& output, int numSamples);

    /** Destroys players that have finished. Called automatically on the message thread. */
    void reapFinishedPlayers();

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;
    void handleAsyncUpdate() override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    PluginHost pluginHost;
    juce::TimeSliceThread readAheadThread { "GoCue disk read-ahead" };
    const int readAheadSamples;
    bool callbackAdded = false;

    mutable juce::CriticalSection lock;               // guards 'players'
    std::vector<std::unique_ptr<CuePlayer>> players;
    juce::int64 startCounter = 0;

    PluginChain masterChain;
    std::map<juce::String, std::unique_ptr<PluginChain>> cueChains;   // keyed by Uuid string
    PluginChain::Listener* chainListener = nullptr;

    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<int> blockSize { 512 };
    juce::AudioBuffer<float> mixBuffer, playerBuffer;  // audio-thread scratch

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace gocue
