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
    const juce::SpinLock::ScopedLockType sl (layoutLock);
    published = l;
}

RegionLoopSource::Layout RegionLoopSource::snapshot() const noexcept
{
    const juce::SpinLock::ScopedLockType sl (layoutLock);
    return published;
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

    // slice boundaries: sorted, unique; the ones inside the region split it, the last one at or before the
    // start owns the region's first slice (its count applies), later ones are out of reach
    std::vector<SliceMarker> all = editSlices;
    std::sort (all.begin(), all.end(), [] (const SliceMarker& a, const SliceMarker& b) { return a.fileSample < b.fileSample; });
    all.erase (std::unique (all.begin(), all.end(), [] (const SliceMarker& a, const SliceMarker& b) { return a.fileSample == b.fileSample; }), all.end());

    int firstCount = editSlices.empty() ? 1 : editFirstSliceCount;
    std::vector<SliceMarker> markers;

    for (const auto& m : all)
    {
        if (m.fileSample <= start)
            firstCount = m.playCount;
        else if (m.fileSample < end)
            markers.push_back (m);
    }

    if ((int) markers.size() > maxRuns - 2)
        markers.resize ((size_t) (maxRuns - 2));

    // run i starts at marker i-1 (or the region start) and plays that slice's count
    juce::int64 runStart = start;
    int runCount = firstCount;
    auto pushRun = [&] (juce::int64 from, juce::int64 to, int count)
    {
        if (to <= from || l.numRuns >= maxRuns)
            return;

        const int index = l.numRuns;

        for (const auto& r : resolvedRuns)
            if (r.fileStart == from)
                count = r.count;   // fixed by a devamp (keyed by file position: marker edits in front do not shift it)

        if (stopAfterStart >= 0 && from > stopAfterStart)
            count = 0;             // devamp with stop: nothing after the loop point

        l.runs[index] = { from, to - from, count };
        ++l.numRuns;
    };

    for (const auto& m : markers)
    {
        pushRun (runStart, m.fileSample, runCount);
        runStart = m.fileSample;
        runCount = m.playCount;
    }

    pushRun (runStart, end, runCount);

    if (markers.empty() && l.numRuns == 1 && firstCount == 1)
    {
        // no slices: fold the sequence count into the single run so the old pass semantics hold
        bool resolved = false;

        for (const auto& r : resolvedRuns)
            if (r.fileStart == start)
                resolved = true;

        if (! resolved)
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

juce::int64 RegionLoopSource::passEndFor (juce::int64 virtualPosition) const noexcept
{
    const auto l = snapshot();
    const auto loc = l.locate (virtualPosition);

    if (loc.beyondEnd || loc.run >= l.numRuns)
        return -1;

    const auto& run = l.runs[loc.run];

    if (run.count < 0)
        return virtualPosition - loc.offset + run.length;   // the endless run: this pass of it

    if (l.sequenceCount < 0)
        return (juce::int64) (loc.sequencePass + 1) * l.sequenceLength();   // finite runs, endless sequence: this sequence pass

    return virtualPosition - loc.offset + run.length;
}

juce::int64 RegionLoopSource::finishCurrentPass (juce::int64 virtualPosition, bool stopAfterThisPass) noexcept
{
    const auto l = snapshot();
    const auto loc = l.locate (virtualPosition);

    if (loc.beyondEnd || loc.run >= l.numRuns)
        return -1;

    const auto& run = l.runs[loc.run];
    auto resolve = [&] (juce::int64 fileStart, int count)
    {
        for (auto& r : resolvedRuns)
            if (r.fileStart == fileStart)
            {
                r.count = count;
                return;
            }

        resolvedRuns.push_back ({ fileStart, count });
    };

    juce::int64 boundary;

    if (run.count < 0)
    {
        resolve (run.fileStart, loc.pass + 1);
        boundary = virtualPosition - loc.offset + run.length;
    }
    else if (l.sequenceCount < 0)
    {
        resolvedSequenceCount = loc.sequencePass + 1;
        boundary = (juce::int64) (loc.sequencePass + 1) * l.sequenceLength();
    }
    else if (! stopAfterThisPass)
    {
        return -1;   // nothing endless to finish
    }
    else
    {
        resolve (run.fileStart, loc.pass + 1);   // finite loop: still end after this pass
        resolvedSequenceCount = loc.sequencePass + 1;
        boundary = virtualPosition - loc.offset + run.length;
    }

    if (stopAfterThisPass)
    {
        stopAfterStart = run.fileStart;
        resolvedSequenceCount = loc.sequencePass + 1;

        if (run.count >= 0 || l.sequenceCount >= 0)
            resolve (run.fileStart, loc.pass + 1);

        boundary = virtualPosition - loc.offset + run.length;   // with the later runs skipped the pass end is the end
    }

    rebuildLayout();
    return boundary;
}

void RegionLoopSource::setEndAfterPass (int pass) noexcept
{
    const auto l = snapshot();
    const juce::int64 fileStart = l.numRuns > 0 ? l.runs[0].fileStart : l.regionStart;
    bool found = false;

    for (auto& r : resolvedRuns)
        if (r.fileStart == fileStart)
        {
            r.count = std::max (0, pass) + 1;
            found = true;
        }

    if (! found)
        resolvedRuns.push_back ({ fileStart, std::max (0, pass) + 1 });

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

juce::int64 RegionLoopSource::virtualPositionFor (juce::int64 fileSample, const Location& where) const noexcept
{
    const auto l = snapshot();

    // the same sequence pass as before (when the sequence is finite and repeats)
    juce::int64 base = 0;

    if (! l.hasEndlessRun())
    {
        const auto seqLen = l.sequenceLength();
        const int pass = l.sequenceCount < 0 ? std::max (0, where.sequencePass) : std::clamp (where.sequencePass, 0, std::max (0, l.sequenceCount - 1));
        base = seqLen > 0 ? seqLen * (juce::int64) pass : 0;
    }

    for (int i = 0; i < l.numRuns; ++i)
    {
        const auto& r = l.runs[i];

        if (r.count == 0 || r.length <= 0)
            continue;

        if (fileSample >= r.fileStart && fileSample < r.fileStart + r.length)
        {
            const int pass = r.count < 0 ? std::max (0, where.pass) : std::clamp (where.pass, 0, r.count - 1);
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
