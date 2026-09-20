#pragma once

#include "model/Cue.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace gocue
{

/** An ordered list of plugin inserts, processed in place on a stereo buffer.

    Editing happens on the message thread; process() runs on the audio thread and only
    takes a short lock around the slot list, so instances are created / prepared before
    they are inserted and destroyed after they have been removed. Each plugin is called
    under its own callback lock and skipped while it is suspended, as JUCE's own hosts do. */
class PluginChain : private juce::AudioProcessorListener
{
    /** A parameter edit reaches both DSP instances at the next block, including while the peer is audible.
        The listener only writes preallocated atomics; apply() runs under the chain's existing callback guard. */
    struct GroupParameterMirror final : private juce::AudioProcessorListener
    {
        GroupParameterMirror (juce::AudioPluginInstance&, juce::AudioPluginInstance&);
        ~GroupParameterMirror() override;
        void refresh();
        void apply();
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        juce::AudioPluginInstance& source;
        juce::AudioPluginInstance& target;
        const int count;
        std::unique_ptr<std::atomic<float>[]> values;
        std::vector<float> applied;
    };

public:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;   // null when the plugin could not be created
        PluginSlotState state;                               // saved description + state (kept for missing plugins)
        std::atomic<bool> bypassed { false };
        // A group edit publishes its targets together. The callback snapshots them without waiting, then
        // delays the switch by the latency up to this slot so all members switch the same input frame.
        std::atomic<unsigned> groupBypassRevision { 0 }; // zero: ordinary single-slot crossfade
        bool audioBypassed = false, pendingBypassed = false;
        bool requestedBypassed = false;
        unsigned requestedGroupRevision = 0;
        std::unique_ptr<Slot> groupPeer;        // second DSP history; moves / appends atomically with its slot
        std::unique_ptr<GroupParameterMirror> groupMirror;
        std::unique_ptr<Slot> pendingGroupPeer; // prepared replacement; ownership normalized on the message thread after handover
        std::unique_ptr<GroupParameterMirror> pendingGroupMirror;
        std::atomic<Slot*> activeGroupPeer { nullptr }; // callback selects a prepared peer; ownership stays on the message thread
        Slot* getGroupPeer() const noexcept
        {
            auto* active = activeGroupPeer.load (std::memory_order_acquire);
            return active != nullptr ? active : groupPeer.get();
        }
        GroupParameterMirror* incomingMirror = nullptr; // peer only; owned by the corresponding original slot
        unsigned audioGroupRevision = 0, pendingGroupRevision = 0;
        int groupSwitchSamples = 0;
        std::vector<unsigned char> groupBypassDelay, groupBypassTargets;
        size_t groupBypassWrite = 0;
        std::atomic<bool> faulted { false };    // threw (or produced NaN / Inf) in processBlock, or threw while preparing / loading state: dry from then on
        bool faultReported = false;             // message thread: the operator has been told
        std::atomic<int> busyBlocks { 0 };      // audio thread: blocks in a row its callback lock was busy (a dry pass each)
        bool stallReported = false;             // message thread: the operator has been told about the dry passes
        juce::AudioBuffer<float> scratch;                    // used for bypass and for plugins that need > 2 channels
        int numScratchChannels = 2;

        // Plugin delay compensation (0.9.8): the dry signal of a bypassed (or busy / suspended / faulted) plugin goes
        // through a delay of the plugin's own latency, so switching it on and off does not move the sound in time.
        // The line is sized on the message thread (prepare, a latency change) under the chain lock; the callback
        // reads and writes it.
        std::atomic<int> latency { 0 };          // samples the plugin delays its output by, as it reported last
        juce::AudioBuffer<float> dryDelay;       // 2-channel ring of the recent input: latency + a few blocks
        int dryDelayWrite = 0;                   // audio thread: the ring's write index
        float wetMix = 1.0f;                     // audio thread: 1 = the plugin's output, 0 = the delayed dry signal; ramps when the bypass changes
        int skipped = 0;                         // audio thread: input samples the plugin has not seen (its callback lock was busy, it was suspended): fed to it from the ring, catchUpBlocks per callback, before it runs again - its own time never falls behind the show
        std::atomic<bool> overflow { false };    // the backlog outgrew the ring: the plugin is reset on the message thread (recoverAfterStalls) instead of catching up, dry until then
        std::atomic<bool> resetPending { false }; // the message thread reset the plugin: the callback takes it from there (backlog dropped, delay line primed)
        int prime = 0;                           // audio thread: dry samples still to pass after a reset while the plugin's own delay line fills from the show again

        bool isMissing() const noexcept { return plugin == nullptr; }
    };

    /** Clears every plugin's delay lines / tails (AudioProcessor::reset), from the audio thread after a panic. */
    void resetProcessing() noexcept;

    struct Listener
    {
        virtual ~Listener() = default;
        /** Called (message thread) right before an instance is destroyed: close its editor now. */
        virtual void pluginAboutToBeRemoved (PluginChain&, juce::AudioPluginInstance&) {}
        /** Slots were added, removed, moved or bypassed. */
        virtual void chainChanged (PluginChain&) {}
    };

    /** Creates an instance for a saved slot, or returns null and fills 'error'. */
    using Factory = std::function<std::unique_ptr<juce::AudioPluginInstance> (const PluginSlotState&, juce::String& error)>;

    static constexpr double maxTailSeconds = 10.0;
    static constexpr int maxScratchChannels = 31;   // JUCE's AudioBuffer holds < 32 channel pointers inline (a 32-channel view needs 33 and allocates on the audio thread); a plugin wanting more is refused
    static constexpr int stallBlocks = 200;         // dry passes in a row before the operator hears of a plugin that does not answer
    static constexpr double bypassRampSeconds = 0.005;   // the wet <-> dry crossfade when a bypass switch moves (32 samples at least)

    /** Group alignment is opt-in for LiveMix microphone chains; other hosts keep ordinary slot processing. */
    explicit PluginChain (bool enableGroupBypass = false) : groupBypassEnabled (enableGroupBypass) {}
    ~PluginChain() override;

    void setListener (Listener* newListener) noexcept { listener = newListener; }

    /** (Re)prepares every plugin for the given rate / block size. Not audio-thread safe. */
    void prepare (double sampleRate, int blockSize);
    double getSampleRate() const noexcept { return sampleRate; }
    int getBlockSize() const noexcept { return blockSize; }

    int getNumSlots() const;
    /** Message thread only; the reference is valid until the chain changes. */
    Slot& getSlot (int index);
    const Slot& getSlot (int index) const;

    /** Takes ownership of a freshly created instance, applies initialState (if any), prepares it
        and inserts it (insertAt == -1 appends). */
    void addPlugin (std::unique_ptr<juce::AudioPluginInstance> plugin, const PluginSlotState& initialState = {}, int insertAt = -1);
    /** Keeps a slot whose plugin is unavailable so its saved state survives the next save. */
    void addMissingSlot (const PluginSlotState& state, int insertAt = -1);
    void removePlugin (int index);
    bool movePlugin (int from, int to);
    /** Bypassed plugins still run (so delays / reverbs keep time) but their output is discarded: the dry signal,
        delayed by the plugin's latency, goes on instead, with a short crossfade either way (no jump in time, no click). */
    void setBypassed (int index, bool shouldBypass);
    /** One group edit: two complete, latency-aligned paths share one final 5 ms crossfade.
        A single changed slot retains its ordinary crossfade. Failure leaves every bypass target untouched. Message thread.
        Requires group alignment enabled at construction. Suppress notifications when repairing the
        runtime state of an unchanged document group. */
    enum class GroupBypassResult { unchanged, changed, failed };
    GroupBypassResult setBypassedTogether (const std::vector<int>& indices, bool shouldBypass, bool notifyListeners = true);
    /** Preflight for a command spanning multiple chains. Does not publish bypass targets. */
    bool prepareBypassedTogether (const std::vector<int>& indices, bool shouldBypass);
    /** Message thread: prepare the second complete signal path before a multi-slot group can be switched.
        Both paths keep running to preserve independent plugin histories, including nonmembers between group slots.
        Instances and buffers are created here, never in process(). The factory also serves later chain edits. */
    void setGroupBypassFactory (Factory factory) { groupFactory = std::move (factory); }
    bool prepareGroupBypass();
    void clear();

    /** Captures every slot's description + getStateInformation() as PluginSlotState. Message thread. */
    /** 'complete' (when given) is cleared when a plugin could not report its state: the caller must not treat the
        result as a faithful save. A state that was read updates the slot's cached state. */
    std::vector<PluginSlotState> getStates (bool* complete = nullptr) const;

    /** Replaces the chain from saved states, instantiating through 'factory'. Failed slots are kept as
        missing. Returns one message per failure. */
    juce::StringArray restore (const std::vector<PluginSlotState>& states, const Factory& factory);
    /** Prepares all saved slots before appending them in one edit. Existing instances keep their histories. */
    juce::StringArray append (const std::vector<PluginSlotState>& states, const Factory& factory);

    /** True when the slots (count, plugin identity) match 'states'; parameter values and the bypass flags are not
        compared (applyStates sets both). Message thread. Used to decide whether an undo step must rebuild this chain. */
    bool matchesStructure (const std::vector<PluginSlotState>& states) const;
    /** For a chain whose structure matches 'states': pushes each slot's saved state / bypass into the live
        instance (undo of a preset change without rebuilding the instances). */
    void applyStates (const std::vector<PluginSlotState>& states);

    /** Sum of the tails of the active (non-bypassed) plugins in series plus every plugin's latency (in flight on the
        wet or the dry path), clamped to [0, maxTailSeconds]. Any thread. */
    double getTailSeconds() const;
    /** Recompute the cached tail (message thread): a plugin may report a longer / shorter tail after a parameter
        change without any structural edit. */
    void refreshTailCache() { updateTailCache(); }
    /** Recompute what the callback reads, from the message thread: the tail, and each plugin's dry delay line when
        the plugin now reports another latency (a limiter's look-ahead changed). Called when a plugin reported a change. */
    void refreshPluginCaches();
    /** Message thread, every UI tick: a plugin whose missed input outgrew its ring (a long stall) is reset - its own
        time starts afresh with the show's - and goes back to its output. Cheap when nothing is pending. */
    void recoverAfterStalls();

    /** True once (since the previous call) when any hosted plugin reported a parameter / state change,
        e.g. the user turned a knob in an editor. Any thread. Used for dirty tracking. */
    bool consumeStateChanged() noexcept { return stateChanged.exchange (false, std::memory_order_acq_rel); }
    /** The names of the plugins that faulted since the previous call (message thread): the operator is told once. */
    juce::StringArray takeNewFaults();
    /** Plugins whose callback lock has been busy for stallBlocks blocks in a row (passed dry meanwhile), once each. */
    juce::StringArray takeNewStalls();
    /** The latency the chain adds, in samples: every live plugin's own, bypassed or not (a bypassed plugin's dry
        signal is delayed by as much). Message thread. */
    int getLatencySamples() const;

    /** Audio thread: processes channels 0-1 of buffer[0, numSamples) in place. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples);

private:
    bool prepareSlot (Slot& slot, bool preparePeer = true);   // false when the plugin threw (or wants too many channels)
    std::unique_ptr<Slot> createGroupPeer (Slot& slot, const PluginSlotState* captured = nullptr); // message thread only
    void refreshGroupState(); // builds replacements outside the chain lock
    void installGroupState() noexcept; // chain lock held, original path alone audible; selects prepared pointers only
    PluginSlotState captureState (Slot& slot, bool* complete) const;
    void processLocked (juce::AudioBuffer<float>& buffer, int numSamples);   // one block of at most the prepared size, the lock held
    void processSlots (juce::AudioBuffer<float>& buffer, int numSamples, bool peer = false);
    void processGroupPaths (juce::AudioBuffer<float>& buffer, int numSamples);
    bool groupPathsMatch() const noexcept;
    void setGroupPathTargets (int path, bool grouped) noexcept;
    void updateTailCache();          // message thread: the tail the callback reads without asking any plugin
    void updateDelayLines();         // message thread, under the lock: each slot's dry delay follows the plugin's latency
    void updateGroupDelayLines (bool reset = false); // message thread, existing chain lock held: sizes the bypass control delays
    void snapshotBypassTargets() noexcept; // audio thread: one bounded attempt; an in-flight edit keeps the previous targets
    static void sizeDelayLine (Slot& slot, int latency, int blockSize);   // (re)allocates and clears - never on the audio thread
    static void delayDryInPlace (Slot& slot, juce::AudioBuffer<float>& dry, int numSamples) noexcept;   // audio thread: records channels 0-1 in the slot's ring and, when the plugin has latency, replaces them with the delayed signal
    bool catchUpSkipped (Slot& slot, int budgetSamples) noexcept;   // audio thread, the plugin's callback lock held: feeds it up to 'budgetSamples' of the input it missed; false when it faulted doing so
    void noteSkipped (Slot& slot, int numSamples) noexcept;         // audio thread: the plugin did not see this block (it is in the ring); flags an overflow when the ring cannot hold the backlog
    static constexpr int ringBlocks = 8;         // the ring holds latency + this many blocks: a stall of that many blocks is caught up in full
    static constexpr int catchUpBlocks = 2;      // missed input fed per callback (so a callback runs the plugin three times at most): a backlog drains by one block per callback
    void markFaulted (Slot& slot) noexcept;
    static bool isFinite (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
    void clearSlots (bool notify);   // the destructor clears without chainChanged (pluginAboutToBeRemoved still closes editors)
    void insertSlot (std::unique_ptr<Slot> slot, int insertAt);
    juce::StringArray loadSlots (const std::vector<PluginSlotState>& states, const Factory& factory, bool append);
    void destroySlot (std::unique_ptr<Slot> slot, bool notifyEditor = true);
    void notifyChanged();

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;

    mutable juce::CriticalSection lock;      // guards 'slots' between the audio thread and edits
    std::vector<std::unique_ptr<Slot>> slots;
    std::atomic<int> slotCount { 0 };        // slots.size(), for getNumSlots() from any thread without the lock
    std::atomic<unsigned> bypassRevision { 0 }; // odd during a message-thread bypass edit, even after publication
    const bool groupBypassEnabled;
    Factory groupFactory;
    bool groupPathsPrepared = false;
    juce::AudioBuffer<float> groupOutput;
    float groupMix = 0.0f;                   // 0: original path, 1: peer; one final-output ramp
    int groupDestination = 0;
    int groupReadySamples[2] { 0, 0 };       // input-frame switch / fresh-instance latency in flight
    std::atomic<bool> groupStateChanged { false }; // non-parameter state: copied on the message thread
    enum class GroupStatePhase { idle, prepared, handover, retired };
    std::atomic<GroupStatePhase> groupStatePhase { GroupStatePhase::idle };
    juce::MidiBuffer midi;
    double sampleRate = 44100.0;
    int blockSize = 512;
    int bypassRampSamples = 220;             // bypassRampSeconds at the prepared rate
    Listener* listener = nullptr;
    std::atomic<bool> stateChanged { false };
    std::atomic<bool> faultRaised { false };   // a slot faulted since takeNewFaults()
    std::atomic<bool> stallRaised { false };   // a slot crossed stallBlocks since takeNewStalls()
    std::atomic<bool> overflowRaised { false };   // a slot's backlog outgrew its ring: recoverAfterStalls() has work
    std::atomic<float> tailSecondsCache { 0.0f };   // getTailSeconds() for the audio thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};

} // namespace gocue
