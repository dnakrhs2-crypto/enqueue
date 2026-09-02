#include "audio/RegionLoopSource.h"

#include <algorithm>

namespace gocue
{

//==============================================================================
juce::int64 RegionLoopSource::Layout::sequenceLength() const noexcept
{
    juce::int64 total = 0;

    for (int i = 0; i < numRuns; ++i)
        if (runs[i].count > 0)
            total += runs[i].length * (juce::int64) runs[i].count;

    return total;
}

bool RegionLoopSource::Layout::hasEndlessRun() const noexcept
{
    for (int i = 0; i < numRuns; ++i)
        if (runs[i].count < 0)
            return true;

    return false;
}

juce::int64 RegionLoopSource::Layout::totalLength() const noexcept
{
    if (numRuns == 0 || regionLength <= 0)
        return 0;

    if (hasEndlessRun() || sequenceCount < 0)
        return infiniteLength;

    return sequenceLength() * (juce::int64) sequenceCount;
}

RegionLoopSource::Location RegionLoopSource::Layout::locate (juce::int64 pos) const noexcept
{
    Location loc;
    loc.fileSample = regionStart;

    if (numRuns == 0 || regionLength <= 0)
    {
        loc.beyondEnd = true;
        return loc;
    }

    pos = std::max<juce::int64> (0, pos);
    const bool endless = hasEndlessRun();
    const juce::int64 seqLen = sequenceLength();
    juce::int64 rem = pos;

    if (! endless)
    {
        if (seqLen <= 0)
        {
            loc.beyondEnd = true;
            return loc;
        }

        loc.sequencePass = (int) std::min<juce::int64> (pos / seqLen, std::numeric_limits<int>::max());
        rem = pos % seqLen;

        if (sequenceCount >= 0 && loc.sequencePass >= sequenceCount)
        {
            // past the end: park on the last sample of the last run
            loc.sequencePass = std::max (0, sequenceCount - 1);
            loc.beyondEnd = true;
            rem = seqLen - 1;
        }
    }

    for (int i = 0; i < numRuns; ++i)
    {
        const auto& r = runs[i];

        if (r.count == 0 || r.length <= 0)
            continue;

        if (r.count < 0)   // endless: everything from here cycles inside this run
        {
            loc.run = i;
            loc.pass = (int) std::min<juce::int64> (rem / r.length, std::numeric_limits<int>::max());
            loc.offset = rem % r.length;
            loc.fileSample = r.fileStart + loc.offset;
            return loc;
        }

        const juce::int64 block = r.length * (juce::int64) r.count;

        if (rem < block)
        {
            loc.run = i;
            loc.pass = (int) (rem / r.length);
            loc.offset = rem % r.length;
            loc.fileSample = r.fileStart + loc.offset;
            return loc;
        }

        rem -= block;
        loc.run = i;
        loc.pass = r.count - 1;
        loc.offset = r.length - 1;
        loc.fileSample = r.fileStart + loc.offset;
    }

    loc.beyondEnd = true;   // ran out of runs (only when the sequence is exhausted)
    return loc;
}

//==============================================================================
RegionLoopSource::RegionLoopSource (std::unique_ptr<juce::AudioFormatReader> readerToUse)
    : reader (std::move (readerToUse))
{
    jassert (reader != nullptr);
    editRegionEnd = reader != nullptr ? reader->lengthInSamples : 0;
    rebuildLayout();
}

RegionLoopSource::~RegionLoopSource() = default;

void RegionLoopSource::publish (const Layout& l) noexcept
{
    layoutVersion.fetch_add (1, std::memory_order_acq_rel);   // odd: write in progress
    published = l;
    layoutVersion.fetch_add (1, std::memory_order_acq_rel);   // even: consistent
}

RegionLoopSource::Layout RegionLoopSource::snapshot() const noexcept
{
    Layout copy;

    for (;;)
    {
        const auto v1 = layoutVersion.load (std::memory_order_acquire);

        if ((v1 & 1u) != 0)
            continue;

        copy = published;

        if (layoutVersion.load (std::memory_order_acquire) == v1)
            return copy;
    }
}

void RegionLoopSource::rebuildLayout()
{
    const juce::int64 fileLength = reader != nullptr ? reader->lengthInSamples : 0;
    juce::int64 start = std::clamp<juce::int64> (editRegionStart, 0, fileLength);
    juce::int64 end = editRegionEnd < 0 || editRegionEnd > fileLength ? fileLength : editRegionEnd;
    end = std::max (start, end);

    Layout l;
    l.regionStart = start;
    l.regionLength = end - start;
    l.sequenceCount = resolvedSequenceCount >= 0 ? resolvedSequenceCount : (editLoopForever ? -1 : std::max (1, editPlayCount));

    // slice boundaries inside the region, sorted, unique
    std::vector<SliceMarker> markers;

    for (const auto& m : editSlices)
        if (m.fileSample > start && m.fileSample < end)
            markers.push_back (m);

    std::sort (markers.begin(), markers.end(), [] (const SliceMarker& a, const SliceMarker& b) { return a.fileSample < b.fileSample; });
    markers.erase (std::unique (markers.begin(), markers.end(), [] (const SliceMarker& a, const SliceMarker& b) { return a.fileSample == b.fileSample; }), markers.end());

    if ((int) markers.size() > maxRuns - 2)
        markers.resize ((size_t) (maxRuns - 2));

    // run i starts at marker i-1 (or the region start) and plays that slice's count
    juce::int64 runStart = start;
    int runCount = markers.empty() ? 1 : editFirstSliceCount;
    auto pushRun = [&] (juce::int64 from, juce::int64 to, int count)
    {
        if (to <= from || l.numRuns >= maxRuns)
            return;

        const int index = l.numRuns;

        if (index < (int) resolvedCounts.size() && resolvedCounts[(size_t) index] >= 0)
            count = resolvedCounts[(size_t) index];   // fixed by a devamp

        if (stopAfterRun >= 0 && index > stopAfterRun)
            count = 0;                                 // devamp with stop: nothing after the loop point

        l.runs[index] = { from, to - from, count };
        ++l.numRuns;
    };

    if (markers.empty())
    {
        // one slice: the classic region loop. Its count is the sequence count (keeps run 0 semantics for devamp).
        pushRun (start, end, 1);
    }
    else
    {
        for (const auto& m : markers)
        {
            pushRun (runStart, m.fileSample, runCount);
            runStart = m.fileSample;
            runCount = m.playCount;
        }

        pushRun (runStart, end, runCount);
    }

    if (markers.empty() && l.numRuns == 1)
    {
        // no slices: fold the sequence count into the single run so the old pass semantics hold
        if (l.runs[0].count == 1 && ! (resolvedCounts.size() > 0 && resolvedCounts[0] >= 0))
            l.runs[0].count = l.sequenceCount;

        l.sequenceCount = 1;
    }

    publish (l);
}

void RegionLoopSource::setRegion (juce::int64 startSample, juce::int64 endSample) noexcept
{
    editRegionStart = startSample;
    editRegionEnd = endSample;
    rebuildLayout();
}

void RegionLoopSource::setPlayCount (int count, bool shouldLoopForever) noexcept
{
    editPlayCount = std::max (1, count);
    editLoopForever = shouldLoopForever;
    rebuildLayout();
}

void RegionLoopSource::setSlices (const std::vector<SliceMarker>& markers, int firstSliceCount)
{
    editSlices = markers;
    editFirstSliceCount = std::max (-1, firstSliceCount);
    rebuildLayout();
}

void RegionLoopSource::finishCurrentPass (juce::int64 virtualPosition, bool stopAfterThisPass) noexcept
{
    const auto l = snapshot();
    const auto loc = l.locate (virtualPosition);

    if (loc.beyondEnd || loc.run >= l.numRuns)
        return;

    if ((int) resolvedCounts.size() < l.numRuns)
        resolvedCounts.resize ((size_t) l.numRuns, -1);

    if (l.runs[loc.run].count < 0)
    {
        resolvedCounts[(size_t) loc.run] = loc.pass + 1;
    }
    else if (l.sequenceCount < 0)
    {
        resolvedSequenceCount = loc.sequencePass + 1;
    }
    else if (! stopAfterThisPass)
    {
        return;   // nothing endless to finish
    }
    else
    {
        resolvedCounts[(size_t) loc.run] = loc.pass + 1;   // finite loop: still end after this pass
        resolvedSequenceCount = loc.sequencePass + 1;
    }

    if (stopAfterThisPass)
    {
        stopAfterRun = loc.run;
        resolvedSequenceCount = loc.sequencePass + 1;
    }

    rebuildLayout();
}

void RegionLoopSource::setEndAfterPass (int pass) noexcept
{
    if (resolvedCounts.empty())
        resolvedCounts.assign (1, -1);

    resolvedCounts[0] = std::max (0, pass) + 1;
    rebuildLayout();
}

void RegionLoopSource::setEnvelope (const Envelope& newEnvelope)
{
    envelope = newEnvelope;
    envelope.sanitise();
}

//==============================================================================
void RegionLoopSource::getRegion (juce::int64& startSample, juce::int64& lengthSamples) const noexcept
{
    const auto l = snapshot();
    startSample = l.regionStart;
    lengthSamples = l.regionLength;
}

juce::int64 RegionLoopSource::getRegionStart() const noexcept
{
    return snapshot().regionStart;
}

juce::int64 RegionLoopSource::getRegionLength() const noexcept
{
    return snapshot().regionLength;
}

bool RegionLoopSource::isInfinite() const noexcept
{
    const auto l = snapshot();
    return l.hasEndlessRun() || l.sequenceCount < 0;
}

RegionLoopSource::Location RegionLoopSource::locate (juce::int64 virtualPosition) const noexcept
{
    return snapshot().locate (virtualPosition);
}

juce::int64 RegionLoopSource::getRunLength (int index) const noexcept
{
    const auto l = snapshot();
    return index >= 0 && index < l.numRuns ? l.runs[index].length : 0;
}

juce::int64 RegionLoopSource::getOffsetFor (juce::int64 position) const noexcept
{
    const auto l = snapshot();
    const auto loc = l.locate (position);
    return std::max<juce::int64> (0, loc.fileSample - l.regionStart);
}

juce::int64 RegionLoopSource::virtualPositionFor (juce::int64 fileSample, int passHint) const noexcept
{
    const auto l = snapshot();
    juce::int64 base = 0;

    for (int i = 0; i < l.numRuns; ++i)
    {
        const auto& r = l.runs[i];

        if (r.count == 0 || r.length <= 0)
            continue;

        if (fileSample >= r.fileStart && fileSample < r.fileStart + r.length)
        {
            const int pass = r.count < 0 ? std::max (0, passHint) : std::clamp (passHint, 0, r.count - 1);
            return base + (juce::int64) pass * r.length + (fileSample - r.fileStart);
        }

        if (r.count < 0)
            return base;   // an endless run before the sample: the sample is unreachable, start of the run

        base += r.length * (juce::int64) r.count;
    }

    return std::max<juce::int64> (0, std::min (base, std::max<juce::int64> (0, l.totalLength() - 1)));
}

juce::int64 RegionLoopSource::getTotalLength() const
{
    return snapshot().totalLength();
}

//==============================================================================
void RegionLoopSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    const juce::int64 startPosition = nextPosition.load (std::memory_order_relaxed);
    nextPosition.store (startPosition + info.numSamples, std::memory_order_relaxed);

