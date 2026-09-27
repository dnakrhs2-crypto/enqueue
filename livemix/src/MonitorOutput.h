#pragma once

#include "lm_asrc.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gocue::livemix
{
/** One producer (the graph) and one consumer (the output device). All storage and filter coefficients are
    prepared before either callback runs. start/stop require the producer to have been detached first. */
class MonitorOutput : private juce::AudioIODeviceCallback
{
public:
    MonitorOutput() = default;
    ~MonitorOutput() override;
    juce::String start (juce::AudioIODeviceType& type, const juce::String& outputName,
                        double inputRate, int inputPeriod, int preferredOutputPeriod = 0, double preferredOutputRate = 0.0);
    void stop();
    /** Null channels produce silence, including blocks skipped by a busy graph. Full FIFO drops this block. */
    void push (const float* left, const float* right, int frames) noexcept;
    double getLatencyMs() const noexcept { return latencyMs; }
    double getTargetMs() const noexcept { return 1000.0 * target / inputRate; }
    /** FIFO fill immediately before a pull is the drift controller's measurement point. */
    double getFillMs() const noexcept;
    std::uint64_t getUnderruns() const noexcept { return underruns.load (std::memory_order_relaxed); }
    std::uint64_t getOverruns() const noexcept { return overruns.load (std::memory_order_relaxed); }
    double getOutputSampleRate() const noexcept { return outputRate; }
    int getOutputPeriod() const noexcept { return outputPeriod; }
    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    /** Offline seam: no AudioIODevice. The nominal rates configure the ASRC; callers schedule independent clocks. */
    void prepareForTest (double producerRate, int producerPeriod, double consumerRate, int consumerPeriod);
    void pullForTest (float* interleavedOutput, int frames) noexcept { pull (interleavedOutput, frames); }

private:
    void prepare (double producerRate, int producerPeriod, double consumerRate, int consumerPeriod, int outputLatency);
    void resetConsumer() noexcept;
    void pull (float* interleavedOutput, int frames) noexcept;
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override { running.store (false, std::memory_order_release); }
    void audioDeviceError (const juce::String&) override { running.store (false, std::memory_order_release); }

    std::unique_ptr<juce::AudioIODevice> device;
    std::vector<float> fifo, outputScratch;
    std::vector<double> asrcStorage; // double alignment required by the shared C module
    lm_asrc* asrc = nullptr;
    lm_drift drift {};
    double inputRate = 48000.0, outputRate = 48000.0, target = 0.0, latencyMs = 0.0;
    int outputPeriod = 256;
    std::uint64_t capacity = 0;
    std::atomic<std::uint64_t> writePosition { 0 };
    std::atomic<std::uint64_t> readPosition { 0 };
    std::atomic<std::uint64_t> underruns { 0 }, overruns { 0 };
    std::atomic<bool> running { false };
    std::uint64_t observedOverruns = 0; // consumer only
    bool prefill = true;              // consumer only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonitorOutput)
};
} // namespace gocue::livemix
