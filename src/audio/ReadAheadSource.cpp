#include "audio/ReadAheadSource.h"

#include <algorithm>

namespace gocue
{

namespace
{
    constexpr int chunkSamples = 2048;   // one background read
    constexpr int keepBehind = 512;      // samples kept before the play position (a tiny backwards seek stays cached)
}

ReadAheadSource::ReadAheadSource (juce::PositionableAudioSource& u, juce::TimeSliceThread& t, int numSamplesToBuffer, int channels)
    : upstream (u), thread (t), numChannels (juce::jmax (1, channels)), ringSize (juce::jmax (4096, numSamplesToBuffer))
{
}

ReadAheadSource::~ReadAheadSource()
{
    releaseResources();
}

void ReadAheadSource::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    thread.removeTimeSliceClient (this);
    upstream.prepareToPlay (samplesPerBlockExpected, sampleRate);

    {
        const juce::ScopedLock sl (rangeLock);
        ring.setSize (numChannels, ringSize, false, true, true);
        validStart = validEnd = 0;
    }

    prepared.store (true, std::memory_order_release);

    // fill enough to start cleanly, then hand the rest to the thread
    const juce::int64 wanted = juce::jmin ((juce::int64) ringSize / 2, (juce::int64) (sampleRate > 0.0 ? sampleRate / 4.0 : 11025.0));

    for (int guard = 0; guard < 64 && getNumSamplesReady() < wanted; ++guard)
        if (! fillChunk())
            break;

    thread.addTimeSliceClient (this);
    thread.moveToFrontOfQueue (this);
}

void ReadAheadSource::releaseResources()
{
    prepared.store (false, std::memory_order_release);
    thread.removeTimeSliceClient (this);

    {
        const juce::ScopedLock sl (readLock);   // no read in flight
    }

    upstream.releaseResources();
}

int ReadAheadSource::getNumSamplesReady() const
{
    const juce::ScopedLock sl (rangeLock);
    const auto pos = playPos.load (std::memory_order_relaxed);
    return (int) juce::jlimit ((juce::int64) 0, (juce::int64) ringSize, validEnd - juce::jmax (pos, validStart)) * (pos >= validStart && pos < validEnd ? 1 : 0);
}

void ReadAheadSource::setNextReadPosition (juce::int64 newPosition)
{
    playPos.store (newPosition, std::memory_order_relaxed);
    thread.moveToFrontOfQueue (this);
}

void ReadAheadSource::invalidate (juce::int64 fromPosition)
{
    {
        const juce::ScopedLock sl (rangeLock);
        validStart = validEnd = 0;   // nothing in the ring describes the new content
        ++generation;                // a background fill that is still reading the old content will not publish
        playPos.store (fromPosition, std::memory_order_relaxed);
    }

    if (prepared.load (std::memory_order_acquire))
        fillChunk();   // the first chunk right now: no silent gap at the jump

    thread.moveToFrontOfQueue (this);
}

void ReadAheadSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    if (info.buffer == nullptr || info.numSamples <= 0)
        return;

    juce::int64 pos, vStart, vEnd;

    {
        const juce::ScopedLock sl (rangeLock);
        pos = playPos.load (std::memory_order_relaxed);
        vStart = validStart;
        vEnd = validEnd;
        playPos.store (pos + info.numSamples, std::memory_order_relaxed);
    }

    const juce::int64 from = juce::jmax (pos, vStart);
    const juce::int64 to = juce::jmin (pos + (juce::int64) info.numSamples, vEnd);

    if (to <= from || ring.getNumSamples() == 0)
        return;   // nothing cached for this block: silence (the thread is behind)

    const int offset = (int) (from - pos);
    int remaining = (int) (to - from);
    juce::int64 readPos = from;
    int dest = info.startSample + offset;

    while (remaining > 0)
    {
        const int ringIndex = (int) (readPos % (juce::int64) ringSize);
        const int n = juce::jmin (remaining, ringSize - ringIndex);

        for (int ch = 0; ch < juce::jmin (numChannels, info.buffer->getNumChannels()); ++ch)
            info.buffer->copyFrom (ch, dest, ring, ch, ringIndex, n);

        readPos += n;
        dest += n;
        remaining -= n;
    }
}

void ReadAheadSource::readIntoRing (juce::int64 start, int length)
{
    const juce::ScopedLock sl (readLock);

    if (upstream.getNextReadPosition() != start)
        upstream.setNextReadPosition (start);

    int done = 0;

    while (done < length)
    {
        const int ringIndex = (int) ((start + done) % (juce::int64) ringSize);
        const int n = juce::jmin (length - done, ringSize - ringIndex);
        juce::AudioSourceChannelInfo info (&ring, ringIndex, n);
        upstream.getNextAudioBlock (info);
        done += n;
    }
}

bool ReadAheadSource::fillChunk()
{
    if (! prepared.load (std::memory_order_acquire) || ring.getNumSamples() == 0)
        return false;

    juce::int64 readStart = 0, readEnd = 0;
    bool restart = false;
    juce::uint32 startedIn = 0;

    {
        const juce::ScopedLock sl (rangeLock);
        startedIn = generation;
        const auto pos = juce::jmax ((juce::int64) 0, playPos.load (std::memory_order_relaxed));
        const juce::int64 targetEnd = pos + (juce::int64) ringSize - keepBehind - 4;

        if (pos < validStart || pos >= validEnd)
        {
            // cache miss: start over at the play position
            readStart = pos;
            readEnd = juce::jmin (targetEnd, pos + (juce::int64) chunkSamples);
            restart = true;
        }
        else if (validEnd < targetEnd)
        {
            readStart = validEnd;
            readEnd = juce::jmin (targetEnd, validEnd + (juce::int64) chunkSamples);
        }
        else
        {
            return false;   // full up to the horizon
        }
    }

    if (readEnd <= readStart)
        return false;

    readIntoRing (readStart, (int) (readEnd - readStart));

    {
        const juce::ScopedLock sl (rangeLock);

        if (generation != startedIn)
            return true;   // invalidated meanwhile: what we read describes the old content

        const auto pos = playPos.load (std::memory_order_relaxed);

        if (restart)
        {
            if (pos < readStart || pos >= readEnd)
                return true;   // the play position moved while we read: the next call restarts again

            validStart = readStart;
            validEnd = readEnd;
        }
        else
        {
            validEnd = readEnd;
            // the ring can only hold ringSize samples: what falls off the back is gone
            validStart = juce::jmax (validStart, validEnd - (juce::int64) ringSize + 1);
        }
    }

    return true;
}

int ReadAheadSource::useTimeSlice()
{
    return fillChunk() ? 1 : 100;
}

} // namespace gocue
