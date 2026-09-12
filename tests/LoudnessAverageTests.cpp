#include "audio/AudioEngine.h"
#include "audio/LoudnessMeter.h"
#include "TestGainPlugin.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <limits>

namespace gocue::tests
{
using livemix::LoudnessMeter;
using livemix::LoudnessStats;

class LoudnessAverageTests : public juce::UnitTest
{
public:
    LoudnessAverageTests() : juce::UnitTest ("LoudnessAverage", "Enqueue") {}

    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 480;

    static double powerFor (double lufs) { return std::pow (10.0, (lufs + 0.691) / 10.0); }

    static void add (LoudnessStats& stats, int blocks, double lufs)
    {
        for (int i = 0; i < blocks; ++i)
            stats.addSubBlock (powerFor (lufs) * 0.5, powerFor (lufs) * 0.5);
    }

    static void sineBlock (float* samples, int startSample, double peakDb)
    {
        const double amplitude = std::pow (10.0, peakDb / 20.0);
        for (int i = 0; i < blockSize; ++i)
            samples[i] = (float) (amplitude * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (startSample + i) / sampleRate));
    }

    void runTest() override
    {
        beginTest ("fixed-amplitude 1 kHz stereo sine: all average windows read -23 LUFS");
        {
            LoudnessMeter meter;
            meter.prepare (sampleRate);
            expect (! meter.getStats().windowed (20).valid);
            std::array<float, blockSize> samples {};
            for (int block = 0; block < 2100; ++block)
            {
                sineBlock (samples.data(), block * blockSize, -23.0);
                meter.process (samples.data(), samples.data(), blockSize);
                meter.poll();
            }
            for (const int seconds : { 5, 10, 20, 30, 60 })
            {
                const auto average = meter.getStats().windowed (seconds);
                expect (average.valid);
                expectWithinAbsoluteError (average.value, -23.0, 0.5);
            }
        }

        beginTest ("silence and sub-blocks at or below the absolute gate are excluded");
        {
            LoudnessStats stats;
            for (int i = 0; i < 50; ++i)
                stats.addSubBlock (0.0, 0.0);
            expect (! stats.windowed (5).valid);
            add (stats, 50, -23.0);
            add (stats, 50, -70.0);
            add (stats, 50, -80.0);
            expectWithinAbsoluteError (stats.windowed (20).value, -23.0, 1e-8);
            expect (! stats.windowed (10).valid);
            expect (! stats.windowed (0).valid);
            expect (! stats.windowed (std::numeric_limits<double>::quiet_NaN()).valid);
        }

        beginTest ("changing the window uses recent power, including across the 60-second ring wrap");
        {
            LoudnessStats stats;
            add (stats, 550, -30.0);
            add (stats, 50, -10.0);
            expectWithinAbsoluteError (stats.windowed (5).value, -10.0, 1e-8);
            const auto tenSecondPower = (powerFor (-30.0) + powerFor (-10.0)) * 0.5;
            expectWithinAbsoluteError (stats.windowed (10).value, LoudnessStats::loudnessOf (tenSecondPower).value, 1e-8);
            expectWithinAbsoluteError (stats.windowed (60).value,
                                       LoudnessStats::loudnessOf ((55 * powerFor (-30.0) + 5 * powerFor (-10.0)) / 60).value, 1e-8);
            add (stats, 600, -20.0);
            expectWithinAbsoluteError (stats.windowed (60).value, -20.0, 1e-8);
            expectWithinAbsoluteError (stats.momentary().value, -20.0, 1e-8);
            expectWithinAbsoluteError (stats.shortTerm().value, -20.0, 1e-8);
            stats.reset();
            expect (! stats.windowed (60).valid);
        }

        beginTest ("AudioEngine offline render updates momentary after master inserts and the output gate");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            Cue mic;
            mic.type = CueType::mic;
            mic.mic.numInputs = 2;
            mic.gainDb = 0.0;
            juce::String error;
            expect (engine.play (mic, &error), error);
            juce::AudioBuffer<float> input (2, blockSize), output (2, blockSize);
            auto& meter = engine.getLoudnessMeter();
            expect (! meter.getStats().momentary().valid);
            for (int block = 0; block < 100; ++block)
            {
                sineBlock (input.getWritePointer (0), block * blockSize, -23.0);
                input.copyFrom (1, 0, input, 0, 0, blockSize);
                engine.renderBlock (output, blockSize, input.getArrayOfReadPointers(), 2);
                meter.poll();
            }
            expect (meter.getStats().momentary().valid);
            expectWithinAbsoluteError (meter.getStats().momentary().value, -29.02, 0.5);
            engine.stopAll();
            for (int block = 0; block < 100; ++block)
            {
                engine.renderBlock (output, blockSize);
                meter.poll();
            }
            expect (! meter.getStats().momentary().valid);
        }

        beginTest ("AudioEngine mono output meters only L, without summing the unused R or averaging channel powers");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize, 1);
            Cue mic;
            mic.type = CueType::mic;
            mic.mic.numInputs = 2;
            mic.gainDb = 0.0;
            juce::String error;
            expect (engine.play (mic, &error), error);
            juce::AudioBuffer<float> input (2, blockSize), output (1, blockSize);
            LoudnessMeter leftOnly;
            leftOnly.prepare (sampleRate);
            auto& meter = engine.getLoudnessMeter();
            // One channel needs twice the power of each channel in the -23 LUFS stereo reference above.
            const double monoPeakDb = -23.0 + 10.0 * std::log10 (2.0);
            for (int block = 0; block < 100; ++block)
            {
                sineBlock (input.getWritePointer (0), block * blockSize, monoPeakDb);
                sineBlock (input.getWritePointer (1), block * blockSize, -6.0);   // present in the mix, absent from the device output
                engine.renderBlock (output, blockSize, input.getArrayOfReadPointers(), 2);
                leftOnly.process (output.getReadPointer (0), nullptr, blockSize);
                leftOnly.poll();
                meter.poll();
            }
            const auto momentary = meter.getStats().momentary();
            const auto average = meter.getStats().windowed (20);
            expect (momentary.valid && average.valid && leftOnly.getStats().momentary().valid);
            expectWithinAbsoluteError (momentary.value, -23.0, 0.5);
            expectWithinAbsoluteError (average.value, -23.0, 0.5);
            expectWithinAbsoluteError (momentary.value, leftOnly.getStats().momentary().value, 1e-8);
            expectWithinAbsoluteError (average.value, leftOnly.getStats().windowed (20).value, 1e-8);
        }
    }
};

static LoudnessAverageTests loudnessAverageTests;
} // namespace gocue::tests
