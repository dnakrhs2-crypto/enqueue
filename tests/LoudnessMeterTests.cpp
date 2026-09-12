#include "audio/LoudnessMeter.h"
#include "MixEngine.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

namespace gocue::tests
{

using namespace gocue::livemix;

/** The LUFS meter against the standard's numbers: the 48 kHz K-weighting table, EBU Tech 3341 / 3342 test signals
    (a 1 kHz stereo sine at -23 dBFS reads -23.0 LUFS, the relative gate drops the quiet parts, LRA of two levels
    20 s each), true peak between samples, the reset, the fifo, and the tap in the engine. */
class LoudnessMeterTests : public juce::UnitTest
{
public:
    LoudnessMeterTests() : juce::UnitTest ("LiveMix loudness meter", "LiveMix") {}

    static constexpr double fs = 48000.0;

    /** Feeds a stereo sine of 'seconds' at 'dbfs' (peak, each channel; a channel at -inf stays silent). */
    static void feedSine (LoudnessMeter& meter, double seconds, double leftDbfs, double rightDbfs, double hz = 1000.0, double phase = 0.0)
    {
        const int total = (int) std::llround (seconds * fs);
        const double al = leftDbfs > -200.0 ? std::pow (10.0, leftDbfs / 20.0) : 0.0;
        const double ar = rightDbfs > -200.0 ? std::pow (10.0, rightDbfs / 20.0) : 0.0;
        constexpr int block = 480;
        std::vector<float> l ((size_t) block), r ((size_t) block);

        for (int done = 0, n = 0; done < total; done += block)
        {
            const int count = juce::jmin (block, total - done);

            for (int i = 0; i < count; ++i, ++n)
            {
                const double s = std::sin (juce::MathConstants<double>::twoPi * hz * n / fs + phase);
                l[(size_t) i] = (float) (al * s);
                r[(size_t) i] = (float) (ar * s);
            }

            meter.process (l.data(), r.data(), count);
            meter.poll();
        }
    }

    /** The summed channel power that reads exactly 'lufs' without any weighting (the stats fed directly). */
    static double powerFor (double lufs) { return std::pow (10.0, (lufs + 0.691) / 10.0); }

    void runTest() override
    {
        beginTest ("the K-weighting at 48 kHz is the standard's table");
        {
            const auto s = KWeightingFilter::shelf (48000.0);
            expectWithinAbsoluteError (s.b0, 1.53512485958697, 1e-9);
            expectWithinAbsoluteError (s.b1, -2.69169618940638, 1e-9);
            expectWithinAbsoluteError (s.b2, 1.19839281085285, 1e-9);
            expectWithinAbsoluteError (s.a1, -1.69065929318241, 1e-9);
            expectWithinAbsoluteError (s.a2, 0.73248077421585, 1e-9);
            const auto h = KWeightingFilter::highPass (48000.0);
            expectWithinAbsoluteError (h.b0, 1.0, 1e-12);
            expectWithinAbsoluteError (h.b1, -2.0, 1e-12);
            expectWithinAbsoluteError (h.b2, 1.0, 1e-12);
            expectWithinAbsoluteError (h.a1, -1.99004745483398, 1e-9);
            expectWithinAbsoluteError (h.a2, 0.99007225036621, 1e-9);
        }

        beginTest ("a 1 kHz stereo sine at -23 dBFS reads -23.0 LUFS (EBU Tech 3341 case 1) in M, S and I");
        {
            LoudnessMeter meter;
            meter.prepare (fs);
            feedSine (meter, 20.0, -23.0, -23.0);
            const auto& st = meter.getStats();
            expect (st.momentary().valid);
            expectWithinAbsoluteError (st.momentary().value, -23.0, 0.1);
            expect (st.shortTerm().valid);
            expectWithinAbsoluteError (st.shortTerm().value, -23.0, 0.1);
            expect (st.integrated().valid);
            expectWithinAbsoluteError (st.integrated().value, -23.0, 0.1);
            expectWithinAbsoluteError (st.maxMomentary().value, -23.0, 0.1);
            expectWithinAbsoluteError (st.maxShortTerm().value, -23.0, 0.1);
            expectWithinAbsoluteError (st.elapsedSeconds(), 20.0, 0.11);
            expect (meter.truePeakMax().valid);
            expectWithinAbsoluteError (meter.truePeakMax().value, -23.0, 0.1);
            expectEquals (meter.getDroppedSubBlocks(), 0);
            expectWithinAbsoluteError (st.loudnessRange().value, 0.0, 0.2);   // one level: no range
        }

        beginTest ("one channel only reads 3 dB less: -23 dBFS on the left alone is -26.0 LUFS, a mono block (no right) the same");
        {
            LoudnessMeter meter;
            meter.prepare (fs);
            feedSine (meter, 10.0, -23.0, -300.0);
            expectWithinAbsoluteError (meter.getStats().integrated().value, -26.0, 0.1);

            LoudnessMeter mono;
            mono.prepare (fs);
            const int total = (int) (10.0 * fs);
            std::vector<float> l (480);

            for (int done = 0, n = 0; done < total; done += 480)
            {
                for (int i = 0; i < 480; ++i, ++n)
                    l[(size_t) i] = (float) (std::pow (10.0, -23.0 / 20.0) * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * n / fs));

                mono.process (l.data(), nullptr, 480);
                mono.poll();
            }

            expectWithinAbsoluteError (mono.getStats().integrated().value, -26.0, 0.1);   // not a copy on both sides (-23)
            expectWithinAbsoluteError (mono.truePeakMax().value, -23.0, 0.1);
        }

