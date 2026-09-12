#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace gocue::livemix
{

/** A loudness reading: 'valid' is false while there is nothing to say (silence, too early, everything gated). */
struct LoudnessValue
{
    bool valid = false;
    double value = 0.0;   // LUFS / LU / dBTP
};

/** The K-weighting of ITU-R BS.1770-4: a high shelf (+4 dB above ~1.7 kHz) then the RLB high-pass (~38 Hz), each a
    biquad; the coefficients are derived for any sample rate (the standard tabulates 48 kHz only). One per channel. */
class KWeightingFilter
{
public:
    struct Coefficients { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

    static Coefficients shelf (double sampleRate);
    static Coefficients highPass (double sampleRate);

    void prepare (double sampleRate);
    void reset() noexcept;

    double process (double x) noexcept
    {
        // two biquads, direct form II transposed
        const double y1 = s1.b0 * x + z1a;
        z1a = s1.b1 * x - s1.a1 * y1 + z2a;
        z2a = s1.b2 * x - s1.a2 * y1;
        const double y2 = s2.b0 * y1 + z1b;
        z1b = s2.b1 * y1 - s2.a1 * y2 + z2b;
        z2b = s2.b2 * y1 - s2.a2 * y2;
        return y2;
    }

private:
    Coefficients s1, s2;
    double z1a = 0.0, z2a = 0.0, z1b = 0.0, z2b = 0.0;
};

/** The true peak of BS.1770-4 Annex 2: the signal 4x oversampled (a polyphase windowed-sinc interpolator, 24 taps a
    phase) and the largest absolute value of the samples and the points between them. One per channel. */
class TruePeakDetector
{
public:
    static constexpr int oversampling = 4;
    static constexpr int tapsPerPhase = 24;

    void reset() noexcept;
    /** The peak this sample and its interpolated neighbours reach (absolute, linear). */
    float process (float x) noexcept;

    static const std::array<std::array<double, tapsPerPhase>, oversampling>& getTaps();

private:
    std::array<double, tapsPerPhase> history {};
    int pos = 0;
};

/** The numbers, from 100 ms sub-blocks of K-weighted mean square (one value a channel): momentary (400 ms),
    short-term (3 s), integrated (gated: absolute -70 LUFS, relative -10 LU, from 400 ms blocks with 75% overlap),
    loudness range (EBU Tech 3342: gated short-term values, 95th minus 10th percentile), the maxima, the elapsed time.
    The integrated and range distributions are histograms (0.1 LU bins, the powers summed exactly), so an hours-long
    stream costs nothing. Message thread. */
class LoudnessStats
{
public:
    static constexpr double subBlockSeconds = 0.1;
    static constexpr int momentaryBlocks = 4;    // 400 ms
    static constexpr int shortTermBlocks = 30;   // 3 s
    static constexpr int historyBlocks = 600;    // 60 s, shared with Enqueue's selectable average
    static constexpr double absoluteGate = -70.0;
    static constexpr double relativeGateIntegrated = -10.0;
    static constexpr double relativeGateRange = -20.0;
    static constexpr double shownFloor = -100.0;   // M and S (ungated) read as nothing below this

    LoudnessStats();

    void reset();
    /** A finished 100 ms sub-block: the mean square of the K-weighted left and right channels. */
    void addSubBlock (double meanSquareLeft, double meanSquareRight);

    LoudnessValue momentary() const;
    LoudnessValue shortTerm() const;
    /** Recent seconds (up to 60): mean channel-sum power of 100 ms blocks strictly above -70 LUFS. */
    LoudnessValue windowed (double seconds) const;
    LoudnessValue integrated() const;
    LoudnessValue loudnessRange() const;
    LoudnessValue maxMomentary() const noexcept { return maxM; }
    LoudnessValue maxShortTerm() const noexcept { return maxS; }
    juce::int64 getSubBlockCount() const noexcept { return subBlocks; }
    double elapsedSeconds() const noexcept { return (double) subBlocks * subBlockSeconds; }

    /** -0.691 + 10 log10 (the summed channel powers) - the loudness of a mean square; silence has none. */
    static LoudnessValue loudnessOf (double summedPower) noexcept;

private:
    struct Histogram
    {
        static constexpr double minLufs = -70.0, maxLufs = 10.0, binSize = 0.1;
        static constexpr int numBins = 801;

        std::vector<int> counts;
        std::vector<double> power;
        juce::int64 total = 0;
        double powerSum = 0.0;

        Histogram();
        void clear();
        void add (double lufs, double summedPower);
        static int binFor (double lufs) noexcept;
        static double lowerEdge (int bin) noexcept { return minLufs + bin * binSize; }
        static double centre (int bin) noexcept { return minLufs + (bin + 0.5) * binSize; }
        /** The loudness of the bin's own mean power (what its blocks really were - the end bins gather what lies
            beyond the range); far below everything when the bin is empty. */
        double binLoudness (int bin) const noexcept;
    };

    double windowPower (int blocks) const noexcept;   // the mean summed power of the last 'blocks' sub-blocks

    std::array<double, (size_t) historyBlocks> ring {};
    int ringPos = 0, filled = 0;
    juce::int64 subBlocks = 0;
    Histogram integratedHist, rangeHist;
    LoudnessValue maxM, maxS;
};

/** The master output's loudness meter: process() on the audio callback (K-weighting, true peak, 100 ms sub-blocks
    into a lock-free fifo), poll() on the message thread (the sub-blocks into the stats), reset() from the UI. */
class LoudnessMeter
{
public:
    LoudnessMeter();

    /** The sample rate (and a fresh filter state); the callback's context. The numbers so far are kept. */
    void prepare (double sampleRate);
    /** Audio thread: a block of the stereo master output. 'right' may be null (mono). */
    void process (const float* left, const float* right, int numSamples) noexcept;
    /** Message thread: the numbers start over (the audio side drops the sub-block it is on). */
    void reset();
    /** Message thread: the finished sub-blocks go into the stats. Cheap; call it from a timer. */
    void poll();

    const LoudnessStats& getStats() const noexcept { return stats; }
    /** dBTP, the largest since the reset. */
    LoudnessValue truePeakMax() const noexcept;
    /** Sub-blocks the fifo could not take (the message thread stalled for over a minute): a gap in the numbers. */
    int getDroppedSubBlocks() const noexcept { return dropped.load (std::memory_order_relaxed); }

private:
    struct SubBlock
    {
        double meanSquareLeft = 0.0, meanSquareRight = 0.0;
        unsigned generation = 0;
    };

    static constexpr int fifoSize = 1024;   // 102 s of sub-blocks

    /** The true peak and the generation it belongs to in one word: a reset writes the new generation with a zero
        peak, and the audio thread publishes a peak only into its own generation - a peak from before the reset can
        neither land after it nor erase one after it. */
    static juce::uint64 packPeak (unsigned gen, float peak) noexcept
    {
        juce::uint32 bits;
        std::memcpy (&bits, &peak, sizeof (bits));
        return ((juce::uint64) gen << 32) | bits;
    }

    static float unpackPeak (juce::uint64 word) noexcept
    {
        const auto bits = (juce::uint32) (word & 0xffffffffu);
        float peak;
        std::memcpy (&peak, &bits, sizeof (peak));
        return peak;
    }

    static unsigned generationOf (juce::uint64 word) noexcept { return (unsigned) (word >> 32); }

    juce::AbstractFifo fifo { fifoSize };
    std::vector<SubBlock> fifoData;
    KWeightingFilter kLeft, kRight;
    TruePeakDetector tpLeft, tpRight;
    double sumLeft = 0.0, sumRight = 0.0;
    int count = 0, samplesPerSubBlock = 4800;
    float blockPeak = 0.0f;
    int truePeakWarmup = 0;   // samples after a reset whose interpolation still mixes in the sound before it
    unsigned audioGeneration = 0;

    std::atomic<unsigned> generation { 0 };
    std::atomic<juce::uint64> truePeakWord { 0 };   // packPeak (generation, peak)
    std::atomic<int> dropped { 0 };
    LoudnessStats stats;
};

} // namespace gocue::livemix
