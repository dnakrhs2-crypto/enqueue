#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace gocue
{

/** Linear-amplitude gain envelope used for fade-in, fade-out and de-clicked stops.
    Owned and driven by the audio thread only: no locking, no allocation. */
class FadeEnvelope
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    }

    /** Jumps to a level immediately, cancelling any ramp. */
    void setLevel (float newLevel) noexcept
    {
        level = target = clampLevel (newLevel);
        remaining = 0;
        step = 0.0f;
        fadingOut = false;
    }

    /** Ramps linearly from the current level to newTarget over durationMs (0 = jump). */
    void fadeTo (float newTarget, double durationMs) noexcept
    {
        target = clampLevel (newTarget);
        fadingOut = target < level;

        const auto samples = std::llround (std::max (0.0, durationMs) * 0.001 * sampleRate);
        remaining = (int) std::min<long long> (samples, 1LL << 30);

        if (remaining <= 0)
        {
            level = target;
            remaining = 0;
            step = 0.0f;
            return;
        }

        step = (target - level) / (float) remaining;
    }

    void fadeIn (double durationMs) noexcept   { fadeTo (1.0f, durationMs); }
    void fadeOut (double durationMs) noexcept  { fadeTo (0.0f, durationMs); }

    bool isRamping() const noexcept            { return remaining > 0; }
    bool isFadingOut() const noexcept          { return fadingOut; }
    /** True once the level sits at silence with no ramp pending. */
    bool hasReachedSilence() const noexcept    { return remaining == 0 && level <= 0.0f; }
    float getLevel() const noexcept            { return level; }
    float getTarget() const noexcept           { return target; }
    int getRemainingSamples() const noexcept   { return remaining; }

    /** Multiplies all channels of buffer[startSample, startSample + numSamples) by the envelope. */
    void applyToBuffer (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept
    {
        int pos = startSample;
        int left = numSamples;

        if (remaining > 0 && left > 0)
        {
            const int n = std::min (left, remaining);
            const float start = level;
            const float end = (n == remaining) ? target : level + step * (float) n;

            buffer.applyGainRamp (pos, n, start, end);

            level = end;
            remaining -= n;
            pos += n;
            left -= n;

            if (remaining == 0)
            {
                level = target;
                step = 0.0f;
            }
        }

        if (left > 0)
            buffer.applyGain (pos, left, level);
    }

private:
    static float clampLevel (float v) noexcept { return std::clamp (v, 0.0f, 1.0f); }

    double sampleRate = 44100.0;
    float level = 1.0f;
    float target = 1.0f;
    float step = 0.0f;
    int remaining = 0;
    bool fadingOut = false;
};

} // namespace gocue
