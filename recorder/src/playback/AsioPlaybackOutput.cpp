#include "IAudioOutput.h"
#include "audio/AsioTimingBridge.h"
#include "support/Platform.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>

namespace gocue::recorder
{
namespace
{
class AsioPlaybackOutput final : public IAudioOutput, private juce::AudioIODeviceCallback
{
public:
    ~AsioPlaybackOutput() override { close(); }
    AudioOutputInfo open(const AudioOutputConfig& config) override
    {
        if (device || !config.sampleRate || config.sampleRate > 768000 || config.deviceIndex < 0
            || config.leftPhysical < 0 || config.rightPhysical < 0 || config.bufferFrames < 0)
            throw std::invalid_argument("Invalid/already-open ASIO playback configuration");
        if (!AsioTimingBridge::hookCompiled()) throw std::runtime_error("ASIO timing tap unavailable; CPU/timer clock fallback disabled");
        fault.store(false); latest.store(-1); stamp = {};
        type.reset(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
        if (!type) throw std::runtime_error("JUCE ASIO unavailable");
        type->scanForDevices(); const auto names = type->getDeviceNames();
        if (config.deviceIndex >= names.size()) throw std::runtime_error("Requested ASIO device was not enumerated");
        device.reset(type->createDevice(names[config.deviceIndex], names[config.deviceIndex]));
        if (!device) throw std::runtime_error("Cannot create ASIO playback device");
        try
        {
            const auto channels = device->getOutputChannelNames().size();
            if (config.leftPhysical >= channels || config.rightPhysical >= channels) throw std::runtime_error("ASIO output map unavailable");
            if (recorderAsioTap.load(std::memory_order_acquire)) throw std::runtime_error("Another ASIO timing tap owns the device");
            // The same native hook feeds the round-03 bridge and the subsequent
            // JUCE output callback. No change to shared JUCE or AsioTimingBridge.
            owner = this; recorderAsioTap.store(&tap, std::memory_order_release); registered = true;
            juce::BigInteger outputs; outputs.setBit(config.leftPhysical); outputs.setBit(config.rightPhysical);
            const auto block = config.bufferFrames ? config.bufferFrames : device->getDefaultBufferSize();
            const auto error = device->open({}, outputs, config.sampleRate, block);
            if (error.isNotEmpty()) throw std::runtime_error(error.toStdString());
            if (device->getCurrentSampleRate() != config.sampleRate || device->getActiveInputChannels() != juce::BigInteger()
                || device->getActiveOutputChannels() != outputs) throw std::runtime_error("ASIO changed sample rate or physical output mapping");
            const auto frames = device->getCurrentBufferSizeSamples();
            if (frames <= 0 || frames > 262144) throw std::runtime_error("ASIO playback block size out of range");
            info = {names[config.deviceIndex], config.sampleRate, static_cast<std::uint32_t>(frames),
                device->getOutputLatencyInSamples(), config.leftPhysical, config.rightPhysical};
            l.resize(frames, 0); r.resize(frames, 0);
            leftIndex = config.leftPhysical <= config.rightPhysical ? 0 : 1;
            rightIndex = config.leftPhysical == config.rightPhysical ? leftIndex : 1 - leftIndex;
            return info;
        }
        catch (...) { close(); throw; }
    }
    void start(IAudioOutputClient& c) override
    {
        if (!device || client) throw std::logic_error("ASIO output is not ready for start");
        client = &c; device->start(this);
    }
    void close() noexcept override
    {
        if (device) { device->stop(); device->close(); }
        if (registered) { recorderAsioTap.store(nullptr, std::memory_order_release); owner = nullptr; registered = false; }
        client = nullptr; device.reset(); type.reset();
    }
    std::int64_t latestOutputSample() const noexcept override { return latest.load(std::memory_order_acquire); }
    juce::Result status() const override
    { return fault.load() ? juce::Result::fail("ASIO output callback stopped, errored, or timing/block size changed") : juce::Result::ok(); }
    void drainTiming() override { bridge.drain(); }
private:
    static AsioPlaybackOutput* owner;
    static void tap(const BlockStamp& s, const NativeInputView* views, std::uint32_t count) noexcept
    {
        auto& o = *owner;
        o.bridge.onAsioBlock(s, views, count);
        o.stamp = s; // hook and output callback are sequential on the SAME ASIO thread
        o.latest.store(s.samplePosition, std::memory_order_release);
    }
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int channels, int frames,
                                         const juce::AudioIODeviceCallbackContext&) override
    {
        for (int i = 0; i < channels; ++i) if (outputs[i]) std::fill_n(outputs[i], frames, 0.0f);
        if (!client || frames <= 0 || static_cast<std::uint32_t>(frames) != info.blockFrames
            || stamp.numSamples != info.blockFrames || stamp.sampleRate != info.sampleRate
            || (stamp.flags & (samplePositionValid | latenciesValid)) != (samplePositionValid | latenciesValid)
            || stamp.outputLatencySamples < 0 || stamp.callbackQpc <= 0 || channels <= (std::max)(leftIndex, rightIndex))
        { fault.store(true, std::memory_order_relaxed); return; }
        client->processOutput(stamp, l.data(), r.data());
        if (leftIndex == rightIndex)
        { if (outputs[leftIndex]) for (int i = 0; i < frames; ++i) outputs[leftIndex][i] = (l[i] + r[i]) * 0.5f; }
        else
        {
            if (outputs[leftIndex]) std::copy_n(l.data(), frames, outputs[leftIndex]);
            if (outputs[rightIndex]) std::copy_n(r.data(), frames, outputs[rightIndex]);
        }
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override { if (client) fault.store(true, std::memory_order_relaxed); }
    void audioDeviceError(const juce::String&) override { fault.store(true, std::memory_order_relaxed); }
    AsioTimingBridge bridge{qpcFrequency()};
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
    IAudioOutputClient* client = nullptr;
    AudioOutputInfo info;
    BlockStamp stamp{};
    std::vector<float> l, r;
    std::atomic<std::int64_t> latest{-1};
    std::atomic<bool> fault{false};
    int leftIndex = 0, rightIndex = 1;
    bool registered = false;
};
AsioPlaybackOutput* AsioPlaybackOutput::owner = nullptr;
}
std::unique_ptr<IAudioOutput> makeAsioPlaybackOutput() { return std::make_unique<AsioPlaybackOutput>(); }
}