        beginTest ("the relative gate: quiet parts 13 LU under the loud part do not pull the integrated value down (Tech 3341 case 3)");
        {
            LoudnessMeter meter;
            meter.prepare (fs);
            feedSine (meter, 10.0, -36.0, -36.0);
            feedSine (meter, 20.0, -23.0, -23.0);
            feedSine (meter, 10.0, -36.0, -36.0);
            expectWithinAbsoluteError (meter.getStats().integrated().value, -23.0, 0.1);
            expectWithinAbsoluteError (meter.getStats().shortTerm().value, -36.0, 0.1);   // the last 3 s
        }

        beginTest ("the absolute gate: a signal under -70 LUFS, and silence, read as nothing");
        {
            LoudnessMeter meter;
            meter.prepare (fs);
            feedSine (meter, 5.0, -80.0, -80.0);
            expect (! meter.getStats().integrated().valid);
            expect (! meter.getStats().loudnessRange().valid);
            expect (meter.getStats().momentary().valid);   // the momentary value is shown even below the gate
            expectWithinAbsoluteError (meter.getStats().momentary().value, -80.0, 0.2);

            LoudnessMeter silent;
            silent.prepare (fs);
            feedSine (silent, 5.0, -300.0, -300.0);
            expect (! silent.getStats().momentary().valid);
            expect (! silent.getStats().shortTerm().valid);
            expect (! silent.getStats().integrated().valid);
            expect (! silent.truePeakMax().valid);
            expectWithinAbsoluteError (silent.getStats().elapsedSeconds(), 5.0, 0.11);
        }

        beginTest ("the reset: the numbers start over, the elapsed time too, a sub-block from before is dropped");
        {
            LoudnessMeter meter;
            meter.prepare (fs);
            feedSine (meter, 5.0, -10.0, -10.0);
            expectWithinAbsoluteError (meter.getStats().integrated().value, -10.0, 0.1);
            meter.reset();
            expect (! meter.getStats().integrated().valid);
            expect (! meter.truePeakMax().valid);
            expectEquals ((int) meter.getStats().getSubBlockCount(), 0);
            feedSine (meter, 5.0, -30.0, -30.0);
            expectWithinAbsoluteError (meter.getStats().integrated().value, -30.0, 0.1);   // not a mix with the -10 part
            expectWithinAbsoluteError (meter.getStats().maxMomentary().value, -30.0, 0.1);
            expectWithinAbsoluteError (meter.truePeakMax().value, -30.0, 0.1);
            expectWithinAbsoluteError (meter.getStats().elapsedSeconds(), 5.0, 0.11);

            // an impulse right after a reset is not lost (the samples count from the first one; only the interpolation waits)
            meter.reset();
            std::vector<float> l (480, 0.0f), r (480, 0.0f);
            l[3] = 0.5f;
            meter.process (l.data(), r.data(), 480);

            for (int i = 0; i < 12; ++i)
                meter.process (r.data(), r.data(), 480);   // silence to the end of the sub-block

            meter.poll();
            expect (meter.truePeakMax().valid);
            expectWithinAbsoluteError (meter.truePeakMax().value, -6.02, 0.05);
        }

        beginTest ("loudness range: 20 s at -20 then 20 s at -30 is 10 LU (Tech 3342), one level is 0");
        {
            LoudnessStats stats;

            for (int i = 0; i < 200; ++i)
                stats.addSubBlock (powerFor (-20.0) * 0.5, powerFor (-20.0) * 0.5);

            expectWithinAbsoluteError (stats.shortTerm().value, -20.0, 0.01);
            expectWithinAbsoluteError (stats.momentary().value, -20.0, 0.01);
            expectWithinAbsoluteError (stats.integrated().value, -20.0, 0.01);
            expectWithinAbsoluteError (stats.loudnessRange().value, 0.0, 0.11);

            for (int i = 0; i < 200; ++i)
                stats.addSubBlock (powerFor (-30.0) * 0.5, powerFor (-30.0) * 0.5);

            expectWithinAbsoluteError (stats.loudnessRange().value, 10.0, 1.0);
            expectWithinAbsoluteError (stats.integrated().value, -22.6, 0.3);   // the power mean of the two halves (10 log10 ((0.01 + 0.001) / 2)), both inside the gate
            expectWithinAbsoluteError (stats.maxShortTerm().value, -20.0, 0.01);
            expectWithinAbsoluteError (stats.elapsedSeconds(), 40.0, 0.001);
        }

