#include "MonitorOutput.h"

#include <algorithm>
#include <cmath>

namespace gocue::livemix
{
static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
static_assert (std::atomic<bool>::is_always_lock_free);

MonitorOutput::~MonitorOutput() { stop(); }

juce::String MonitorOutput::start (juce::AudioIODeviceType& type, const juce::String& outputName,
                                  double producerRate, int producerPeriod, int preferredOutputPeriod, double preferredOutputRate)
{
    stop();
    if (outputName.isEmpty() || ! std::isfinite (producerRate) || producerRate <= 0.0 || producerPeriod <= 0)
        return juce::String::fromUTF8 ("모니터 장치 설정이 올바르지 않습니다.");
    device.reset (type.createDevice (outputName, {}));
    if (device == nullptr)
        return juce::String::fromUTF8 ("모니터 장치를 만들지 못했습니다: ") + outputName;

    auto fail = [this] (const juce::String& error) { stop(); return error; };
    if (device->getOutputChannelNames().size() < 2)
        return fail (juce::String::fromUTF8 ("모니터 장치에 출력 1-2가 필요합니다."));
    const auto rates = device->getAvailableSampleRates();
    double rate = preferredOutputRate > 0.0 ? preferredOutputRate : producerRate;
    if (! rates.contains (rate))
    {
        rate = device->getCurrentSampleRate();
        if (! rates.contains (rate)) rate = rates.isEmpty() ? producerRate : rates[0];
    }
    const auto sizes = device->getAvailableBufferSizes();
    const int wantedPeriod = preferredOutputPeriod > 0 ? preferredOutputPeriod : producerPeriod;
    const int period = sizes.contains (wantedPeriod) ? wantedPeriod : device->getDefaultBufferSize();
    juce::BigInteger outputs;
    outputs.setRange (0, 2, true);
    const auto error = device->open ({}, outputs, rate, period);
    if (error.isNotEmpty()) return fail (error);
    if (! device->isOpen() || device->getActiveOutputChannels() != outputs
        || device->getCurrentSampleRate() <= 0.0 || device->getCurrentBufferSizeSamples() <= 0)
        return fail (juce::String::fromUTF8 ("모니터 장치의 출력 1-2를 열지 못했습니다."));
    prepare (producerRate, producerPeriod, device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
             device->getOutputLatencyInSamples());
    device->start (this);
    const auto startError = device->getLastError();
    if (startError.isNotEmpty()) return fail (startError);
    if (! device->isPlaying() || ! isRunning())
        return fail (juce::String::fromUTF8 ("모니터 장치를 시작하지 못했습니다."));
    return {};
}

void MonitorOutput::stop()
{
    if (device != nullptr)
    {
        device->stop(); // joins the consumer before its storage can be reused
        device->close();
        device.reset();
    }
    running.store (false, std::memory_order_release);
}

void MonitorOutput::prepare (double producerRate, int producerPeriod, double consumerRate, int consumerPeriod, int outputLatency)
{
    inputRate = producerRate;
    outputRate = consumerRate;
    outputPeriod = consumerPeriod;
    target = 2.0 * consumerPeriod * producerRate / consumerRate + producerPeriod + 0.003 * producerRate;
    capacity = 1;
    const double required = juce::jmax (0.25 * inputRate, 2.0 * target + producerPeriod);
    while ((double) capacity < required) capacity <<= 1;
    fifo.assign ((size_t) capacity * 2, 0.0f);
    outputScratch.assign ((size_t) outputPeriod * 2, 0.0f);
    asrcStorage.resize ((lm_asrc_size (2) + sizeof (double) - 1) / sizeof (double));
    asrc = reinterpret_cast<lm_asrc*> (asrcStorage.data());
    lm_asrc_init (asrc, 2, inputRate, outputRate);
    writePosition.store (0, std::memory_order_relaxed);
    readPosition.store (0, std::memory_order_relaxed);
    underruns.store (0, std::memory_order_relaxed);
    overruns.store (0, std::memory_order_relaxed);
    observedOverruns = 0;
    resetConsumer();
    latencyMs = 1000.0 * outputLatency / outputRate + getTargetMs();
}

void MonitorOutput::prepareForTest (double producerRate, int producerPeriod, double consumerRate, int consumerPeriod)
{
    stop();
    jassert (producerRate > 0.0 && consumerRate > 0.0 && producerPeriod > 0 && consumerPeriod > 0);
    prepare (producerRate, producerPeriod, consumerRate, consumerPeriod, 0);
    running.store (true, std::memory_order_release);
}

void MonitorOutput::push (const float* left, const float* right, int frames) noexcept
{
    if (frames <= 0 || capacity == 0) return;
    const auto write = writePosition.load (std::memory_order_relaxed);
    const auto read = readPosition.load (std::memory_order_acquire);
    if ((std::uint64_t) frames > capacity - (write - read))
    {
        overruns.fetch_add (1, std::memory_order_release);
        return; // never overwrite PCM the consumer might be reading
    }
    for (int i = 0; i < frames; ++i)
    {
        const auto index = (size_t) ((write + (std::uint64_t) i) & (capacity - 1)) * 2;
        fifo[index]     = left  != nullptr ? left[i]  : 0.0f;
        fifo[index + 1] = right != nullptr ? right[i] : 0.0f;
    }
    writePosition.store (write + (std::uint64_t) frames, std::memory_order_release);
}

double MonitorOutput::getFillMs() const noexcept
{
    const auto read = readPosition.load (std::memory_order_acquire);
    const auto write = writePosition.load (std::memory_order_acquire);
    return 1000.0 * (double) juce::jmin (capacity, write >= read ? write - read : 0) / inputRate;
}

void MonitorOutput::resetConsumer() noexcept
{
    prefill = true;
    lm_asrc_reset (asrc);
    lm_asrc_set_correction_ppm (asrc, 0.0);
    lm_drift_init (&drift, target, outputRate / outputPeriod);
}

void MonitorOutput::pull (float* output, int frames) noexcept
{
    if (frames <= 0) return;
    std::fill_n (output, (size_t) frames * 2, 0.0f);
    if (asrc == nullptr || ! running.load (std::memory_order_acquire)) return;
    const auto over = overruns.load (std::memory_order_acquire);
    const auto write = writePosition.load (std::memory_order_acquire);
    auto read = readPosition.load (std::memory_order_relaxed);
    if (over != observedOverruns)
    {
        observedOverruns = over;
        readPosition.store (write, std::memory_order_release);
        resetConsumer();
        return;
    }
    const double fill = (double) (write - read);
    if (prefill)
    {
        if (fill < target) return;
        prefill = false;
    }
    lm_asrc_set_correction_ppm (asrc, lm_drift_update (&drift, fill, frames / outputRate));
    int made = 0;
    while (made < frames)
    {
        const auto index = read & (capacity - 1);
        const int available = (int) juce::jmin (write - read, capacity - index);
        int used = 0;
        const int produced = lm_asrc_process (asrc, fifo.data() + (size_t) index * 2, available, &used,
                                              output + (size_t) made * 2, frames - made);
        read += (std::uint64_t) used;
        made += produced;
        if (used == 0 && produced == 0) break;
    }
    if (made < frames)
    {
        underruns.fetch_add (1, std::memory_order_relaxed);
        // Drop the partial block and filter history once. Prefill emits silence without counting again.
        std::fill_n (output, (size_t) frames * 2, 0.0f);
        read = write;
        resetConsumer();
    }
    readPosition.store (read, std::memory_order_release);
}

void MonitorOutput::audioDeviceAboutToStart (juce::AudioIODevice* starting)
{
    // A driver format change requires a message-thread restart to rebuild the filter and storage.
    running.store (starting->isOpen() && starting->getCurrentSampleRate() == outputRate
                   && starting->getCurrentBufferSizeSamples() == outputPeriod, std::memory_order_release);
}

void MonitorOutput::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int channels,
                                                      int frames, const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals;
    for (int ch = 0; ch < channels; ++ch)
        if (outputs[ch] != nullptr) juce::FloatVectorOperations::clear (outputs[ch], frames);
    for (int offset = 0; offset < frames; offset += outputPeriod)
    {
        const int count = juce::jmin (outputPeriod, frames - offset);
        pull (outputScratch.data(), count);
        for (int ch = 0; ch < juce::jmin (2, channels); ++ch)
            if (outputs[ch] != nullptr)
                for (int i = 0; i < count; ++i)
                    outputs[ch][offset + i] = outputScratch[(size_t) i * 2 + (size_t) ch];
    }
}
} // namespace gocue::livemix
