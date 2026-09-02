#include "audio/StretchSource.h"

#if defined(_MSC_VER)
 #pragma warning (push)
 #pragma warning (disable: 4127 4244 4267 4305 4456 4457 4458 4459)
#endif
#include <signalsmith-stretch/signalsmith-stretch.h>
#if defined(_MSC_VER)
 #pragma warning (pop)
#endif

#include <cmath>

namespace gocue
{

struct StretchSource::Impl
{
    signalsmith::stretch::SignalsmithStretch<float> stretch;
};

StretchSource::StretchSource (juce::PositionableAudioSource& source, int numChannels, double fileRate)
    : upstream (source), impl (std::make_unique<Impl>()), channels (juce::jmax (1, numChannels)), fileSampleRate (fileRate > 0.0 ? fileRate : 44100.0)
{
    impl->stretch.presetDefault (channels, (float) fileSampleRate);
    inputPointers.resize ((size_t) channels, nullptr);
    outputPointers.resize ((size_t) channels, nullptr);
}

StretchSource::~StretchSource() = default;

int StretchSource::getPreRollSamples() const noexcept
{
    return impl->stretch.outputSeekLength ((float) maxRate) + 1;
}

void StretchSource::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    maxBlock = juce::jmax (64, samplesPerBlockExpected);
    const int capacity = (int) std::ceil (maxBlock * maxRate) + 64 + getPreRollSamples();
    inputBuffer.setSize (channels, capacity, false, false, true);
    spareOutput.setSize (channels, maxBlock, false, false, true);
    upstream.prepareToPlay (maxBlock, sampleRate);
    prepared = true;
    seekTo (position.load (std::memory_order_relaxed));
    pendingSeek.store (-1, std::memory_order_release);   // the seek just happened: the first block must not redo it
}

void StretchSource::releaseResources()
{
    upstream.releaseResources();
    prepared = false;
}

void StretchSource::setNextReadPosition (juce::int64 newPosition)
{
    position.store (newPosition, std::memory_order_relaxed);
    pendingSeek.store (newPosition, std::memory_order_release);   // the audio thread re-seeks before the next block
}

void StretchSource::pull (int numInputSamples)
{
    juce::AudioSourceChannelInfo info (&inputBuffer, 0, numInputSamples);
    upstream.getNextAudioBlock (info);
}

void StretchSource::seekTo (juce::int64 newPosition)
{
    if (! prepared)
        return;

    const double r = rate.load (std::memory_order_relaxed);
    const int preRoll = juce::jmin (inputBuffer.getNumSamples(), impl->stretch.outputSeekLength ((float) r) + 1);

    // the upstream delivers silence before 0, so a pre-roll from a negative position is fine
    upstream.setNextReadPosition (newPosition - preRoll);
    pull (preRoll);

    for (int c = 0; c < channels; ++c)
        inputPointers[(size_t) c] = inputBuffer.getWritePointer (c);

    impl->stretch.outputSeek (inputPointers.data(), preRoll);
    inputCarry = 0.0;
    position.store (newPosition, std::memory_order_relaxed);
}

void StretchSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    if (! prepared || info.buffer == nullptr || info.numSamples <= 0)
    {
        info.clearActiveBufferRegion();
        return;
    }

    if (const auto seek = pendingSeek.exchange (-1, std::memory_order_acq_rel); seek >= 0)
        seekTo (seek);

    int remaining = info.numSamples;
    int outOffset = info.startSample;

    while (remaining > 0)
    {
        const int n = juce::jmin (remaining, maxBlock);
        const double r = rate.load (std::memory_order_relaxed);
        const double wanted = n * r + inputCarry;
        int numInput = (int) std::floor (wanted);
        inputCarry = wanted - numInput;
        numInput = juce::jlimit (0, inputBuffer.getNumSamples(), numInput);

        pull (numInput);

        for (int c = 0; c < channels; ++c)
        {
            inputPointers[(size_t) c] = inputBuffer.getWritePointer (c);
            outputPointers[(size_t) c] = c < info.buffer->getNumChannels() ? info.buffer->getWritePointer (c, outOffset) : nullptr;
        }

        // channels the caller's buffer does not have are still produced (the stretcher needs every channel); park them in the input buffer tail
        bool spare = false;

        for (int c = 0; c < channels; ++c)
            if (outputPointers[(size_t) c] == nullptr)
                spare = true;

        if (spare)
        {
            for (int c = 0; c < channels; ++c)
                if (outputPointers[(size_t) c] == nullptr)
                    outputPointers[(size_t) c] = spareOutput.getWritePointer (c);   // never aliases the input the stretcher still reads

            impl->stretch.process (inputPointers.data(), numInput, outputPointers.data(), n);
        }
        else
        {
            impl->stretch.process (inputPointers.data(), numInput, outputPointers.data(), n);
        }

        position.fetch_add (numInput, std::memory_order_relaxed);
        outOffset += n;
        remaining -= n;
    }
}

} // namespace gocue
