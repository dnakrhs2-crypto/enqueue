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
public:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;   // null when the plugin could not be created
        PluginSlotState state;                               // saved description + state (kept for missing plugins)
        std::atomic<bool> bypassed { false };
        std::atomic<bool> faulted { false };    // threw inside processBlock: bypassed from then on (a hung plugin cannot be helped in-process)
        juce::AudioBuffer<float> scratch;                    // used for bypass and for plugins that need > 2 channels
        int numScratchChannels = 2;

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

    PluginChain() = default;
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
    /** Bypassed plugins still run (so delays / reverbs keep time) but their output is discarded. */
    void setBypassed (int index, bool shouldBypass);
    void clear();

    /** Captures every slot's description + getStateInformation() as PluginSlotState. Message thread. */
    std::vector<PluginSlotState> getStates() const;

    /** Replaces the chain from saved states, instantiating through 'factory'. Failed slots are kept as
        missing. Returns one message per failure. */
    juce::StringArray restore (const std::vector<PluginSlotState>& states, const Factory& factory);

    /** True when the slots (count, plugin identity, bypass) match 'states'; parameter values are not compared.
        Message thread. Used to decide whether an undo step must rebuild this chain. */
    bool matchesStructure (const std::vector<PluginSlotState>& states) const;
    /** For a chain whose structure matches 'states': pushes each slot's saved state / bypass into the live
        instance (undo of a preset change without rebuilding the instances). */
    void applyStates (const std::vector<PluginSlotState>& states);

    /** Sum of the tails of the active (non-bypassed) plugins in series, clamped to [0, maxTailSeconds]. Any thread. */
    double getTailSeconds() const;

    /** True once (since the previous call) when any hosted plugin reported a parameter / state change,
        e.g. the user turned a knob in an editor. Any thread. Used for dirty tracking. */
    bool consumeStateChanged() noexcept { return stateChanged.exchange (false, std::memory_order_acq_rel); }

    /** Audio thread: processes channels 0-1 of buffer[0, numSamples) in place. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples);

private:
    void prepareSlot (Slot& slot);
    void clearSlots (bool notify);   // the destructor clears without chainChanged (pluginAboutToBeRemoved still closes editors)
    void insertSlot (std::unique_ptr<Slot> slot, int insertAt);
    void destroySlot (std::unique_ptr<Slot> slot);
    void notifyChanged();

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;

    mutable juce::CriticalSection lock;      // guards 'slots' between the audio thread and edits
    std::vector<std::unique_ptr<Slot>> slots;
    juce::MidiBuffer midi;
    double sampleRate = 44100.0;
    int blockSize = 512;
    Listener* listener = nullptr;
    std::atomic<bool> stateChanged { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};

} // namespace gocue
