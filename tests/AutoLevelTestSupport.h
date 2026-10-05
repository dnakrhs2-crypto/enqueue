#pragma once

#include "audio/AutoLeveler.h"
#include <cmath>
#include <functional>

namespace gocue::tests::auto_level
{
constexpr int rate = 48000, block = 480;

/** Independent ungated 3 s K-weighted reference meter, at 10 ms resolution. */
struct Meter
{
    Meter() { left.prepare (rate); right.prepare (rate); }
    void process (const juce::AudioBuffer<float>& b)
    {
        double sum = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double l = left.process (b.getSample (0, i));
            const double r = b.getNumChannels() > 1 ? right.process (b.getSample (1, i)) : 0.0;
            sum += l * l + r * r;
        }
        total += sum - energy[(size_t) pos];
        energy[(size_t) pos] = sum;
        pos = (pos + 1) % (int) energy.size();
        count = juce::jmin ((int) energy.size(), count + 1);
    }
    double lufs() const { return -0.691 + 10.0 * std::log10 (juce::jmax (1.0e-30, total / juce::jmax (1, count * block))); }
    livemix::KWeightingFilter left, right;
    std::array<double, 300> energy {};
    double total = 0.0;
    int pos = 0, count = 0;
};

/** Periodic multi-tone, calibrated using the leveler's K weighting, independent of the controller. */
struct Signal
{
    Signal()
    {
        for (int i = 0; i < rate; ++i)
        {
            const double t = juce::MathConstants<double>::twoPi * i / rate;
            wave[(size_t) i] = (float) (0.50 * std::sin (223 * t) + 0.29 * std::sin (701 * t + 0.7) + 0.17 * std::sin (1499 * t + 1.4));
        }
        livemix::KWeightingFilter filter;
        filter.prepare (rate);
        for (const auto x : wave) filter.process (x);
        double energy = 0;
        for (const auto x : wave) { const double y = filter.process (x); energy += 2.0 * y * y; }
        reference = -0.691 + 10.0 * std::log10 (energy / rate);
    }
    void fill (juce::AudioBuffer<float>& b, double lufs)
    {
        const double gain = lufs < -200 ? 0.0 : std::pow (10.0, (lufs - reference) / 20.0);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float x = (float) (wave[(size_t) (position++ % rate)] * gain);
            for (int ch = 0; ch < b.getNumChannels(); ++ch) b.setSample (ch, i, x);
        }
    }
    std::vector<float> wave = std::vector<float> ((size_t) rate);
    double reference = 0;
    juce::int64 position = 0;
};

struct Rig
{
    Rig() { leveler.prepare (rate, block, 2); leveler.setEnabled (true); }
    void step (double lufs)
    {
        signal.fill (buffer, lufs);
        input.process (buffer);
        leveler.process (buffer, block);
        output.process (buffer);
        gains.push_back (leveler.getGainDb());
        for (int ch = 0; ch < 2; ++ch) peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, block));
    }
    void feed (double seconds, double lufs) { for (int i = 0; i < (int) std::llround (seconds * 100); ++i) step (lufs); }
    AutoLeveler leveler;
    Signal signal;
    juce::AudioBuffer<float> buffer { 2, block };
    Meter input, output;
    std::vector<double> gains;
    float peak = 0;
};
} // namespace gocue::tests::auto_level
