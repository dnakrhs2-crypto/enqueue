#pragma once

#include "audio/CuePlayer.h"
#include "audio/PluginChain.h"
#include "audio/PluginHost.h"
#include "model/AudioPatch.h"

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
        bool audition = false;              // started by an audition GO / preview
        double progress = 0.0;              // 0..1 through the total length (all passes); -1 while looping forever
        juce::int64 startOrder = 0;         // increases with every start (ordering the active-cues list)
    };

    struct PlayOptions
    {
        double startSeconds = 0.0;          // file seconds after the region start to begin at (pass 0)
        bool audition = false;              // mark the instance as an audition
        bool silent = false;                // audition "출력 없음": plays (timing, sequences) but reaches no output
        juce::Uuid patchOverride = juce::Uuid::null();   // audition "대체 패치": play through this patch instead of the cue own patch
    };

    /** @param readAheadSamples  disk read-ahead per cue; 0 = synchronous reads (offline tests). */
    explicit AudioEngine (int readAheadSamples = 65536);
    ~AudioEngine() override;

    /** Opens the output device (0 in / up to maxDeviceOutputs out) from a saved state or the system default.
        Returns an error message, or an empty string on success. */
    juce::String initialise (const juce::XmlElement* savedDeviceState);
    static constexpr int maxDeviceOutputs = 64;
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
    /** True when a running instance of the cue was started as an audition. */
    bool isAuditioning (const juce::Uuid& cueId) const;
    std::vector<PlayingCue> getPlayingCues() const;
    /** The running cue that was started last (null Uuid if none).
        @param ignoreFadingOut  skip cues that are already fading out. */
    juce::Uuid getMostRecentlyStartedCue (bool ignoreFadingOut) const;
    int getNumPlaying() const;

    //==========================================================================
    // Audio patches: cue outputs (level matrix columns) -> inserts -> routing -> device outputs

    /** Installs the project's patches: routing, main levels, output counts and insert chains (restored from
        their saved states where the live chain differs; errors returned). Players keep running; players of a
        removed patch move to the default (first) patch. Without patches the engine mixes straight to outputs 1-2. */
    juce::StringArray setPatches (const std::vector<AudioPatch>& patches);
    /** Live routing / main level / stereo-pair / name changes of one patch (same output count). */
    void updatePatchLevels (const AudioPatch& patch);
    /** The patch a cue plays through: its own, or the default when the id is null / unknown. Null without patches. */
    const AudioPatch* findPatchForCue (const Cue& cue) const noexcept;
    const AudioPatch* findPatch (const juce::Uuid& patchId) const noexcept;
    /** Active device output channels (2 offline / before a device opened). */
    int getNumDeviceOutputs() const noexcept { return numDeviceOutputs.load (std::memory_order_relaxed); }
    /** Insert chains of a patch, created on demand. A stereo pair's chain lives on the first output of the pair. */
    PluginChain& getPatchCueOutputChain (const juce::Uuid& patchId, int cueOutput);
    PluginChain* findPatchCueOutputChain (const juce::Uuid& patchId, int cueOutput) const;
    PluginChain& getPatchDeviceOutputChain (const juce::Uuid& patchId, int deviceOutput);
    PluginChain* findPatchDeviceOutputChain (const juce::Uuid& patchId, int deviceOutput) const;
    /** Writes the live insert chain states into the patch (for saving). */
    void capturePatchInsertStates (AudioPatch& patch) const;

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

    /** Sets the render sample rate / block size (and the device output count when >= 1) and re-prepares
        every player, patch and chain. */
    void prepare (double newSampleRate, int newBlockSize, int newNumDeviceOutputs = -1);

    /** Renders one block into 'output' (device outputs; channels beyond the prepared count are cleared).
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

    /** Runtime state of one patch. Structure changes only on the message thread under 'lock'. */
    struct PatchRuntime
    {
        AudioPatch patch;
        std::vector<float> currentRouting, targetRouting;   // [cueOutput * routingOutputs + deviceOutput]
        int routingOutputs = 0;
        juce::AudioBuffer<float> bus;          // cue outputs (K x block)
        juce::AudioBuffer<float> routed;       // device outputs after routing (M x block)
        juce::AudioBuffer<float> pairScratch;  // 2 x block: mono inserts run through the stereo chain
        std::map<int, std::unique_ptr<PluginChain>> cueOutputChains, deviceOutputChains;
    };

    PatchRuntime* findRuntime (const juce::Uuid& patchId) const noexcept;
    PatchRuntime* runtimeForCue (const Cue& cue) const noexcept;
    /** Sizes buffers / routing tables for the current block size and device outputs (allocates: call under 'lock' or before publishing). */
    void prepareRuntimeBuffers (PatchRuntime& r);
    void computeRouting (const PatchRuntime& r, std::vector<float>& out) const;
    void renderPatch (PatchRuntime& r, int numSamples) noexcept;
    static void processInsert (PluginChain& chain, juce::AudioBuffer<float>& buffer, int firstChannel, bool stereo,
                               juce::AudioBuffer<float>& scratch, int numSamples) noexcept;
    template <typename Fn> void forEachPatchChain (Fn&& fn) const;

    mutable juce::CriticalSection lock;               // guards 'players' and the patch runtimes' audio state
    std::vector<std::unique_ptr<CuePlayer>> players;
    std::vector<std::unique_ptr<PatchRuntime>> patchRuntimes;   // [0] = default patch; empty = legacy stereo mix
    std::unique_ptr<PatchRuntime> muteRuntime;                   // audition "출력 없음": a bus that is never routed
    juce::int64 startCounter = 0;
    std::atomic<int> numDeviceOutputs { 2 };

    PluginChain masterChain;
    std::map<juce::String, std::unique_ptr<PluginChain>> cueChains;   // keyed by Uuid string
    PluginChain::Listener* chainListener = nullptr;

    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<int> blockSize { 512 };
    juce::AudioBuffer<float> mixBuffer, playerBuffer;  // audio-thread scratch

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace gocue
