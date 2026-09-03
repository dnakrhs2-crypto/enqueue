#include "audio/HighQualityResampler.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#pragma warning (push, 0)
#include "CDSPResampler.h"   // r8brain-free-src (MIT), header-only
#pragma warning (pop)

#include <algorithm>
#include <cmath>

namespace gocue
{

namespace
{
    constexpr int inputChunk = 512;          // upstream pull per conversion call
    constexpr double transitionBand = 2.0;   // % of the spectrum: flat to ~21 kHz at 44.1 k
    constexpr double attenuation = 180.0;    // dB: 24-bit clean
}

HighQualityResampler::HighQualityResampler (juce::AudioSource& u, int channels, double rate)
    : upstream (u), numChannels (juce::jmax (1, channels)), sourceRate (rate > 0.0 ? rate : 44100.0)
{
}

HighQualityResampler::~HighQualityResampler() = default;

void HighQualityResampler::setDeviceRate (double rate)
{
    deviceRate = rate > 0.0 ? rate : sourceRate;
    bypass = juce::approximatelyEqual (deviceRate, sourceRate);
    converters.clear();
    fifoRead = fifoFilled = 0;

    if (bypass)
        return;

    inChunk = inputChunk;
    const double ratio = deviceRate / sourceRate;
    const int maxOut = (int) std::ceil (inChunk * std::max (1.0, ratio)) * 2 + 1024;   // a burst after the latency never overflows

    for (int ch = 0; ch < numChannels; ++ch)
        converters.push_back (std::make_unique<r8b::CDSPResampler> (sourceRate, deviceRate, inChunk, transitionBand, attenuation, r8b::fprLinearPhase));

    inBuffer.setSize (numChannels, inChunk, false, true, true);
    inDouble.assign ((size_t) numChannels, std::vector<double> ((size_t) inChunk, 0.0));
    fifo.assign ((size_t) numChannels, std::vector<double> ((size_t) maxOut, 0.0));
}

void HighQualityResampler::prepareToPlay (int samplesPerBlockExpected, double)
{
    // the upstream runs at the file rate; it gets a block sized for the input chunk
    upstream.prepareToPlay (bypass ? juce::jmax (1, samplesPerBlockExpected) : inChunk, sourceRate);
    reset();
}

void HighQualityResampler::releaseResources()
{
    upstream.releaseResources();
}

void HighQualityResampler::reset() noexcept
{
    for (auto& c : converters)
        c->clear();

    fifoRead = fifoFilled = 0;
}

void HighQualityResampler::pullChunk()
{
    juce::AudioSourceChannelInfo in (&inBuffer, 0, inChunk);
    in.clearActiveBufferRegion();
    upstream.getNextAudioBlock (in);

    int produced = 0;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = inBuffer.getReadPointer (juce::jmin (ch, inBuffer.getNumChannels() - 1));
        auto& d = inDouble[(size_t) ch];

        for (int i = 0; i < inChunk; ++i)
            d[(size_t) i] = (double) src[i];

        double* out = nullptr;
        const int n = converters[(size_t) ch]->process (d.data(), inChunk, out);   // the same count for every channel
        auto& f = fifo[(size_t) ch];
        const int fit = juce::jmin (n, (int) f.size());

        if (fit > 0)
            std::copy (out, out + fit, f.begin());

        produced = fit;
    }

    fifoRead = 0;
    fifoFilled = produced;
}

void HighQualityResampler::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    if (bypass || converters.empty())
    {
        upstream.getNextAudioBlock (info);
        return;
    }

    info.clearActiveBufferRegion();
    int written = 0;

    for (int guard = 0; written < info.numSamples && guard < 64; ++guard)
    {
        if (fifoFilled == 0)
        {
            pullChunk();   // the first calls after a reset yield nothing until the filter has enough history
            continue;
        }

        const int take = juce::jmin (info.numSamples - written, fifoFilled);

        for (int ch = 0; ch < juce::jmin (numChannels, info.buffer->getNumChannels()); ++ch)
        {
            float* dest = info.buffer->getWritePointer (ch, info.startSample + written);
            const auto& f = fifo[(size_t) ch];

            for (int i = 0; i < take; ++i)
                dest[i] = (float) f[(size_t) (fifoRead + i)];
        }

        fifoRead += take;
        fifoFilled -= take;
        written += take;
    }
}

} // namespace gocue
