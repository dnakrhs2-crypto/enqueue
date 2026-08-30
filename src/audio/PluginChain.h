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
    they are inserted and destroyed after they have been removed. */
class PluginChain
{
public:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;   // null when the plugin could not be created
        PluginSlotState state;                               // saved description + state (kept for missing plugins)
        std::atomic<bool> bypassed { false };
        juce::AudioBuffer<float> scratch;                    // used when the plugin needs more than 2 channels
        int numScratchChannels = 2;

        bool isMissing() const noexcept { return plugin == nullptr; }
    };

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
    ~PluginChain();

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
    void setBypassed (int index, bool shouldBypass);
    void clear();

    /** Captures every slot's description + getStateInformation() as PluginSlotState. Message thread. */
    std::vector<PluginSlotState> getStates() const;

    /** Replaces the chain from saved states, instantiating through 'factory'. Failed slots are kept as
        missing. Returns one message per failure. */
    juce::StringArray restore (const std::vector<PluginSlotState>& states, const Factory& factory);

    /** Longest tail of the active (non-bypassed) plugins, clamped to [0, maxTailSeconds]. Any thread. */
    double getTailSeconds() const;

    /** Audio thread: processes channels 0-1 of buffer[0, numSamples) in place. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples);

private:
    void prepareSlot (Slot& slot);
    void insertSlot (std::unique_ptr<Slot> slot, int insertAt);
    void notifyChanged();

    mutable juce::CriticalSection lock;      // guards 'slots' between the audio thread and edits
    std::vector<std::unique_ptr<Slot>> slots;
    juce::MidiBuffer midi;
    double sampleRate = 44100.0;
    int blockSize = 512;
    Listener* listener = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};

} // namespace gocue
