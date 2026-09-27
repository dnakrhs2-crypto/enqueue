#include "MonitorOutput.h"

#include <algorithm>
#include <cmath>

namespace gocue::livemix
{
static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
static_assert (std::atomic<bool>::is_always_lock_free);
static_assert (std::atomic<double>::is_always_lock_free && std::atomic<int>::is_always_lock_free);

MonitorOutput::~MonitorOutput() { stop(); }

juce::String MonitorOutput::start (juce::AudioIODeviceType& type, const juce::String& outputName,
                                  double producerRate, int producerPeriod, int preferredOutputPeriod, double preferredOutputRate)
{
    stop();
    inputRate = producerRate;
    inputPeriod = producerPeriod;
    restartRequested.store (false, std::memory_order_release);
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
    restartRequested.store (false, std::memory_order_release); // an owner stop/failure is not a driver restart request
}

void MonitorOutput::prepare (double producerRate, int producerPeriod, double consumerRate, int consumerPeriod, int outputLatency)
{
    inputRate = producerRate;
    inputPeriod = producerPeriod;
    outputRate = consumerRate;
    outputPeriod = consumerPeriod;
    restartOutputRate.store (consumerRate, std::memory_order_relaxed);
    restartOutputPeriod.store (consumerPeriod, std::memory_order_relaxed);
    restartRequested.store (false, std::memory_order_release);
    fadeFrames = juce::jmax (1, (int) (0.005 * outputRate));
    fadeOutRemaining = 0;
    lastOutput = tail = {};
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
    fillBias = 0.0;
    lm_drift_init (&drift, target, outputRate / outputPeriod);
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
    // A discontinuity changes fill, not either device's clock. Preserve ppm, integral and acquisition age.
    drift.lp = (drift.ppm - drift.integ) / drift.kp;
    drift.primed = 1;
    reseatingFill = drift.elapsed > 0.0;
    reseatSum = reseatSeconds = 0.0;
    fadeInRemaining = fadeFrames;
}

void MonitorOutput::releaseTail (float* output, int offset, int frames) noexcept
{
    for (int i = offset; i < frames && fadeOutRemaining > 0; ++i)
    {
        const float gain = (float) --fadeOutRemaining / (float) fadeFrames;
        for (int ch = 0; ch < 2; ++ch) output[(size_t) i * 2 + ch] = tail[(size_t) ch] * gain;
    }
    for (int ch = 0; ch < 2; ++ch) lastOutput[(size_t) ch] = output[(size_t) (frames - 1) * 2 + ch];
}

void MonitorOutput::fadeOut (float* output, int made, int frames) noexcept
{
    const int start = juce::jmax (0, made - fadeFrames);
    for (int ch = 0; ch < 2; ++ch)
        tail[(size_t) ch] = made > 0 ? output[(size_t) (made - 1) * 2 + ch] : lastOutput[(size_t) ch];
    fadeOutRemaining = fadeFrames;
    for (int i = start; i < made; ++i)
    {
        const float gain = (float) --fadeOutRemaining / (float) fadeFrames;
        for (int ch = 0; ch < 2; ++ch) output[(size_t) i * 2 + ch] *= gain;
    }
    // Too few produced samples: continue the release from their last value, including across short callbacks.
    releaseTail (output, made, frames);
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
        fadeOut (output, 0, frames);
        return;
    }
    double fill = (double) (write - read);
    if (prefill)
    {
        if (fill < target || fadeOutRemaining > 0)
        {
            releaseTail (output, 0, frames);
            return;
        }
        // Re-seat to the same measurement point instead of feeding a whole producer-period overshoot into PI.
        read = write - (std::uint64_t) std::ceil (target);
        fill = (double) (write - read);
        prefill = false;
    }
    const double dt = frames / outputRate;
    if (reseatingFill)
    {
        // A refill moves the FIFO's operating point by a fraction of a producer period. Measure that offset
        // using the learned clock before resuming PI, so the controller does not pitch-bend to undo a seek.
        reseatSum += fill * dt;
        reseatSeconds += dt;
        drift.elapsed += dt;
        if (reseatSeconds >= 2.0) // two controller time constants average the producer's block phase
        {
            fillBias = reseatSum / reseatSeconds - target - drift.lp;
            reseatingFill = false;
        }
    }
    else
        lm_drift_update (&drift, fill - fillBias, dt);
    lm_asrc_set_correction_ppm (asrc, drift.ppm);
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
    for (int i = 0; i < made && fadeInRemaining > 0; ++i)
    {
        const float ramp = 1.0f - (float) --fadeInRemaining / (float) fadeFrames;
        const float gain = ramp * ramp; // quiet the freshly reset sinc history before the audible part of the fade
        for (int ch = 0; ch < 2; ++ch) output[(size_t) i * 2 + ch] *= gain;
    }
    if (made < frames)
    {
        underruns.fetch_add (1, std::memory_order_relaxed);
        fadeOut (output, made, frames);
        read = write;
        resetConsumer();
    }
    for (int ch = 0; ch < 2; ++ch) lastOutput[(size_t) ch] = output[(size_t) (frames - 1) * 2 + ch];
    readPosition.store (read, std::memory_order_release);
}

void MonitorOutput::audioDeviceAboutToStart (juce::AudioIODevice* starting)
{
    // A driver format change requires a message-thread restart to rebuild the filter and storage.
    restartOutputRate.store (starting->getCurrentSampleRate(), std::memory_order_relaxed);
    restartOutputPeriod.store (starting->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
    if (! starting->isOpen() || starting->getCurrentSampleRate() != outputRate
        || starting->getCurrentBufferSizeSamples() != outputPeriod)
        requestRestart();
    else
        running.store (! needsRestart(), std::memory_order_release);
}

void MonitorOutput::requestRestart() noexcept
{
    running.store (false, std::memory_order_release);
    restartRequested.store (true, std::memory_order_release);
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
