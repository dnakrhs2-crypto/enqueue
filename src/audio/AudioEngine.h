#pragma once

#include "audio/CuePlayer.h"
#include "audio/PluginChain.h"
#include "audio/PluginHost.h"
#include "model/AudioPatch.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <array>
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
        bool explicitStart = false;         // true: startSeconds was chosen by the user (a waveform click) and wins over a loaded position
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
    static constexpr int maxPlayers = 256;   // simultaneous instances; play() refuses beyond it (no growth under the audio lock)
    /** Outputs beyond a stereo pair are for ASIO only (gom, 2026-09-03): the Windows Audio modes stay at 1-2. */
    static constexpr int stereoOnlyOutputs = 2;
    /** True for a device type that may open more than two outputs (ASIO). */
    static bool typeAllowsMultichannel (const juce::String& deviceTypeName) { return deviceTypeName.containsIgnoreCase ("ASIO"); }
    /** True when the current device type may open more than two outputs (ASIO). */
    bool currentTypeAllowsMultichannel() const;
    /** How many outputs the current device type may open: maxDeviceOutputs on ASIO, 2 elsewhere. */
    int outputLimitForCurrentType() const { return currentTypeAllowsMultichannel() ? maxDeviceOutputs : stereoOnlyOutputs; }
    /** Applies the policy to the open device: a non-ASIO device is trimmed to outputs 1-2; an ASIO device whose channels
        were never chosen is widened back to everything it has (JUCE keeps the last explicit count as its default, so
        after a trim elsewhere it would open ASIO with 1-2 only). Returns the device error when the reopen failed (the
        device is closed then). Message thread. */
    juce::String enforceOutputLimit();
    /** The saved device state with the policy applied before the first open: a non-ASIO type asks for 1-2 straight
        away instead of opening wide and being trimmed (an exclusive-mode device may refuse the wide request). A state
        without a type is judged by the type that lists its output device. */
    std::unique_ptr<juce::XmlElement> normaliseDeviceState (const juce::XmlElement* saved);
    /** What the running device may carry: channels at or beyond this index are silenced in the callback. */
    int getOutputChannelLimit() const noexcept { return outputChannelLimit.load (std::memory_order_relaxed); }
    static constexpr int maxDeviceInputs = 32;
    /** Mic cues need device inputs: opens the first 'channels' input channels (the mic cue rows are the *first*
        open inputs, so inputs 1..N must be the ones open); 0 leaves the device as is. Restarts the device when it
        has to open more, so call it while nothing plays if possible. Returns an error message when the device
        could not be reconfigured (the previous setup is restored). */
    juce::String setInputsWanted (int channels);
    /** The engine noticed finished players: called from the UI timer (never from the audio thread). */
    void reapIfNeeded();
    /** Input channels the open device delivers (0 offline). */
    int getNumDeviceInputs() const noexcept { return numDeviceInputs.load (std::memory_order_relaxed); }
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
    /** The final output gate: closes over 'rampMilliseconds' and clears the plugin chains once silent; play()/resume() reopen it. */
    void closeOutputGate (int rampMilliseconds, bool resetChains);
    void openOutputGate();
    bool isDeviceRunning() const noexcept { return deviceRunning.load (std::memory_order_relaxed); }
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
    /** Lets a looping cue finish the pass that is audible now, then go on (or stop when 'stopAfter'). */
    /** Devamp on the newest running instance: ends its current loop pass. Returns the seconds until that
        boundary at the current rate, or -1 when the cue is not running or has no loop pass to finish. */
    double finishCurrentPass (const juce::Uuid& cueId, bool stopAfter = false);
    /** Virtual position (file samples on the newest running instance's timeline); -1 when not running. */
    juce::int64 getVirtualPosition (const juce::Uuid& cueId) const;
    /** Live rate / file sample rate of the newest running instance (1.0 / 44100 when not running). */
    double getLiveRate (const juce::Uuid& cueId) const;
    double getFileSampleRate (const juce::Uuid& cueId) const;
    /** Seconds (at the current rate) until the running cue reaches the end of its current pass; -1 when none. */
    double getSecondsToPassEnd (const juce::Uuid& cueId) const;
    /** Live trim / rate for a running cue (the inspector while it plays). */
    void setLiveRegion (const juce::Uuid& cueId, double startSeconds, double endSeconds);
    /** Live fade-envelope edit for the running instances of a cue. */
    void setLiveEnvelope (const juce::Uuid& cueId, const Envelope& envelope);
    void setLiveRate (const juce::Uuid& cueId, double rate);
    /** Play count / infinite loop of every running instance of the cue, applied at once (message thread). */
    void setLivePlayCount (const juce::Uuid& cueId, int playCount, bool infiniteLoop);
    /** Live slice markers for a running cue. */
    void setLiveSlices (const juce::Uuid& cueId, const std::vector<Slice>& slices, int firstSliceCount);
    void setLiveGainDb (const juce::Uuid& cueId, double gainDb);
    /** Live level matrix / trim for a running cue (ramped over ~10 ms). */
    void setLiveLevels (const juce::Uuid& cueId, const LevelMatrix& levels, const TrimLevels& trim);

    struct LiveState
    {
        double gainDb = 0.0;
        LevelMatrix levels;
        TrimLevels trim;
        double rate = 1.0;
    };
    /** Current live values of the most recently started running instance of the cue (fade cues start from here). */
    bool getLiveState (const juce::Uuid& cueId, LiveState& out) const;
    /** Start order of the most recently started running (not loaded, not finished) instance; -1 when none.
        A fade / devamp remembers it so a restart of the same cue is a different instance to them. */
    juce::int64 getStartOrder (const juce::Uuid& cueId) const;
    /** Scrub: jump to a fraction (0..1) of the cue's total length. */
    void seekToFraction (const juce::Uuid& cueId, double fraction);
    /** Jumps the running instances of a cue to a file position (seconds from the file start), inside their current pass. */
    void seekToFileSeconds (const juce::Uuid& cueId, double fileSeconds);
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
    /** Installs the patch list. Plugin instances of patches that keep their structure survive; with
        'applySavedStates' (project open) their saved parameter states are pushed in, otherwise the live state stays. */
    juce::StringArray setPatches (const std::vector<AudioPatch>& patches, bool applySavedStates = false);
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
        'inputs' (device input channels, may be null) feed the mic cues. Audio thread, or the test harness. */
    void renderBlock (juce::AudioBuffer<float>& output, int numSamples, const float* const* inputs = nullptr, int numInputs = 0);

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
    juce::TimeSliceThread readAheadThread { "Enqueue disk read-ahead" };
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
    // --- the final output gate: whatever the players and plugin chains do, a closed gate means silence at the device
    std::atomic<bool> hardPanicRequested { false };    // set before the lock is taken: the callback mutes at once, even while the lock is busy
    std::atomic<int> outputGateTarget { 1 };            // 1 open, 0 closed
    std::atomic<int> outputGateRampSamples { 220 };     // ramp length of the next move (5 ms at 44.1 k by default)
    std::atomic<juce::int64> outputGateCloseCountdown { -1 };   // samples until a scheduled close (soft panic), -1 = none
    std::atomic<bool> resetChainsWhenClosed { false };  // plugin tails are cleared once the gate reaches silence
    std::atomic<bool> outputGateSnapOpen { false };     // a closed gate reopens at once for a new cue (nothing audible is behind it)
    float outputGateGain = 1.0f;                        // audio thread only
    int outputGateRemaining = 0;                 // audio thread: samples left in the current ramp
    int outputGateSeenTarget = 1;                // audio thread: the target the current ramp heads for
    std::atomic<bool> chainResetPending { false };   // set by the audio thread once the gate closed after a panic
    std::atomic<bool> deviceRunning { false };
    bool deviceExpected = false;                        // initialise() ran: a stopped device refuses new cues (offline tests never initialise)
    void applyOutputGate (juce::AudioBuffer<float>& output, int numSamples) noexcept;
    void resetAllChainsForPanic();   // message thread
    std::atomic<int> outputChannelLimit { maxDeviceOutputs };   // set when a device starts, read by the callback
    std::atomic<bool> outputMaskOutOfRange { false };           // a non-ASIO device runs on channels past the limit: nothing goes out until the trim
    juce::String lastPolicyType;                                // device type seen by the last enforceOutputLimit(): a switch that lost its device gets a stereo retry
    /** Opens the current type's device (the setup's, else the type's default) with outputs 1-2 only. */
    juce::String openStereoDefault();
    bool formatPrepared = false;                                // prepare() ran once: a restart at the same format keeps the players' state
    double previousSampleRate = 0.0;
    int previousBlockSize = 0;
    std::atomic<int> numDeviceInputs { 0 };
    std::atomic<bool> reapNeeded { false };
    std::array<const float*, maxDeviceInputs> inputPointers {};   // >=32-output path: input block pointers, no allocation

    PluginChain masterChain;
    std::map<juce::String, std::unique_ptr<PluginChain>> cueChains;   // keyed by Uuid string
    PluginChain::Listener* chainListener = nullptr;

    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<int> blockSize { 512 };
    juce::AudioBuffer<float> mixBuffer, playerBuffer;  // audio-thread scratch
    juce::AudioBuffer<float> deviceScratch;            // >32 device outputs: juce::AudioBuffer would heap-allocate its channel table per callback

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace gocue
