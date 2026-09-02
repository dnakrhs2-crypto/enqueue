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