    const auto l = snapshot();   // one consistent layout for the whole block

    if (reader == nullptr || l.regionLength <= 0 || l.numRuns == 0 || info.buffer == nullptr)
    {
        reachedEnd.store (true, std::memory_order_relaxed);
        return;
    }

    const auto total = l.totalLength();
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

        const auto loc = l.locate (pos);

        if (loc.beyondEnd)
        {
            reachedEnd.store (true, std::memory_order_relaxed);
            break;
        }

        const auto& run = l.runs[loc.run];
        const int chunk = (int) std::min<juce::int64> ({ (juce::int64) remaining, run.length - loc.offset, total - pos });

        if (chunk <= 0)
        {
            reachedEnd.store (true, std::memory_order_relaxed);
            break;
        }

        {
            // every file channel into its own buffer channel (no stereo duplication: the level matrix routes)
            constexpr int maxChannels = 32;
            float* dests[maxChannels];
            const int numCh = std::min ({ (int) reader->numChannels, info.buffer->getNumChannels(), maxChannels });

            for (int ch = 0; ch < numCh; ++ch)
                dests[ch] = info.buffer->getWritePointer (ch, dest);

            reader->read (reinterpret_cast<int* const*> (dests), numCh, loc.fileSample, chunk, false);

            if (! reader->usesFloatingPointData)
                for (int ch = 0; ch < numCh; ++ch)
                    juce::FloatVectorOperations::convertFixedToFloat (dests[ch], reinterpret_cast<const int*> (dests[ch]), 1.0f / 0x7fffffff, chunk);
        }

        if (envelope.isActive())
            applyEnvelope (*info.buffer, dest, chunk, loc.fileSample - l.regionStart, l.regionLength);

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
