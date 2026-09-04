#pragma once

#include "model/Cue.h"   // PluginSlotState

#include <juce_core/juce_core.h>

#include <vector>

namespace gocue::livemix
{

/** Where a channel's (or an FX channel's) signal goes: the master bus and / or a device output pair. */
struct MixOutput
{
    bool master = true;
    bool direct = false;
    int directFirst = 2;   // 0-based device output of the pair's left side (the right side is directFirst + 1)
};

/** How much of a mic channel goes to one FX channel, taken before (pre) or after (post) the channel's VST3 chain. */
struct MixSend
{
    juce::Uuid fx = juce::Uuid::null();
    double amount = 0.0;   // 0..1
    bool pre = false;
};

/** A mic channel: one ASIO input (or a stereo pair) through a VST3 chain, a mic ON/OFF switch, sends and outputs.
    No gain, no fader: the channel is unity. */
struct MixChannel
{
    juce::Uuid id;
    juce::String name;
    bool on = true;
    int inputFirst = 0;    // 0-based device input; a stereo channel takes inputFirst and inputFirst + 1
    bool stereo = false;
    std::vector<PluginSlotState> chain;
    std::vector<MixSend> sends;   // one per FX channel after sanitise()
    MixOutput output;
};

/** An FX channel (reverb, delay ...): the sum of the sends through a VST3 chain, then a return amount, to the outputs. */
struct MixFx
{
    juce::Uuid id;
    juce::String name;
    std::vector<PluginSlotState> chain;
    double returnAmount = 1.0;   // 0..1
    bool mono = false;           // the FX output summed to mono (L+R)/2 on both sides
    MixOutput output;
};

struct MixMaster
{
    std::vector<PluginSlotState> chain;
    int outputFirst = 0;   // the main output pair
};

struct MixDevice
{
    juce::String name;      // ASIO device name ("" = whatever opens)
    int bufferSize = 256;
    double sampleRate = 48000.0;
};

/** The session file (.livemix): everything the app needs to come back exactly as it was. */
struct MixSession
{
    static constexpr int currentVersion = 1;
    static constexpr int maxChannels = 8;
    static constexpr int maxFx = 4;
    static constexpr int maxDeviceChannels = 64;
    static constexpr int maxChainSlots = 16;                        // plugins per chain
    static constexpr juce::int64 maxFileBytes = 32 * 1024 * 1024;   // a session file beyond this is not a session
    static constexpr const char* fileExtension = ".livemix";

    juce::String name;
    MixDevice device;
    std::vector<MixChannel> channels;
    std::vector<MixFx> fx;
    MixMaster master;

    MixChannel* findChannel (const juce::Uuid& id) noexcept;
    const MixChannel* findChannel (const juce::Uuid& id) const noexcept;
    MixFx* findFx (const juce::Uuid& id) noexcept;
    const MixFx* findFx (const juce::Uuid& id) const noexcept;
    int indexOfFx (const juce::Uuid& id) const noexcept;

    /** A new mic channel / FX channel with the next free name, appended. Returns its index (-1 at the limit). */
    int addChannel (const juce::String& newName = {});
    int addFx (const juce::String& newName = {});
    void removeChannel (const juce::Uuid& id);
    void removeFx (const juce::Uuid& id);

    /** The send of 'channel' to 'fx' (created when missing). */
    MixSend& sendFor (MixChannel& channel, const juce::Uuid& fxId);

    /** Clamps every value, drops sends to FX channels that do not exist, adds the missing ones, replaces duplicate
        ids, trims to the limits. Called after loading and before saving. */
    void sanitise();

    juce::String toJson() const;
    static juce::Result fromJson (const juce::String& json, MixSession& out, juce::StringArray* warnings = nullptr);

    /** Verified atomic save (SafeFileWrite) / load. */
    juce::Result save (const juce::File& file) const;
    static juce::Result load (const juce::File& file, MixSession& out, juce::StringArray* warnings = nullptr);
    /** "" when a file of that size may be read as a session. */
    static juce::Result checkFileSize (juce::int64 bytes);
};

} // namespace gocue::livemix
