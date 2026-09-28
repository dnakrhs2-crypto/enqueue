#include "HostTransport.h"

#include <cmath>

namespace gocue::livemix
{

static_assert (std::atomic<bool>::is_always_lock_free);

void HostTransport::prepare (double newSampleRate) noexcept
{
    newSampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    if (sampleRate != newSampleRate)
        nextSample = chunkStart = 0;
    sampleRate = newSampleRate;
}

void HostTransport::beginBlock() noexcept
{
    playing = getSendTransport();
    chunkStart = nextSample;
}

juce::AudioPlayHead::PositionInfo HostTransport::getPositionAt (juce::int64 sample) const noexcept
{
    juce::AudioPlayHead::PositionInfo position;
    // A stopped position clears JUCE's cached VST2 playing flag. An empty Optional would leave it playing.
    // Both JUCE hosts require timeInSamples even when stopped; no other optional fields are valid then.
    position.setTimeInSamples (playing ? sample : 0);
    if (playing)
    {
        const double seconds = (double) sample / sampleRate;
        const double ppq = seconds * 2.0;
        position.setIsPlaying (true);
        position.setTimeInSeconds (seconds);
        position.setBpm (120.0);
        position.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
        position.setPpqPosition (ppq);
        position.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
    }
    return position;
}

} // namespace gocue::livemix
