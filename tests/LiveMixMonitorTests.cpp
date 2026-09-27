#include "MonitorOutput.h"

#include <juce_core/juce_core.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

namespace gocue::livemix
{
struct MonitorOutputTestAccess
{
    static lm_drift state (const MonitorOutput& monitor) { return monitor.drift; }
};
}

namespace gocue::tests
{
using namespace gocue::livemix;

class LiveMixMonitorTests : public juce::UnitTest
{
public:
    LiveMixMonitorTests() : juce::UnitTest ("LiveMix split monitor", "LiveMix") {}

    void runTest() override
    {
        for (double ppm : { -300.0, 300.0 })
        {
            beginTest ("30 minute audio soak, consumer clock " + juce::String (ppm) + " ppm");
            MonitorOutput monitor;
            constexpr int producerPeriod = 96, consumerPeriod = 240;
            monitor.prepareForTest (48000.0, producerPeriod, 48000.0, consumerPeriod);
            expectWithinAbsoluteError (monitor.getTargetMs(), 15.0, 1.0e-9);
            expectWithinAbsoluteError (monitor.getLatencyMs(), 15.0, 1.0e-9);
            std::array<float, producerPeriod> input;
            std::array<float, consumerPeriod * 2> output;
            for (int i = 0; i < producerPeriod; ++i)
                input[(size_t) i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * i / 48.0);

            double nextInput = 0.0, minFill = 1.0e9, maxFill = 0.0, power = 0.0, stereoError = 0.0;
            juce::uint64 samples = 0, underAt60 = 0, overAt60 = 0;
            const double consumerRate = 48000.0 * (1.0 + ppm * 1.0e-6);
            const int callbacks = (int) (30.0 * 60.0 * consumerRate / consumerPeriod);
            for (int b = 0; b < callbacks; ++b)
            {
                const double time = (double) b * consumerPeriod / consumerRate;
                while (nextInput <= time + 1.0e-10)
                {
                    monitor.push (input.data(), input.data(), producerPeriod);
                    nextInput += (double) producerPeriod / 48000.0;
                }
                if (time < 60.0)
                {
                    underAt60 = monitor.getUnderruns();
                    overAt60 = monitor.getOverruns();
                }
                else
                {
                    minFill = juce::jmin (minFill, monitor.getFillMs());
                    maxFill = juce::jmax (maxFill, monitor.getFillMs());
                }
                monitor.pullForTest (output.data(), consumerPeriod);
                if (time >= 60.0)
                    for (int i = 0; i < consumerPeriod; ++i)
                    {
                        const double l = output[(size_t) i * 2], r = output[(size_t) i * 2 + 1];
                        power += l * l;
                        stereoError = juce::jmax (stereoError, std::abs (l - r));
                        ++samples;
                    }
            }
            expect (monitor.getUnderruns() == underAt60);
            expect (monitor.getOverruns() == overAt60);
            const double gainDb = 10.0 * std::log10 (power / (double) samples / 0.125);
            expectWithinAbsoluteError (gainDb, 0.0, 0.05);
            expectLessOrEqual (maxFill, monitor.getTargetMs() + 3.0);
            expectGreaterOrEqual (minFill, monitor.getTargetMs() - 3.0);
            expectWithinAbsoluteError (stereoError, 0.0, 1.0e-9);
            logMessage ("monitor " + juce::String (ppm) + " ppm: fill=" + juce::String (minFill, 3) + ".."
                        + juce::String (maxFill, 3) + " ms, level=" + juce::String (gainDb, 6)
                        + " dB, under=" + juce::String (monitor.getUnderruns()) + ", over=" + juce::String (monitor.getOverruns()));
        }

        beginTest ("200 ms producer stall counts once, clears stale audio, then prefills and recovers");
        {
            MonitorOutput monitor;
            monitor.prepareForTest (48000.0, 240, 48000.0, 240);
            std::array<float, 240> input;
            std::array<float, 480> output;
            input.fill (0.25f);
            for (int b = 0; b < 200; ++b)
            {
                monitor.push (input.data(), input.data(), 240);
                monitor.pullForTest (output.data(), 240);
            }
            expectWithinAbsoluteError (output.back(), 0.25f, 1.0e-5f);
            for (int b = 0; b < 40; ++b)
            {
                monitor.pullForTest (output.data(), 240);
                if (b > 5) // the partial block and at most 5 ms of release are retained
                    expect (std::all_of (output.begin(), output.end(), [] (float x) { return x == 0.0f; }));
            }
            expect (monitor.getUnderruns() == 1);
            // Fresh silence must stay exactly silent, proving that neither queued PCM nor sinc history repeats.
            input.fill (0.0f);
            for (int b = 0; b < 20; ++b)
            {
                monitor.push (input.data(), input.data(), 240);
                monitor.pullForTest (output.data(), 240);
                expect (std::all_of (output.begin(), output.end(), [] (float x) { return x == 0.0f; }));
            }
            input.fill (-0.125f);
            bool sawRecovery = false;
            for (int b = 0; b < 200; ++b)
            {
                monitor.push (input.data(), input.data(), 240);
                monitor.pullForTest (output.data(), 240);
                if (! sawRecovery && std::abs (output.back()) > 0.01f)
                {
                    sawRecovery = true;
                }
            }
            expect (sawRecovery);
            expectWithinAbsoluteError (output.back(), -0.125f, 1.0e-5f);
            expect (monitor.getUnderruns() == 1);
            expect (monitor.getOverruns() == 0);
        }

        beginTest ("200 ms stall preserves learned clock and has smooth release and recovery edges");
        for (double truePpm : { -300.0, 300.0 })
        {
            MonitorOutput monitor;
            constexpr int period = 240;
            constexpr int producerPeriod = 96;
            monitor.prepareForTest (48000.0, producerPeriod, 48000.0, period);
            std::array<float, producerPeriod> input;
            std::array<float, period * 2> output;
            const double producerRate = 48000.0 * (1.0 + truePpm * 1.0e-6);
            double nextInput = 0.0, maxStep = 0.0, maxPpmDeviation = 0.0;
            float previous = 0.0f;
            int sample = 0;
            lm_drift before {};
            for (int b = 0; b < 13200; ++b)
            {
                const double time = b * 0.005;
                while (nextInput <= time + 1.0e-10)
                {
                    for (auto& x : input)
                        x = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 137.0 * sample++ / 48000.0);
                    if (nextInput < 60.0 || nextInput >= 60.2)
                        monitor.push (input.data(), input.data(), producerPeriod);
                    nextInput += producerPeriod / producerRate;
                }
                if (b == 12000) before = MonitorOutputTestAccess::state (monitor);
                monitor.pullForTest (output.data(), period);
                for (int i = 0; i < period; ++i)
                {
                    const auto value = output[(size_t) i * 2];
                    if (time >= 60.0) maxStep = std::max (maxStep, (double) std::abs (value - previous));
                    previous = value;
                }
                if (time >= 60.0)
                    maxPpmDeviation = std::max (maxPpmDeviation, std::abs (MonitorOutputTestAccess::state (monitor).ppm - before.ppm));
            }
            expectWithinAbsoluteError (before.ppm, truePpm, 50.0);
            expectLessOrEqual (maxPpmDeviation, 50.0);
            expectLessOrEqual (maxStep, 2.0 * 0.5 * std::sin (juce::MathConstants<double>::pi * 137.0 / 48000.0) + 0.01);
            expect (monitor.getUnderruns() == 1 && monitor.getOverruns() == 0);
            const auto recovered = MonitorOutputTestAccess::state (monitor);
            logMessage ("stall " + juce::String (truePpm) + " ppm: before=" + juce::String (before.ppm, 3)
                        + ", recovered=" + juce::String (recovered.ppm, 3) + ", max change=" + juce::String (maxPpmDeviation, 3)
                        + ", max sample step=" + juce::String (maxStep, 6));
            expectGreaterOrEqual (recovered.elapsed, before.elapsed);
            // Overflow re-prefill preserves the same three learned quantities exactly.
            for (int b = 0; b < 200; ++b) monitor.push (input.data(), input.data(), producerPeriod);
            monitor.pullForTest (output.data(), period);
            const auto overflow = MonitorOutputTestAccess::state (monitor);
            expectWithinAbsoluteError (overflow.ppm, recovered.ppm, 1.0e-9);
            expectWithinAbsoluteError (overflow.integ, recovered.integ, 1.0e-9);
            expectWithinAbsoluteError (overflow.elapsed, recovered.elapsed, 1.0e-9);
            for (int b = 0; b < 8; ++b) monitor.push (input.data(), input.data(), producerPeriod);
            monitor.pullForTest (output.data(), period);
            const auto refilled = MonitorOutputTestAccess::state (monitor);
            expectWithinAbsoluteError (refilled.ppm, recovered.ppm, 1.0e-9);
            expectWithinAbsoluteError (refilled.integ, recovered.integ, 1.0e-9);
            expectGreaterThan (refilled.elapsed, recovered.elapsed);
            expect (std::any_of (output.begin(), output.end(), [] (float x) { return std::abs (x) > 0.01f; }));
            monitor.prepareForTest (44100.0, period, 48000.0, period);
            const auto restarted = MonitorOutputTestAccess::state (monitor);
            expect (restarted.ppm == 0.0 && restarted.integ == 0.0 && restarted.elapsed == 0.0);
        }

