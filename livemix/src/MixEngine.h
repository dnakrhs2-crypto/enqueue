#pragma once

#include "MixModel.h"
#include "audio/PluginChain.h"
#include "audio/PluginHost.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace gocue::livemix
{

/** The live graph on one ASIO device:

        input(s) -> [pre tap] -> channel VST3 chain -> [post tap] -> mic ON/OFF ramp -> master bus / direct output pair
        FX channel: sum of sends -> FX chain -> return amount -> master bus / direct output pair
        master bus -> master chain -> main output pair

    Parameters are atomics (no lock for a live change); the graph's node lists change under a short lock (a pointer
    swap); plugins are created and destroyed on the message thread outside it. renderBlock() is the whole callback
    and runs offline in the tests. */
class MixEngine : private juce::AudioIODeviceCallback
{
public:
    static constexpr int maxChannels = MixSession::maxChannels;
    static constexpr int maxFx = MixSession::maxFx;
    static constexpr int maxDeviceChannels = MixSession::maxDeviceChannels;
    static constexpr double onOffRampSeconds = 0.005;

    MixEngine();
    ~MixEngine() override;

    /** Opens the saved ASIO device (or the first ASIO device) with every input and output. "" on success. */
    juce::String initialise (const juce::XmlElement* savedDeviceState);
    /** Opens an ASIO device by name (an empty name keeps the current one) with an optional rate / buffer and every
        channel, and registers the callback. On failure the device that was running is restored; the message says
        what failed (and whether the rollback did too). Message thread. */
    juce::String openDevice (const juce::String& name, double sampleRate = 0.0, int bufferSize = 0);
    /** A new buffer size on the running device (every channel kept). */
    juce::String setBufferSize (int samples);
    /** After a device change: every input and output of the device is enabled. A refused widening goes back to the
        channels that worked (JUCE closes the device on a failed reconfiguration). */
    juce::String openAllChannels();
    /** Opens the device a session was saved with (name, rate, buffer, every channel) when it is not the one running.
        "" when it runs (or the session names none); otherwise why not - the device that was running is kept then. */
    juce::String openSessionDevice (const MixDevice& device);
    void shutdown();
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    PluginHost& getPluginHost() noexcept { return pluginHost; }
    bool isDeviceRunning() const noexcept { return deviceRunning.load (std::memory_order_acquire); }
    double getSampleRate() const noexcept { return sampleRate.load (std::memory_order_relaxed); }
    int getBlockSize() const noexcept { return blockSize.load (std::memory_order_relaxed); }
    int getNumDeviceInputs() const noexcept { return numDeviceInputs.load (std::memory_order_relaxed); }
    int getNumDeviceOutputs() const noexcept { return numDeviceOutputs.load (std::memory_order_relaxed); }
    /** Input + output latency of the device, in milliseconds (0 offline). */
    double getLatencyMs() const;
    /** Share of the block time the last callbacks needed (0..1+, smoothed). */
    double getDspLoad() const noexcept { return dspLoad.load (std::memory_order_relaxed); }
    int getXRunCount() const;

    /** Offline preparation (the callback does the same when the device starts). */
    void prepare (double newSampleRate, int newBlockSize);
    /** Renders one block: the outputs are cleared and then written. Inputs / outputs may have fewer channels than
        the graph refers to (those refer to silence / go nowhere). */
    void renderBlock (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples);

    /** Makes the graph match the session. Without 'restoreChains' (a structural edit) existing nodes are reused by id
        with their live chains untouched and only added nodes are built. With it (a file opened) every node and the
        master chain are rebuilt with their saved plugins, off the graph, and published in one swap: the callback
        never runs a mix of old and new. Plugin errors go to 'errors'. */
    void applySession (const MixSession& session, juce::StringArray* errors = nullptr, bool restoreChains = false);
    /** Copies the live plugin states / order into the session's chain fields (before a save). */
    void captureLivePluginStates (MixSession& session) const;

    void setChannelOn (const juce::Uuid& id, bool on);
    void setChannelInput (const juce::Uuid& id, int first, bool stereo);
    void setChannelOutput (const juce::Uuid& id, const MixOutput& output);
    void setSend (const juce::Uuid& channelId, const juce::Uuid& fxId, double amount, bool pre);
    void setFxReturn (const juce::Uuid& fxId, double amount);
    void setFxOutput (const juce::Uuid& fxId, const MixOutput& output);
    void setMasterOutput (int first);

    PluginChain* getChannelChain (const juce::Uuid& id) const noexcept;
    PluginChain* getFxChain (const juce::Uuid& id) const noexcept;
    PluginChain& getMasterChain() noexcept { return *master.chain; }
    void forEachChain (const std::function<void (PluginChain&)>& fn) const;

    struct Meter { float left = 0.0f, right = 0.0f; };
    /** Peak since the last read (the UI holds and decays). */
    Meter readChannelMeter (const juce::Uuid& id);
    Meter readFxMeter (const juce::Uuid& id);
    Meter readMasterMeter();

private:
    struct MeterCell
    {
        std::atomic<float> left { 0.0f }, right { 0.0f };

        void push (float l, float r) noexcept
        {
            float cur = left.load (std::memory_order_relaxed);
            while (l > cur && ! left.compare_exchange_weak (cur, l, std::memory_order_relaxed)) {}
            cur = right.load (std::memory_order_relaxed);
            while (r > cur && ! right.compare_exchange_weak (cur, r, std::memory_order_relaxed)) {}
        }

        Meter take() noexcept { return { left.exchange (0.0f, std::memory_order_relaxed), right.exchange (0.0f, std::memory_order_relaxed) }; }
    };

    struct Send
    {
        std::atomic<float> amount { 0.0f };
        std::atomic<bool> pre { false };
    };

    struct ChannelNode
    {
        juce::Uuid id;
        std::atomic<bool> on { true };
        std::atomic<int> inputFirst { 0 };
        std::atomic<bool> stereo { false };
        std::atomic<bool> toMaster { true }, direct { false };
        std::atomic<int> directFirst { 2 };
        std::unique_ptr<PluginChain> chain = std::make_unique<PluginChain>();
        std::array<Send, (size_t) maxFx> sends;
        float onGain = 1.0f;   // audio thread
        MeterCell meter;
    };

    struct FxNode
    {
        juce::Uuid id;
        std::atomic<float> returnAmount { 1.0f };
        std::atomic<bool> toMaster { true }, direct { false };
        std::atomic<int> directFirst { 2 };
        std::unique_ptr<PluginChain> chain = std::make_unique<PluginChain>();
        MeterCell meter;
    };

    struct MasterNode
    {
        std::unique_ptr<PluginChain> chain = std::make_unique<PluginChain>();
        std::atomic<int> outputFirst { 0 };
        MeterCell meter;
    };

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;

    juce::AudioIODeviceType* findAsioType();
    void ensureCallback();
    ChannelNode* findChannel (const juce::Uuid& id) const noexcept;
    FxNode* findFx (const juce::Uuid& id) const noexcept;
    static void applyOutput (const MixOutput& output, std::atomic<bool>& toMaster, std::atomic<bool>& direct, std::atomic<int>& directFirst);
    static void addToOutputs (float* const* outputs, int numOutputs, int first, const juce::AudioBuffer<float>& source, int offset, int numSamples) noexcept;
    juce::AudioDeviceManager deviceManager;
    PluginHost pluginHost;
    bool callbackAdded = false;

    juce::CriticalSection lock;   // the node lists and the buffers: held by the callback, taken briefly by edits
    std::vector<std::unique_ptr<ChannelNode>> channels;
    std::vector<std::unique_ptr<FxNode>> fxNodes;
    MasterNode master;

    juce::AudioBuffer<float> chBuf, preBuf, masterBus;
    std::array<juce::AudioBuffer<float>, (size_t) maxFx> fxBus;

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> blockSize { 256 };
    std::atomic<bool> deviceRunning { false };
    std::atomic<int> numDeviceInputs { 0 }, numDeviceOutputs { 0 };
    std::atomic<double> dspLoad { 0.0 };
    std::atomic<double> inputLatencyMs { 0.0 }, outputLatencyMs { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixEngine)
};

} // namespace gocue::livemix
