#include "model/Cue.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace gocue
{

float Cue::gainLinear() const noexcept
{
    const double db = juce::jlimit (minGainDb, maxGainDb, gainDb);
    return juce::Decibels::decibelsToGain ((float) db, (float) minGainDb);
}

Cue Cue::duplicated() const
{
    Cue copy (*this);
    copy.id = juce::Uuid();
    return copy;
}

double Cue::regionEnd() const noexcept
{
    if (audio.endSeconds >= 0.0)
        return durationSeconds > 0.0 ? std::min (audio.endSeconds, durationSeconds) : audio.endSeconds;

    return durationSeconds;
}

double Cue::regionLength() const noexcept
{
    return std::max (0.0, regionEnd() - regionStart());
}

double Cue::passLength() const noexcept
{
    return regionLength() / std::max (AudioCueData::minRate, audio.rate);
}

double Cue::effectiveLength() const noexcept
{
    return audio.infiniteLoop ? -1.0 : passLength() * (double) audio.playCount;
}

void Cue::sanitise() noexcept
{
    auto fixSeconds = [] (double& v, double hi)
    {
        if (! std::isfinite (v) || v < 0.0)
            v = 0.0;

        v = std::min (v, hi);
    };

    fixSeconds (preWaitSeconds, maxWaitSeconds);
    fixSeconds (postWaitSeconds, maxWaitSeconds);
    color = juce::jlimit (0, 20, color);
    secondColor = juce::jlimit (0, 20, secondColor);
    wallClock.hour = juce::jlimit (0, 23, wallClock.hour);
    wallClock.minute = juce::jlimit (0, 59, wallClock.minute);
    wallClock.second = juce::jlimit (0, 59, wallClock.second);
    wallClock.daysMask &= 0x7f;
    fixSeconds (fadeStopOthers.seconds, 600.0);
    fixSeconds (duck.seconds, 600.0);

    if (! std::isfinite (duck.levelDb))
        duck.levelDb = -12.0;

    duck.levelDb = juce::jlimit (minGainDb, maxGainDb, duck.levelDb);

    fadeOutMs = juce::jlimit (0, maxFadeMs, fadeOutMs);

    if (! std::isfinite (gainDb))
        gainDb = 0.0;

    gainDb = juce::jlimit (minGainDb, maxGainDb, gainDb);

    if (! std::isfinite (durationSeconds) || durationSeconds < 0.0)
        durationSeconds = 0.0;

    auto& a = audio;

    if (! std::isfinite (a.startSeconds) || a.startSeconds < 0.0)
        a.startSeconds = 0.0;

    if (! std::isfinite (a.endSeconds) || a.endSeconds < 0.0)
        a.endSeconds = -1.0;

    if (a.endSeconds >= 0.0 && a.endSeconds <= a.startSeconds)   // empty region: play to the end instead of nothing
        a.endSeconds = -1.0;

    if (durationSeconds > 0.0 && a.startSeconds >= durationSeconds)
        a.startSeconds = 0.0;

    a.playCount = juce::jlimit (1, AudioCueData::maxPlayCount, a.playCount);

    if (! std::isfinite (a.rate))
        a.rate = 1.0;

    a.rate = juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, a.rate);
    a.envelope.sanitise();
}

} // namespace gocue