        beginTest ("the range's relative gate: a stretch 25 LU under the rest is left out of the range");
        {
            LoudnessStats stats;

            for (int i = 0; i < 300; ++i)
                stats.addSubBlock (powerFor (-20.0) * 0.5, powerFor (-20.0) * 0.5);

            for (int i = 0; i < 100; ++i)
                stats.addSubBlock (powerFor (-45.0) * 0.5, powerFor (-45.0) * 0.5);

            expectWithinAbsoluteError (stats.loudnessRange().value, 0.0, 1.0);   // -45 is 25 LU under the mean: gated
        }

        beginTest ("true peak: a sine whose peaks fall between the samples is read at its real level");
        {
            // fs/4 with a 45 degree offset: every sample sits at 0.707 of the peak (-3 dB), the peak is between them
            LoudnessMeter meter;
            meter.prepare (fs);
            feedSine (meter, 1.0, -6.0, -6.0, fs / 4.0, juce::MathConstants<double>::pi / 4.0);
            expectWithinAbsoluteError (meter.truePeakMax().value, -6.0, 0.25);

            LoudnessMeter low;
            low.prepare (fs);
            feedSine (low, 1.0, -6.0, -6.0, 1000.0);
            expectWithinAbsoluteError (low.truePeakMax().value, -6.0, 0.1);

            TruePeakDetector detector;
            float peak = 0.0f;

            for (int n = 0; n < 4800; ++n)
                peak = juce::jmax (peak, detector.process ((float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 12000.0 * n / fs + juce::MathConstants<double>::pi / 4.0))));

            expectWithinAbsoluteError ((double) peak, 0.5, 0.015);
            expect (juce::isPositiveAndBelow (detector.getTaps()[0][0] * 0.0 + 1.0, 2.0));   // the taps exist (a compile-time check of the accessor)
        }

        beginTest ("the fifo: sub-blocks pile up while nobody polls and are dropped past its size, without a crash");
        {
            LoudnessMeter meter;
            meter.prepare (fs);
            const int block = 4800;
            std::vector<float> l ((size_t) block, 0.1f), r ((size_t) block, 0.1f);

            for (int i = 0; i < 1100; ++i)   // 110 s of 100 ms sub-blocks, no poll: 1023 fit (one slot is the fifo's guard), the rest are dropped
                meter.process (l.data(), r.data(), block);

            expect (meter.getDroppedSubBlocks() > 0);
            meter.poll();
            expect (meter.getStats().getSubBlockCount() >= 1000);
            expect (meter.getStats().integrated().valid);
        }

        beginTest ("the engine taps the master output after its chain: a mic into the master reads on the meter");
        {
            MixEngine engine;
            engine.prepare (fs, 480);
            MixSession s;
            s.addChannel ("A");   // input 0, to the master (mono: both sides)
            juce::StringArray errors;
            engine.applySession (s, &errors, true);
            expectEquals (errors.size(), 0);

            juce::AudioBuffer<float> in (2, 480), out (2, 480);
            const double amp = std::pow (10.0, -20.0 / 20.0);
            auto& meter = engine.getLoudnessMeter();

            for (int b = 0, n = 0; b < 500; ++b)   // 5 s
            {
                for (int i = 0; i < 480; ++i, ++n)
                    in.setSample (0, i, (float) (amp * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * n / fs)));

                engine.renderBlock (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 2, 480);
                meter.poll();
            }

            expect (meter.getStats().integrated().valid);
            expectWithinAbsoluteError (meter.getStats().integrated().value, -20.0, 0.15);   // the mono mic on both sides: like a stereo sine at -20 dBFS
            expectWithinAbsoluteError (meter.truePeakMax().value, -20.0, 0.1);

            engine.setChannelOn (s.channels[0].id, false);

            for (int b = 0; b < 400; ++b)   // 4 s of OFF: the momentary value falls away, the integrated stays (gated)
            {
                engine.renderBlock (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 2, 480);
                meter.poll();
            }

            expect (! meter.getStats().momentary().valid);
            expectWithinAbsoluteError (meter.getStats().integrated().value, -20.0, 0.15);
        }
    }
};

static LoudnessMeterTests loudnessMeterTests;

} // namespace gocue::tests
