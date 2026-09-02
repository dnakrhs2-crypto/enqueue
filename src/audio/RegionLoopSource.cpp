#include "audio/RegionLoopSource.h"

#include <algorithm>

namespace gocue
{

RegionLoopSource::RegionLoopSource (std::unique_ptr<juce::AudioFormatReader> readerToUse)
    : reader (std::move (readerToUse))
{
    jassert (reader != nullptr);
    setRegion (0, reader != nullptr ? reader->lengthInSamples : 0);
}

RegionLoopSource::~RegionLoopSource() = default;

void RegionLoopSource::setRegion (juce::int64 startSample, juce::int64 endSample) noexcept
{
    const juce::int64 fileLength = reader != nullptr ? reader->lengthInSamples : 0;

    if (endSample <= 0 || endSample > fileLength)
        endSample = fileLength;

    startSample = std::clamp<juce::int64> (startSample, 0, fileLength);

    regionStart.store (startSample, std::memory_order_relaxed);
    regionLength.store (std::max<juce::int64> (0, endSample - startSample), std::memory_order_relaxed);
}

void RegionLoopSource::setPlayCount (int count, bool shouldLoopForever) noexcept
{
    playCount.store (std::max (1, count), std::memory_order_relaxed);
    loopForever.store (shouldLoopForever, std::memory_order_relaxed);
}

void RegionLoopSource::setEndAfterPass (int pass) noexcept
{
    endAfterPass.store (std::max (0, pass), std::memory_order_relaxed);
}

void RegionLoopSource::setEnvelope (const Envelope& newEnvelope)
{
    envelope = newEnvelope;
    envelope.sanitise();
}

bool RegionLoopSource::isInfinite() const noexcept
{
    return loopForever.load (std::memory_order_relaxed) && endAfterPass.load (std::memory_order_relaxed) < 0;
}

int RegionLoopSource::getPassIndexFor (juce::int64 position) const noexcept
{
    const auto len = getRegionLength();
    return len > 0 && position > 0 ? (int) (position / len) : 0;
}

juce::int64 RegionLoopSource::getOffsetFor (juce::int64 position) const noexcept
{
    const auto len = getRegionLength();
    return len > 0 && position > 0 ? position % len : 0;
}

juce::int64 RegionLoopSource::getTotalLength() const
{
    const auto len = getRegionLength();

    if (len <= 0)
        return 0;

    const int lastPass = endAfterPass.load (std::memory_order_relaxed);

    if (lastPass >= 0)
        return ((juce::int64) lastPass + 1) * len;

    if (loopForever.load (std::memory_order_relaxed))
        return infiniteLength;

    return (juce::int64) playCount.load (std::memory_order_relaxed) * len;
}

void RegionLoopSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    const juce::int64 startPosition = nextPosition.load (std::memory_order_relaxed);
    nextPosition.store (startPosition + info.numSamples, std::memory_order_relaxed);

    const auto len = getRegionLength();
    const auto start = getRegionStart();

    if (reader == nullptr || len <= 0 || info.buffer == nullptr)
    {
        reachedEnd.store (true, std::memory_order_relaxed);
        return;
    }

    const auto total = getTotalLength();
    juce::int64 pos = startPosition;
    int dest = info.startSample;
    int remaining = info.numSamples;

    while (remaining > 0)
    {
        if (pos >= total)
        {
            reachedEnd.store (true, std::memory_order_relaxed);
            break;
        }

        if (pos < 0)   // a seek before the start: silence until 0
        {
            const int gap = (int) std::min<juce::int64> (remaining, -pos);
            pos += gap;
            dest += gap;
            remaining -= gap;
            continue;
        }

        const juce::int64 offset = pos % len;
        const int chunk = (int) std::min<juce::int64> ({ (juce::int64) remaining, len - offset, total - pos });

        reader->read (info.buffer, dest, chunk, start + offset, true, true);

        if (envelope.isActive())
            applyEnvelope (*info.buffer, dest, chunk, offset, len);

        pos += chunk;
        dest += chunk;
        remaining -= chunk;
    }
}

void RegionLoopSource::applyEnvelope (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                                      juce::int64 offsetInRegion, juce::int64 regionLen) const
{
    const double sr = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    const double regionSeconds = (double) regionLen / sr;
    constexpr int step = 64;
    int done = 0;

    while (done < numSamples)
    {
        const int n = std::min (step, numSamples - done);
        const float a = envelope.levelAt ((double) (offsetInRegion + done) / sr, regionSeconds);
        const float b = envelope.levelAt ((double) (offsetInRegion + done + n) / sr, regionSeconds);
        buffer.applyGainRamp (startSample + done, n, a, b);
        done += n;
    }
}

} // namespace gocue
