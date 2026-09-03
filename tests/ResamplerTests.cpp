#include "audio/HighQualityResampler.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace gocue::tests
{

/** A sine at a fixed rate, endless. */
struct SineSource : public juce::AudioSource
{
    SineSource (double rate, double hz, float amp) : sampleRate (rate), frequency (hz), amplitude (amp) {}

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        for (int i = 0; i < info.numSamples; ++i)
        {
            const float v = amplitude * (float) std::sin (phase);
            phase += juce::MathConstants<double>::twoPi * frequency / sampleRate;

            for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                info.buffer->setSample (ch, info.startSample + i, v);
        }
    }

    double sampleRate, frequency, phase = 0.0;
    float amplitude;
};

class ResamplerTests : public juce::UnitTest
{
public:
    ResamplerTests() : juce::UnitTest ("HighQualityResampler", "GoCue") {}

    static int zeroCrossings (const juce::AudioBuffer<float>& b, int from, int to)
    {
        int n = 0;
        const float* s = b.getReadPointer (0);

        for (int i = from + 1; i < to; ++i)
            if ((s[i - 1] < 0.0f) != (s[i] < 0.0f))
                ++n;

        return n;
    }

    void runTest() override
    {
        beginTest ("48 k -> 44.1 k keeps the pitch and the level");
        {
            SineSource sine (48000.0, 1000.0, 0.5f);
            HighQualityResampler r (sine, 2, 48000.0);
            r.setDeviceRate (44100.0);
            r.prepareToPlay (512, 44100.0);
            expect (! r.isBypassed());

            juce::AudioBuffer<float> out (2, 44100 * 2);
            out.clear();

            for (int pos = 0; pos < out.getNumSamples(); pos += 480)   // odd block size: the FIFO must cope
            {
                juce::AudioSourceChannelInfo info (&out, pos, juce::jmin (480, out.getNumSamples() - pos));
                r.getNextAudioBlock (info);
            }

            // skip the first half second (filter warm-up), measure one full second
            const int from = 22050, to = from + 44100;
            const int crossings = zeroCrossings (out, from, to);
            expect (std::abs (crossings - 2000) <= 3, "zero crossings " + juce::String (crossings));

            const float rms = out.getRMSLevel (0, from, to - from);
            expectWithinAbsoluteError (rms, 0.5f / std::sqrt (2.0f), 0.01f);

            // both channels identical (per-channel converters run the same path)
            float maxDiff = 0.0f;
            for (int i = from; i < to; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (out.getSample (0, i) - out.getSample (1, i)));
            expect (maxDiff < 1.0e-6f);
        }

        beginTest ("44.1 k -> 48 k as well");
        {
            SineSource sine (44100.0, 440.0, 0.25f);
            HighQualityResampler r (sine, 1, 44100.0);
            r.setDeviceRate (48000.0);
            r.prepareToPlay (256, 48000.0);
            juce::AudioBuffer<float> out (1, 48000 * 2);
            out.clear();

            for (int pos = 0; pos < out.getNumSamples(); pos += 256)
            {
                juce::AudioSourceChannelInfo info (&out, pos, juce::jmin (256, out.getNumSamples() - pos));
                r.getNextAudioBlock (info);
            }

            const int from = 24000, to = from + 48000;
            const int crossings = zeroCrossings (out, from, to);
            expect (std::abs (crossings - 880) <= 3, "zero crossings " + juce::String (crossings));
            expectWithinAbsoluteError (out.getRMSLevel (0, from, to - from), 0.25f / std::sqrt (2.0f), 0.005f);
        }

        beginTest ("a large request at a steep ratio is filled completely and without clicks");
        {
            SineSource sine (96000.0, 2000.0, 0.5f);
            HighQualityResampler r (sine, 2, 96000.0);
            r.setDeviceRate (44100.0);
            r.prepareToPlay (8192, 44100.0);

            juce::AudioBuffer<float> out (2, 8192 * 3);
            out.clear();

            for (int pos = 0; pos < out.getNumSamples(); pos += 8192)   // 8192 output samples need ~17.8 k input = 35 chunks
            {
                juce::AudioSourceChannelInfo info (&out, pos, 8192);
                r.getNextAudioBlock (info);
            }

            // the last block is full level to its very end (no silent tail from an exhausted loop)
            expectWithinAbsoluteError (out.getRMSLevel (0, out.getNumSamples() - 512, 512), 0.5f / std::sqrt (2.0f), 0.01f);

            // continuity: a 2 kHz sine at 44.1 k moves at most ~0.14 per sample; a click would jump further
            float maxStep = 0.0f;
            for (int i = 4096; i < out.getNumSamples(); ++i)
                maxStep = juce::jmax (maxStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
            expectLessThan (maxStep, 0.16f);
        }

        beginTest ("equal rates pass straight through");
        {
            SineSource sine (48000.0, 1000.0, 0.5f);
            HighQualityResampler r (sine, 1, 48000.0);
            r.setDeviceRate (48000.0);
            r.prepareToPlay (512, 48000.0);
            expect (r.isBypassed());

            juce::AudioBuffer<float> out (1, 512);
            juce::AudioSourceChannelInfo info (&out, 0, 512);
            r.getNextAudioBlock (info);
            expectWithinAbsoluteError (out.getSample (0, 1), 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 / 48000.0), 1.0e-5f);
        }
    }
};

static ResamplerTests resamplerTests;

} // namespace gocue::tests
