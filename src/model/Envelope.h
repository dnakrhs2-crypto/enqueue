#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace gocue
{

/** One breakpoint of the integrated fade envelope. */
struct EnvelopePoint
{
    double x = 0.0;       // seconds from the region start, or a 0..1 fraction of the region when locked to the trim
    double level = 1.0;   // linear gain 0..1
};

/** The integrated fade envelope drawn over the waveform (QLab's "integrated fade").
    Levels between points are interpolated linearly or with a smooth (cosine) curve.
    A disabled or empty envelope is unity gain. Plain data, message thread; the audio
    thread only reads a copy that was handed over before playback started. */
struct Envelope
{
    bool enabled = false;
    bool linear = false;        // false = smooth segments
    bool lockToTrim = true;     // x is a fraction of the region so the shape stretches with the trim
    std::vector<EnvelopePoint> points;

    static constexpr int maxPoints = 256;

    bool isActive() const noexcept { return enabled && ! points.empty(); }

    /** Sorts by x, clamps levels to 0..1 and x to >= 0 (and <= 1 when locked), drops excess points. */
    void sanitise() noexcept
    {
        for (auto& p : points)
        {
            if (! std::isfinite (p.x))
                p.x = 0.0;

            if (! std::isfinite (p.level))
                p.level = 1.0;

            p.level = std::clamp (p.level, 0.0, 1.0);
            p.x = lockToTrim ? std::clamp (p.x, 0.0, 1.0) : std::max (0.0, p.x);
        }

        std::stable_sort (points.begin(), points.end(),
                          [] (const EnvelopePoint& a, const EnvelopePoint& b) { return a.x < b.x; });

        if ((int) points.size() > maxPoints)
            points.resize ((size_t) maxPoints);
    }

    double toX (double seconds, double regionLength) const noexcept
    {
        if (! lockToTrim)
            return seconds;

        return regionLength > 0.0 ? seconds / regionLength : 0.0;
    }

    double toSeconds (double x, double regionLength) const noexcept
    {
        return lockToTrim ? x * regionLength : x;
    }

    /** Gain at 'seconds' after the region start (1 when inactive). Before the first point the first
        level holds, after the last point the last level holds. */
    float levelAt (double seconds, double regionLength) const noexcept
    {
        if (! isActive())
            return 1.0f;

        const double x = toX (seconds, regionLength);

        if (x <= points.front().x)
            return (float) points.front().level;

        if (x >= points.back().x)
            return (float) points.back().level;

        for (size_t i = 1; i < points.size(); ++i)
        {
            const auto& a = points[i - 1];
            const auto& b = points[i];

            if (x <= b.x)
            {
                const double span = b.x - a.x;
                double t = span > 0.0 ? (x - a.x) / span : 1.0;

                if (! linear)
                    t = 0.5 - 0.5 * std::cos (t * juce::MathConstants<double>::pi);

                return (float) (a.level + (b.level - a.level) * t);
            }
        }

        return (float) points.back().level;
    }

    /** Seconds after the region start until the envelope first reaches (almost) full level; 0 when inactive. */
    double fadeInSeconds (double regionLength) const noexcept
    {
        if (! isActive() || points.front().level >= 0.999)
            return 0.0;

        for (const auto& p : points)
            if (p.level >= 0.999)
                return toSeconds (p.x, regionLength);

        return toSeconds (points.back().x, regionLength);
    }

    /** Switches between fraction and seconds storage, keeping the shape at the current region length. */
    void setLockToTrim (bool shouldLock, double regionLength)
    {
        if (shouldLock == lockToTrim)
            return;

        for (auto& p : points)
            p.x = shouldLock ? (regionLength > 0.0 ? p.x / regionLength : 0.0) : p.x * regionLength;

        lockToTrim = shouldLock;
        sanitise();
    }

    /** A classic linear fade-in (0, 0) -> (fadeInSeconds, 1), unlocked so it keeps its length when trimmed.
        Used to migrate version-1 projects (fadeInMs). */
    static Envelope fromFadeIn (double fadeInSeconds)
    {
        Envelope e;
        e.lockToTrim = false;
        e.linear = true;

        if (fadeInSeconds > 0.0)
        {
            e.enabled = true;
            e.points = { { 0.0, 0.0 }, { fadeInSeconds, 1.0 } };
        }

        return e;
    }
};

} // namespace gocue