        beginTest ("a stopped consumer cannot block the producer; overflow discards stale audio on resume");
        {
            MonitorOutput monitor;
            monitor.prepareForTest (48000.0, 240, 48000.0, 240);
            std::array<float, 240> input;
            std::array<float, 480> output;
            input.fill (0.5f);
            const double started = juce::Time::getMillisecondCounterHiRes();
            for (int b = 0; b < 10000; ++b)
                monitor.push (input.data(), input.data(), 240);
            expectLessThan (juce::Time::getMillisecondCounterHiRes() - started, 2000.0);
            expect (monitor.getOverruns() > 0);
            expectGreaterOrEqual (monitor.getFillMs(), 250.0);
            monitor.pullForTest (output.data(), 240);
            expect (std::all_of (output.begin(), output.end(), [] (float x) { return x == 0.0f; }));
            input.fill (0.0f);
            for (int b = 0; b < 100; ++b)
            {
                monitor.push (input.data(), input.data(), 240);
                monitor.pullForTest (output.data(), 240);
                expect (std::all_of (output.begin(), output.end(), [] (float x) { return x == 0.0f; }));
            }
        }

        beginTest ("different nominal sample rates use the same ASRC consumer");
        for (const double outputRate : { 44100.0, 96000.0 })
        {
            MonitorOutput monitor;
            const int outPeriod = (int) (outputRate / 200.0);
            monitor.prepareForTest (48000.0, 240, outputRate, outPeriod);
            std::array<float, 240> input;
            std::vector<float> output ((size_t) outPeriod * 2);
            double nextInput = 0.0, power = 0.0;
            int sample = 0, count = 0;
            for (int b = 0; b < 4000; ++b)
            {
                const double time = b * outPeriod / outputRate;
                while (nextInput <= time)
                {
                    for (auto& x : input)
                        x = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * sample++ / 48.0);
                    monitor.push (input.data(), input.data(), 240);
                    nextInput += 0.005;
                }
                monitor.pullForTest (output.data(), outPeriod);
                if (time > 10.0)
                    for (int i = 0; i < outPeriod; ++i)
                    {
                        power += output[(size_t) i * 2] * output[(size_t) i * 2];
                        ++count;
                    }
            }
            expect (monitor.getUnderruns() == 0 && monitor.getOverruns() == 0);
            expectWithinAbsoluteError (10.0 * std::log10 (power / count / 0.125), 0.0, 0.05);
        }
    }
};

static LiveMixMonitorTests liveMixMonitorTests;
} // namespace gocue::tests
