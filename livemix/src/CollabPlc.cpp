#include "CollabPlc.h"
#include <algorithm>
#include <cmath>

namespace gocue::livemix::collab
{
namespace
{
// Mean removal rejects DC as a pitch and makes the confidence meaningful for
// quiet speech. All candidate lags compare the same reference window.
double correlation (const float* end, int count, int lag) noexcept
{
    double a = 0, b = 0, aa = 0, bb = 0, ab = 0;
    for (int i = -count; i < 0; ++i)
    {
        const double x = end[i], y = end[i - lag];
        a += x; b += y; aa += x * x; bb += y * y; ab += x * y;
    }
    aa -= a * a / count; bb -= b * b / count; ab -= a * b / count;
    const double minimum = count * CollabPlc::Tuning::minRms * CollabPlc::Tuning::minRms;
    return aa > minimum && bb > minimum ? ab / std::sqrt (aa * bb) : 0.0;
}
}

void CollabPlc::prepare (double rate)
{
    const auto frames = [rate] (double ms) { return std::max (1, (int) std::lround (rate * ms / 1000.0)); };
    minPitch = frames (Tuning::minPitchMs); maxPitch = frames (Tuning::maxPitchMs);
    correlationFrames = frames (Tuning::correlationMs);
    historyFrames = std::max (frames (Tuning::historyMs), maxPitch * 3 + maxPitch / 4 + 2);
    fullLevelFrames = frames (Tuning::fullLevelMs); stepFrames = frames (Tuning::attenuationStepMs);
    muteFrames = frames (Tuning::muteMs); fallbackFrames = frames (Tuning::fallbackMs);
    decimation = std::max (1, (int) std::lround (rate / Tuning::searchRate));
    history.resize ((size_t) historyFrames * 2); frozen.resize (history.size());
    mono.resize ((size_t) historyFrames); coarse.resize ((size_t) (historyFrames / decimation));
    coarseCorrelation.resize ((size_t) (maxPitch / decimation + 1));
    reset();
}

void CollabPlc::reset() noexcept
{
    written = available = frozenFrames = elapsed = recoveryFrames = recoveryLeft = joinStart = joinPosition = 0;
    pitch = 0; joinFrames = 1; erasing = false; tail = {}; realTail = {}; joinCorrection = {}; realJoin = {};
}

void CollabPlc::remember (const float* input, int frames) noexcept
{
    if (frames <= 0) return;
    if (frames > historyFrames) { input += (size_t) (frames - historyFrames) * 2; frames = historyFrames; }
    const int first = std::min (frames, historyFrames - written);
    std::copy_n (input, (size_t) first * 2, history.data() + (size_t) written * 2);
    std::copy_n (input + (size_t) first * 2, (size_t) (frames - first) * 2, history.data());
    written = (written + frames) % historyFrames;
    available = std::min (historyFrames, available + frames);
}

double CollabPlc::findPitch() noexcept
{
    // Search only contiguous real input. Short histories still support shorter
    // pitches, provided a >=2.5 ms comparison and the loop's quarter-period
    // seam both fit. Never correlate the zero-filled prefix or a previous gap.
    const int largestLag = std::min ({ maxPitch, available - minPitch, (int) ((available - 1) / 1.25) });
    if (largestLag < minPitch) return 0;
    const int comparison = std::min (correlationFrames, available - largestLag);
    for (int i = 0; i < historyFrames; ++i)
        mono[(size_t) i] = (frozen[(size_t) i * 2] + frozen[(size_t) i * 2 + 1]) * 0.5f;
    // Average before decimating, rather than aliasing wideband noise into a tone.
    const int coarseCount = historyFrames / decimation;
    const int offset = historyFrames - coarseCount * decimation;
    for (int i = 0; i < coarseCount; ++i)
    {
        float sum = 0;
        for (int j = 0; j < decimation; ++j) sum += mono[(size_t) (offset + i * decimation + j)];
        coarse[(size_t) i] = sum / decimation;
    }
    const int firstLag = std::max (1, minPitch / decimation), lastLag = largestLag / decimation;
    double coarseBest = -1;
    for (int lag = firstLag; lag <= lastLag; ++lag)
    {
        const double value = correlation (coarse.data() + coarseCount, comparison / decimation, lag);
        coarseCorrelation[(size_t) lag] = value;
        coarseBest = std::max (coarseBest, value);
    }
    if (coarseBest < Tuning::minCorrelation - 0.02) return 0;
    // Compare refined local peaks, not partially climbed coarse peaks. At
    // 44.1 kHz the coarse grid can otherwise prefer twice the true period and
    // turn a completed recovery into an apparently interrupted one.
    int bestLag = minPitch;
    double best = -1;
    for (int candidate = firstLag; candidate <= lastLag; ++candidate)
    {
        const double value = coarseCorrelation[(size_t) candidate];
        if (value < coarseBest - 0.02
            || (candidate > firstLag && value < coarseCorrelation[(size_t) candidate - 1])
            || (candidate < lastLag && value < coarseCorrelation[(size_t) candidate + 1])) continue;
        const int centre = candidate * decimation;
        double peak = -1; int peakLag = minPitch;
        for (int lag = std::max (minPitch, centre - decimation); lag <= std::min (largestLag, centre + decimation); ++lag)
        {
            const double refined = correlation (mono.data() + historyFrames, comparison, lag);
            if (refined > peak) { peak = refined; peakLag = lag; }
        }
        // Full-rate comparison retains the shortest near-equal period.
        if (peak > best + 0.002) { best = peak; bestLag = peakLag; }
    }
    if (best < Tuning::minCorrelation) return 0;
    double fraction = 0;
    if (bestLag > minPitch && bestLag < largestLag)
    {
        const double before = correlation (mono.data() + historyFrames, comparison, bestLag - 1);
        const double after = correlation (mono.data() + historyFrames, comparison, bestLag + 1);
        const double curvature = before - 2 * best + after;
        if (curvature < -1.0e-12) fraction = std::clamp (0.5 * (before - after) / curvature, -0.5, 0.5);
    }
    return bestLag + fraction;
}

float CollabPlc::sample (double position, int channel) const noexcept
{
    position = std::clamp (position, 0.0, (double) historyFrames - 1);
    const int i = (int) position;
    const float a = frozen[(size_t) i * 2 + channel];
    const float b = frozen[(size_t) std::min (i + 1, historyFrames - 1) * 2 + channel];
    return a + (float) (position - i) * (b - a);
}

float CollabPlc::loop (int time, int periods, int channel) const noexcept
{
    const double length = periods * pitch, phase = std::fmod ((double) time, length);
    const double overlap = pitch * 0.25;
    const float a = sample (historyFrames - length + phase, channel);
    if (phase < length - overlap) return a;
    // Smooth the *synthetic* loop tail with the history preceding its start.
    // Unlike G.711 Appendix I, no already delivered good samples are overwritten.
    const float b = sample (historyFrames - 2 * length + phase, channel);
    return a + (float) ((phase - length + overlap) / overlap) * (b - a);
}

void CollabPlc::beginLoss() noexcept
{
    erasing = true; elapsed = recoveryLeft = joinStart = joinPosition = 0;
    frozenFrames = available; joinCorrection = {}; realJoin = {};
    // The unused prefix is zero when a call has less than a full history.
    std::fill (frozen.begin(), frozen.end(), 0.0f);
    for (int i = 0; i < available; ++i)
    {
        const int from = (written + historyFrames - available + i) % historyFrames;
        for (int ch = 0; ch < 2; ++ch)
            frozen[(size_t) (historyFrames - available + i) * 2 + ch] = history[(size_t) from * 2 + ch];
    }
    pitch = findPitch(); // Once per continuous erasure, including refill.
    joinFrames = pitch > 0 ? pitch * 0.25 : fallbackFrames;
    if (pitch > 0)
        for (int ch = 0; ch < 2; ++ch)
            joinCorrection[(size_t) ch] = tail[(size_t) ch] - sample (historyFrames - pitch - 1, ch);
}

std::array<float, 2> CollabPlc::prediction (int time) const noexcept
{
    std::array<float, 2> value {};
    if (time >= muteFrames) return value;
    if (pitch == 0)
    {
        const float gain = std::max (0.0f, 1.0f - (float) (time + 1) / fallbackFrames);
        for (int ch = 0; ch < 2; ++ch) value[(size_t) ch] = frozen[(size_t) (historyFrames - 1) * 2 + ch] * gain;
    }
    else
    {
        const int periods = std::min ({ 3, 1 + time / stepFrames, (int) ((frozenFrames - pitch * 0.25 - 1) / pitch) });
        const double overlap = pitch * 0.25;
        const int transition = (periods - 1) * stepFrames;
        const float gain = (float) std::max (0.0, 1.0 - Tuning::attenuationPerStep * std::max (0, time + 1 - fullLevelFrames) / stepFrames);
        for (int ch = 0; ch < 2; ++ch)
        {
            float x = loop (time, periods, ch);
            if (periods > 1 && time - transition < overlap)
            {
                const float old = loop (time, periods - 1, ch);
                x = old + (float) ((time - transition) / overlap) * (x - old);
            }
            value[(size_t) ch] = x * gain;
        }
    }
    return value;
}

std::array<float, 2> CollabPlc::synthesize() noexcept
{
    auto value = prediction (elapsed);
    const float realMix = (float) std::max (0.0, 1.0 - (joinPosition + 1) / joinFrames);
    if (elapsed < muteFrames)
    {
        const double remainingJoin = std::min (joinFrames, (double) muteFrames - joinStart);
        const float mix = (float) std::max (0.0, 1.0 - (joinPosition + 1) / remainingJoin);
        for (int ch = 0; ch < 2; ++ch) value[(size_t) ch] += joinCorrection[(size_t) ch] * mix;
        ++elapsed;
    }
    // After the old waveform expires, an interrupted overlap can still end in
    // audible REAL input. Release that component instead of cutting it to zero.
    // With real silence this is exactly zero; old synthetic audio cannot leak
    // past the 60 ms budget or become the source of a new loss.
    for (int ch = 0; ch < 2; ++ch) value[(size_t) ch] += realJoin[(size_t) ch] * realMix;
    if (joinPosition < joinFrames) ++joinPosition;
    return value;
}

void CollabPlc::conceal (float* audio, int frames) noexcept
{
    if (frames <= 0) return;
    if (! erasing)
    {
        if (recoveryLeft == 0) beginLoss();
        else
        {
            // A relapse keeps the same frozen waveform, phase and attenuation.
            // Join from the partially recovered output without recycling it.
            const auto previous = prediction (std::max (0, elapsed - 1));
            joinStart = elapsed;
            joinPosition = 0; joinFrames = pitch > 0 ? pitch * 0.25 : fallbackFrames;
            realJoin = realTail;
            for (int ch = 0; ch < 2; ++ch)
                joinCorrection[(size_t) ch] = tail[(size_t) ch] - realTail[(size_t) ch] - previous[(size_t) ch];
        }
        erasing = true; recoveryLeft = 0;
        written = available = 0; // Only contiguous, genuinely received audio is history.
    }
    for (int i = 0; i < frames; ++i)
    {
        const auto value = synthesize();
        audio[(size_t) i * 2] = value[0]; audio[(size_t) i * 2 + 1] = value[1];
    }
    tail = { audio[(size_t) frames * 2 - 2], audio[(size_t) frames * 2 - 1] };
}

void CollabPlc::receive (float* audio, int frames) noexcept
{
    if (frames <= 0) return;
    remember (audio, frames); // Save the real input BEFORE the recovery overlap.
    realTail = { audio[(size_t) frames * 2 - 2], audio[(size_t) frames * 2 - 1] };
    if (erasing)
    {
        erasing = false;
        recoveryFrames = recoveryLeft = pitch > 0 ? (int) std::ceil (pitch) : fallbackFrames;
    }
    int blended = 0;
    for (; blended < frames && recoveryLeft > 0; ++blended)
    {
        const auto value = synthesize();
        const float progress = 1.0f - (float) --recoveryLeft / recoveryFrames;
        // The ASRC still has a ramp/zero tail (or has just been reset). A
        // zero-slope overlap suppresses that first filter transient without
        // holding any real samples or extending the one-period recovery.
        const float gain = progress * progress * (3.0f - 2.0f * progress);
        if (blended == frames - 1)
            for (auto& x : realTail) x *= gain;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& real = audio[(size_t) blended * 2 + ch];
            real = value[(size_t) ch] + gain * (real - value[(size_t) ch]);
        }
    }
    // synthesize() alone advances elapsed, including the synthetic overlap.
    // Once recoveryLeft reaches zero this episode is over, regardless of the
    // amount of new history. Good audio never spends a concealment budget.
    tail = { audio[(size_t) frames * 2 - 2], audio[(size_t) frames * 2 - 1] };
}
}
