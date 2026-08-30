#include "model/Cue.h"

#include <juce_audio_basics/juce_audio_basics.h>
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

void Cue::sanitise() noexcept
{
    fadeInMs  = juce::jlimit (0, maxFadeMs, fadeInMs);
    fadeOutMs = juce::jlimit (0, maxFadeMs, fadeOutMs);

    if (! std::isfinite (gainDb))
        gainDb = 0.0;

    gainDb = juce::jlimit (minGainDb, maxGainDb, gainDb);

    if (! std::isfinite (durationSeconds) || durationSeconds < 0.0)
        durationSeconds = 0.0;
}

} // namespace gocue
