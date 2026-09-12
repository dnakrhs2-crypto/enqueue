#include "LoudnessMeter.h"

#include <algorithm>

namespace gocue::livemix
{

//==============================================================================
KWeightingFilter::Coefficients KWeightingFilter::shelf (double sampleRate)
{
    // the analogue prototype of the standard's 48 kHz table (f0, gain, Q from BS.1770-4 / libebur128), bilinear
    const double f0 = 1681.974450955533, gainDb = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
    const double Vh = std::pow (10.0, gainDb / 20.0);
    const double Vb = std::pow (Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;

    Coefficients c;
    c.b0 = (Vh + Vb * K / Q + K * K) / a0;
    c.b1 = 2.0 * (K * K - Vh) / a0;
    c.b2 = (Vh - Vb * K / Q + K * K) / a0;
    c.a1 = 2.0 * (K * K - 1.0) / a0;
    c.a2 = (1.0 - K / Q + K * K) / a0;
    return c;
}

KWeightingFilter::Coefficients KWeightingFilter::highPass (double sampleRate)
{
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
    const double a0 = 1.0 + K / Q + K * K;

    Coefficients c;
    c.b0 = 1.0;   // the standard's table: 1, -2, 1 over the normalised denominator
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (K * K - 1.0) / a0;
    c.a2 = (1.0 - K / Q + K * K) / a0;
    return c;
}

void KWeightingFilter::prepare (double sampleRate)
{
    s1 = shelf (juce::jmax (8000.0, sampleRate));
    s2 = highPass (juce::jmax (8000.0, sampleRate));
    reset();
}

void KWeightingFilter::reset() noexcept
{
    z1a = z2a = z1b = z2b = 0.0;
}

//==============================================================================
const std::array<std::array<double, TruePeakDetector::tapsPerPhase>, TruePeakDetector::oversampling>& TruePeakDetector::getTaps()
{
    static const auto taps = []
    {
        // a Kaiser-windowed sinc low-pass at the original Nyquist in the 4x domain, split into its four phases (each a
        // fractional delay with unity gain at DC); beta 6: flat to ~0.8 of Nyquist, ~ -60 dB images
        constexpr int length = tapsPerPhase * oversampling;
        constexpr double beta = 6.0, cutoff = 0.5 / oversampling;
        const double centre = (length - 1) / 2.0;

        auto besselI0 = [] (double x)
        {
            double sum = 1.0, term = 1.0;

            for (int k = 1; k < 200; ++k)
            {
                term *= (x * 0.5) / k;
                const double t2 = term * term;
                sum += t2;

                if (t2 < sum * 1e-15)
                    break;
            }

            return sum;
        };

        std::array<double, (size_t) length> h {};
        const double windowNorm = besselI0 (beta);

        for (int n = 0; n < length; ++n)
        {
            const double t = (n - centre);
            const double x = 2.0 * cutoff * t;
            const double sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
            const double r = t / centre;
            const double window = besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / windowNorm;
            h[(size_t) n] = 2.0 * cutoff * sinc * window * oversampling;
        }

        std::array<std::array<double, tapsPerPhase>, oversampling> phases {};

        for (int p = 0; p < oversampling; ++p)
        {
            double sum = 0.0;

            for (int k = 0; k < tapsPerPhase; ++k)
            {
                phases[(size_t) p][(size_t) k] = h[(size_t) (k * oversampling + p)];
                sum += phases[(size_t) p][(size_t) k];
            }

            for (auto& tap : phases[(size_t) p])
                tap /= sum;
        }

        return phases;
    }();

    return taps;
}

void TruePeakDetector::reset() noexcept
{
    history.fill (0.0);
    pos = 0;
}

float TruePeakDetector::process (float x) noexcept
{
    const auto& taps = getTaps();
    history[(size_t) pos] = x;
    double peak = std::abs ((double) x);   // the samples themselves are on the curve too

    for (int p = 0; p < oversampling; ++p)
    {
        double acc = 0.0;
        int idx = pos;

        for (int k = 0; k < tapsPerPhase; ++k)
        {
            acc += taps[(size_t) p][(size_t) k] * history[(size_t) idx];

            if (--idx < 0)
                idx = tapsPerPhase - 1;
        }

        peak = juce::jmax (peak, std::abs (acc));
    }

    if (++pos >= tapsPerPhase)
        pos = 0;

    return (float) peak;
}

//==============================================================================
LoudnessStats::Histogram::Histogram()
{
    counts.assign ((size_t) numBins, 0);
    power.assign ((size_t) numBins, 0.0);
}

void LoudnessStats::Histogram::clear()
{
    std::fill (counts.begin(), counts.end(), 0);
    std::fill (power.begin(), power.end(), 0.0);
    total = 0;
    powerSum = 0.0;
}

int LoudnessStats::Histogram::binFor (double lufs) noexcept
{
    return juce::jlimit (0, numBins - 1, (int) std::floor ((lufs - minLufs) / binSize));
}

void LoudnessStats::Histogram::add (double lufs, double summedPower)
{
    const auto bin = (size_t) binFor (lufs);
    ++counts[bin];
    power[bin] += summedPower;
    ++total;
    powerSum += summedPower;
}

double LoudnessStats::Histogram::binLoudness (int bin) const noexcept
{
    const auto c = counts[(size_t) bin];

    if (c == 0)
        return -1000.0;   // nothing there: under every gate

    const auto l = loudnessOf (power[(size_t) bin] / (double) c);
    return l.valid ? l.value : -1000.0;
}

LoudnessStats::LoudnessStats()
{
    reset();
}

void LoudnessStats::reset()
{
    ring.fill (0.0);
    ringPos = filled = 0;
    subBlocks = 0;
    integratedHist.clear();
    rangeHist.clear();
    maxM = maxS = {};
}

LoudnessValue LoudnessStats::loudnessOf (double summedPower) noexcept
{
    if (! (summedPower > 1e-12))   // -120 LUFS and below (and a NaN): nothing
        return {};

    return { true, -0.691 + 10.0 * std::log10 (summedPower) };
}

double LoudnessStats::windowPower (int blocks) const noexcept
{
    double sum = 0.0;
    int idx = ringPos;

    for (int i = 0; i < blocks; ++i)
    {
        if (--idx < 0)
            idx = historyBlocks - 1;

        sum += ring[(size_t) idx];
    }

    return sum / blocks;
}

void LoudnessStats::addSubBlock (double meanSquareLeft, double meanSquareRight)
{
    if (! std::isfinite (meanSquareLeft)) meanSquareLeft = 0.0;
    if (! std::isfinite (meanSquareRight)) meanSquareRight = 0.0;

    ring[(size_t) ringPos] = juce::jmax (0.0, meanSquareLeft) + juce::jmax (0.0, meanSquareRight);   // both channels weigh 1 (L / R)
    ringPos = (ringPos + 1) % historyBlocks;
    filled = juce::jmin (historyBlocks, filled + 1);
    ++subBlocks;

    if (filled >= momentaryBlocks)
    {
        const double p = windowPower (momentaryBlocks);
        const auto m = loudnessOf (p);

        if (m.valid && m.value > absoluteGate)   // the absolute gate (strict, as the standard has it): into the integrated distribution
            integratedHist.add (m.value, p);

        if (m.valid && m.value >= shownFloor && (! maxM.valid || m.value > maxM.value))   // the maximum of what is shown: ungated
            maxM = m;
    }

    if (filled >= shortTermBlocks)
    {
        const double p = windowPower (shortTermBlocks);
        const auto s = loudnessOf (p);

        if (s.valid && s.value > absoluteGate)
            rangeHist.add (s.value, p);

        if (s.valid && s.value >= shownFloor && (! maxS.valid || s.value > maxS.value))
            maxS = s;
    }
}

LoudnessValue LoudnessStats::momentary() const
{
    if (filled < momentaryBlocks)
        return {};

    const auto m = loudnessOf (windowPower (momentaryBlocks));
    return m.valid && m.value >= shownFloor ? m : LoudnessValue {};   // -100 and below reads as nothing
}

LoudnessValue LoudnessStats::shortTerm() const
{
    if (filled < shortTermBlocks)
        return {};

    const auto s = loudnessOf (windowPower (shortTermBlocks));
    return s.valid && s.value >= shownFloor ? s : LoudnessValue {};
}

LoudnessValue LoudnessStats::windowed (double seconds) const
{
    if (! std::isfinite (seconds) || seconds <= 0.0)
        return {};

    const int blocks = juce::jmin (filled, (int) std::llround (juce::jmin (60.0, seconds) / subBlockSeconds));
    // Compare powers to avoid logarithms for every sub-block on each UI tick.
    static const double gatePower = std::pow (10.0, (absoluteGate + 0.691) / 10.0);
    double sum = 0.0;
    int accepted = 0, idx = ringPos;

    for (int i = 0; i < blocks; ++i)
    {
        if (--idx < 0)
            idx = historyBlocks - 1;

        const double power = ring[(size_t) idx];
        if (power > gatePower)
        {
            sum += power;
            ++accepted;
        }
    }

    return accepted > 0 ? loudnessOf (sum / accepted) : LoudnessValue {};
}

LoudnessValue LoudnessStats::integrated() const
{
    if (integratedHist.total == 0)
        return {};

    // the relative gate: 10 LU under the loudness of the blocks over the absolute gate (all of the histogram)
    const auto ungated = loudnessOf (integratedHist.powerSum / (double) integratedHist.total);

    if (! ungated.valid)
        return {};

    const double threshold = ungated.value + relativeGateIntegrated;
    juce::int64 n = 0;
    double p = 0.0;

    for (int bin = 0; bin < Histogram::numBins; ++bin)
        if (integratedHist.binLoudness (bin) >= threshold)   // occupied bins by the loudness of their own mean power (the end bins gather what is beyond the range)
        {
            n += integratedHist.counts[(size_t) bin];
            p += integratedHist.power[(size_t) bin];
        }

    if (n == 0)
        return {};

    return loudnessOf (p / (double) n);
}

LoudnessValue LoudnessStats::loudnessRange() const
{
    if (rangeHist.total == 0)
        return {};

    const auto ungated = loudnessOf (rangeHist.powerSum / (double) rangeHist.total);

    if (! ungated.valid)
        return {};

    const double threshold = ungated.value + relativeGateRange;
    juce::int64 n = 0;

    for (int bin = 0; bin < Histogram::numBins; ++bin)
        if (rangeHist.binLoudness (bin) >= threshold)
            n += rangeHist.counts[(size_t) bin];

    if (n == 0)
        return {};

    // the 10th and 95th percentiles of the gated distribution: the values at the zero-based ranks round ((n - 1) p)
    // of the sorted values (EBU Tech 3342's reference), read off the cumulative histogram (bin centres)
    const auto rankLow = (juce::int64) std::llround ((double) (n - 1) * 0.10);
    const auto rankHigh = (juce::int64) std::llround ((double) (n - 1) * 0.95);
    juce::int64 seen = 0;
    double p10 = 0.0, p95 = 0.0;
    bool have10 = false, have95 = false;

    for (int bin = 0; bin < Histogram::numBins && ! have95; ++bin)
    {
        if (rangeHist.counts[(size_t) bin] == 0 || rangeHist.binLoudness (bin) < threshold)
            continue;

        seen += rangeHist.counts[(size_t) bin];

        if (! have10 && seen > rankLow)
        {
            p10 = Histogram::centre (bin);
            have10 = true;
        }

        if (! have95 && seen > rankHigh)
        {
            p95 = Histogram::centre (bin);
            have95 = true;
        }
    }

    return { true, juce::jmax (0.0, p95 - p10) };
}

//==============================================================================
LoudnessMeter::LoudnessMeter()
{
    fifoData.resize ((size_t) fifoSize);
    TruePeakDetector::getTaps();   // built here, on the message thread: never inside the first audio callback
    prepare (48000.0);
}

void LoudnessMeter::prepare (double sampleRate)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    kLeft.prepare (sr);
    kRight.prepare (sr);
    tpLeft.reset();
    tpRight.reset();
    samplesPerSubBlock = juce::jmax (1, (int) std::llround (sr * LoudnessStats::subBlockSeconds));
    sumLeft = sumRight = 0.0;
    count = 0;
    blockPeak = 0.0f;
}

void LoudnessMeter::process (const float* left, const float* right, int numSamples) noexcept
{
    if (left == nullptr || numSamples <= 0)
        return;

    const unsigned gen = generation.load (std::memory_order_acquire);

    if (gen != audioGeneration)
    {
        // a reset: the sub-block being built is dropped (it belongs to the numbers before the reset)
        audioGeneration = gen;
        sumLeft = sumRight = 0.0;
        count = 0;
        blockPeak = 0.0f;
        truePeakWarmup = TruePeakDetector::tapsPerPhase;   // the interpolators' memory is of the sound before the reset: not this measurement's peak
    }

    const bool stereo = right != nullptr;   // one channel: the other is absent, not a copy (a copy would read 3 LU high)

    for (int i = 0; i < numSamples; ++i)
    {
        float l = left[i];
        float r = stereo ? right[i] : 0.0f;

        if (! std::isfinite (l)) l = 0.0f;   // a NaN would poison every number until the reset
        if (! std::isfinite (r)) r = 0.0f;

        const double kl = kLeft.process (l);
        sumLeft += kl * kl;
        float interpolated = tpLeft.process (l);
        float raw = std::abs (l);

        if (stereo)
        {
            const double kr = kRight.process (r);
            sumRight += kr * kr;
            interpolated = juce::jmax (interpolated, tpRight.process (r));
            raw = juce::jmax (raw, std::abs (r));
        }

        // right after a reset the interpolators still hold the sound before it: the samples themselves count, the
        // points between them not until that memory has passed through
        if (truePeakWarmup > 0)
        {
            --truePeakWarmup;
            blockPeak = juce::jmax (blockPeak, raw);
        }
        else
        {
            blockPeak = juce::jmax (blockPeak, interpolated);
        }

        if (++count >= samplesPerSubBlock)
        {
            int start1, size1, start2, size2;
            fifo.prepareToWrite (1, start1, size1, start2, size2);

            if (size1 > 0)
            {
                fifoData[(size_t) start1] = { sumLeft / count, sumRight / count, audioGeneration };
                fifo.finishedWrite (1);
            }
            else
            {
                dropped.fetch_add (1, std::memory_order_relaxed);
            }

            // the peak goes into its own generation only: a reset that has begun (the word already carries the next
            // generation) or finished makes this block's peak a thing of the old measurement
            auto cur = truePeakWord.load (std::memory_order_relaxed);

            while (generationOf (cur) == audioGeneration && blockPeak > unpackPeak (cur)
                   && ! truePeakWord.compare_exchange_weak (cur, packPeak (audioGeneration, blockPeak), std::memory_order_relaxed)) {}

            sumLeft = sumRight = 0.0;
            count = 0;
            blockPeak = 0.0f;
        }
    }
}

void LoudnessMeter::reset()
{
    // the peak word takes the next generation first: from here a block of the old generation cannot publish its peak;
    // then the generation itself, which the audio thread follows at its next block
    const unsigned next = generation.load (std::memory_order_acquire) + 1;
    truePeakWord.store (packPeak (next, 0.0f), std::memory_order_release);
    generation.store (next, std::memory_order_release);
    dropped.store (0, std::memory_order_relaxed);
    stats.reset();
}

void LoudnessMeter::poll()
{
    const unsigned gen = generation.load (std::memory_order_acquire);
    int start1, size1, start2, size2;
    fifo.prepareToRead (fifo.getNumReady(), start1, size1, start2, size2);

    auto take = [this, gen] (int start, int size)
    {
        for (int i = 0; i < size; ++i)
        {
            const auto& sb = fifoData[(size_t) (start + i)];

            if (sb.generation == gen)   // a sub-block from before a reset is skipped
                stats.addSubBlock (sb.meanSquareLeft, sb.meanSquareRight);
        }
    };

    take (start1, size1);
    take (start2, size2);
    fifo.finishedRead (size1 + size2);
}

LoudnessValue LoudnessMeter::truePeakMax() const noexcept
{
    const auto word = truePeakWord.load (std::memory_order_acquire);
    const float p = generationOf (word) == generation.load (std::memory_order_acquire) ? unpackPeak (word) : 0.0f;

    if (! (p > 1e-7f))
        return {};

    return { true, 20.0 * std::log10 ((double) p) };
}

} // namespace gocue::livemix
