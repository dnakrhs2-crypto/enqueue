#include "audio/MediaFoundationAudioFormat.h"

#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** Decodes tests/assets/sweep.m4a (0.5 s stereo sine sweep 200-1700 Hz at 0.5, AAC 160 kbps, made with ffmpeg) and
    compares it with sweep_ref.wav (the same file decoded by ffmpeg, which applies the AAC priming / edit list). */
class MediaFoundationTests : public juce::UnitTest
{
public:
    MediaFoundationTests() : juce::UnitTest ("MediaFoundation reader", "GoCue") {}

    static juce::File findAssetDir()
    {
        // the test binary lives in build/vs2022/tests/GoCueTests_artefacts/<config>/; walk up to the repo
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (int i = 0; i < 8; ++i)
        {
            const auto candidate = dir.getChildFile ("tests").getChildFile ("assets");

            if (candidate.getChildFile ("sweep.m4a").existsAsFile())
                return candidate;

            dir = dir.getParentDirectory();
        }

        return {};
    }

    /** Lag of 'b' relative to 'a' with the highest normalised correlation over 'n' samples (searched within +-range). */
    static int bestLag (const juce::AudioBuffer<float>& a, int aStart, const juce::AudioBuffer<float>& b, int bStart, int n, int range, float& corrOut)
    {
        int best = 0;
        corrOut = -1.0f;

        for (int lag = -range; lag <= range; ++lag)
        {
            if (aStart + lag < 0 || aStart + lag + n > a.getNumSamples() || bStart + n > b.getNumSamples())
                continue;

            double sab = 0.0, saa = 0.0, sbb = 0.0;

            for (int i = 0; i < n; ++i)
            {
                const double x = a.getSample (0, aStart + lag + i);
                const double y = b.getSample (0, bStart + i);
                sab += x * y;
                saa += x * x;
                sbb += y * y;
            }

            const float corr = saa > 0.0 && sbb > 0.0 ? (float) (sab / std::sqrt (saa * sbb)) : 0.0f;

            if (corr > corrOut)
            {
                corrOut = corr;
                best = lag;
            }
        }

        return best;
    }

    static float maxDiff (const juce::AudioBuffer<float>& a, int aStart, const juce::AudioBuffer<float>& b, int bStart, int n)
    {
        float d = 0.0f;

        for (int i = 0; i < n; ++i)
            d = juce::jmax (d, std::abs (a.getSample (0, aStart + i) - b.getSample (0, bStart + i)));

        return d;
    }

    void runTest() override
    {
        beginTest ("Media Foundation decodes an AAC file: rate, channels, length, content");
        {
            if (! MediaFoundationAudioFormat::isAvailable())
            {
                logMessage ("Media Foundation not available on this machine: skipped");
                return;
            }

            const auto assets = findAssetDir();

            if (assets == juce::File())
            {
                logMessage ("tests/assets/sweep.m4a not found: skipped");
                return;
            }

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            formats.registerFormat (new MediaFoundationAudioFormat(), false);

            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (assets.getChildFile ("sweep.m4a")));
            std::unique_ptr<juce::AudioFormatReader> reference (formats.createReaderFor (assets.getChildFile ("sweep_ref.wav")));
            expect (reader != nullptr);
            expect (reference != nullptr);

            if (reader == nullptr || reference == nullptr)
                return;

            expectWithinAbsoluteError (reader->sampleRate, 44100.0, 1.0);
            expectEquals ((int) reader->numChannels, 2);
            expect (reader->usesFloatingPointData);
            expect (std::abs ((double) reader->lengthInSamples - (double) reference->lengthInSamples) < 4500.0);   // AAC priming / padding
            logMessage ("length: MF " + juce::String (reader->lengthInSamples) + " vs ffmpeg " + juce::String (reference->lengthInSamples));

            juce::AudioBuffer<float> all (2, (int) reader->lengthInSamples);
            expect (reader->read (&all, 0, all.getNumSamples(), 0, true, true));
            juce::AudioBuffer<float> ref (2, (int) reference->lengthInSamples);
            expect (reference->read (&ref, 0, ref.getNumSamples(), 0, true, true));

            if (const auto dump = juce::SystemStats::getEnvironmentVariable ("GOCUE_MF_DUMP", {}); dump.isNotEmpty())
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> out (juce::File (dump).createOutputStream());
                auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (32));
                if (writer != nullptr) { writer->writeFromAudioSampleBuffer (all, 0, all.getNumSamples()); logMessage ("dumped " + dump); }
            }

            // the same level as ffmpeg (a 0.5 sine: rms 0.354)
            const int mid = juce::jmin (all.getNumSamples(), ref.getNumSamples()) / 2;
            expectWithinAbsoluteError (all.getRMSLevel (0, mid, 4000), ref.getRMSLevel (0, mid, 4000), 0.03f);
            expectWithinAbsoluteError (all.getRMSLevel (1, mid, 4000), ref.getRMSLevel (1, mid, 4000), 0.03f);

            // the same audio as ffmpeg's decode, sample-aligned (the decoder's priming frame is dropped)
            float corr = 0.0f;
            const int lagVsRef = bestLag (ref, mid, all, mid, 2000, 4000, corr);
            logMessage ("sequential decode vs ffmpeg: lag " + juce::String (lagVsRef) + " samples, correlation " + juce::String (corr, 4));
            expect (corr > 0.99f);
            expect (lagVsRef == 0);

            beginTest ("seeking lands on the same samples as the sequential decode");
            {
                juce::AudioBuffer<float> chunk (2, 4410);
                expect (reader->read (&chunk, 0, 4410, mid, true, true));

                const int lag = bestLag (all, mid, chunk, 0, 2000, 4000, corr);
                logMessage ("seek vs sequential: lag " + juce::String (lag) + " samples, correlation " + juce::String (corr, 4)
                            + ", max diff " + juce::String (maxDiff (all, mid, chunk, 0, 4000), 5));
                expect (lag == 0);
                expect (maxDiff (all, mid, chunk, 0, 4000) < 1.0e-3f);   // same decoder, same frames: sample-exact after the pre-roll

                // a backwards seek, then reading past the end fills with silence
                expect (reader->read (&chunk, 0, 4410, 1000, true, true));
                expect (bestLag (all, 1000, chunk, 0, 2000, 4000, corr) == 0);
                expect (maxDiff (all, 1000, chunk, 0, 4000) < 1.0e-3f);

                juce::AudioBuffer<float> tail (2, 2000);
                expect (reader->read (&tail, 0, 2000, reader->lengthInSamples - 1000, true, true));
                expectWithinAbsoluteError (tail.getRMSLevel (0, 1000, 1000), 0.0f, 1e-6f);
            }
        }
    }
};

static MediaFoundationTests mediaFoundationTests;

} // namespace gocue::tests
