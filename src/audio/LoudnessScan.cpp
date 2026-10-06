#include "audio/LoudnessScan.h"

#include "audio/LoudnessMeter.h"
#include "model/SafeFileWrite.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace gocue
{

double loudnessMatchDb (const LoudnessScanResult& result, double targetLufs) noexcept
{
    if (! result.valid || ! std::isfinite (result.integratedLufs) || ! std::isfinite (targetLufs))
        return 0.0;

    double gain = targetLufs - result.integratedLufs;

    if (gain > 0.0)
        gain = juce::jmin (gain, juce::jmax (0.0, juce::jmin (12.0, 2.0 - result.peakDb)));

    return juce::jmax (-20.0, gain);
}

bool LoudnessScan::Key::operator< (const Key& other) const noexcept
{
    if (const int c = path.compare (other.path); c != 0)
        return c < 0;

    if (startMs != other.startMs)
        return startMs < other.startMs;

    return endMs < other.endMs;
}

LoudnessScan::Key LoudnessScan::keyFor (const juce::File& file, double regionStart, double regionEnd)
{
    Key key;
    key.path = file.getFullPathName();
    key.startMs = (juce::int64) std::llround (juce::jmax (0.0, regionStart) * 1000.0);
    key.endMs = regionEnd < 0.0 ? -1 : (juce::int64) std::llround (regionEnd * 1000.0);
    return key;
}

LoudnessScan::LoudnessScan (juce::AudioFormatManager& formatsToUse, const juce::File& cacheFileToUse)
    : juce::Thread ("Enqueue loudness scan"), formats (formatsToUse), cacheFile (cacheFileToUse)
{
    loadCache();
    lastRecheck = juce::Time::currentTimeMillis();
    // below every playback thread (the audio callback, the read-ahead), yet not 'background': on Windows that is IDLE,
    // which a busy machine starves (a 20 s file took 2.6 s, while playing 14.5 s)
    startThread (juce::Thread::Priority::low);
}

LoudnessScan::~LoudnessScan()
{
    signalThreadShouldExit();
    work.signal();
    stopThread (5000);
    cancelPendingUpdate();

    if (dirty)
        saveCache();
}

bool LoudnessScan::stopForExit (int ms)
{
    signalThreadShouldExit();
    work.signal();
    notify();

    if (waitForThreadToExit (ms))
        return true;

    onResults = nullptr;   // nothing reaches the owner from here on
    cancelPendingUpdate();
    saveCache();           // the thread takes the lock only for moments, never while it reads
    return false;
}

std::optional<LoudnessScanResult> LoudnessScan::lookup (const juce::File& file, double regionStart, double regionEnd)
{
    if (file == juce::File())
        return {};

    const auto key = keyFor (file, regionStart, regionEnd);
    std::optional<LoudnessScanResult> found;
    const juce::ScopedLock sl (lock);

    if (const auto it = entries.find (key); it != entries.end())
    {
        if (it->second.used < sessionStart)
            dirty = true;   // its first use this session: kept on disk (the oldest go first when the cache is full)

        it->second.used = juce::Time::currentTimeMillis();

        if (checked.count (key) > 0)
            found = it->second.result;   // confirmed this session: never an earlier answer for a file changed since
    }

    if (checked.count (key) == 0 && inQueue.insert (key).second)
    {
        queue.push_back (key);
        work.signal();
    }

    return found;
}

int LoudnessScan::getPendingCount() const
{
    const juce::ScopedLock sl (lock);
    return (int) queue.size() + measuring.load (std::memory_order_relaxed);
}

LoudnessScanResult LoudnessScan::measure (juce::AudioFormatManager& formatsToUse, const juce::File& file, double regionStart,
                                          double regionEnd, const std::function<bool()>& keepGoing,
                                          const std::function<void()>& pace)
{
    LoudnessScanResult result;
    std::unique_ptr<juce::AudioFormatReader> reader (formatsToUse.createReaderFor (file));

    if (reader == nullptr || ! (reader->sampleRate > 0.0) || reader->numChannels == 0)
    {
        result.failed = true;   // not opened (a share that did not answer, a format this PC cannot decode): next session again
        return result;
    }

    if (reader->lengthInSamples <= 0)
        return result;          // empty: nothing to match

    const double rate = reader->sampleRate;
    const juce::int64 total = reader->lengthInSamples;
    const juce::int64 start = juce::jlimit<juce::int64> (0, total, (juce::int64) std::llround (juce::jmax (0.0, regionStart) * rate));
    const juce::int64 end = regionEnd >= 0.0 ? juce::jlimit<juce::int64> (start, total, (juce::int64) std::llround (regionEnd * rate)) : total;
    result.seconds = (double) (end - start) / rate;

    if (result.seconds < minSeconds)
        return result;

    // what outputs 1-2 hear with the cue's default routing: a mono file on both, else the first two channels
    const bool mono = reader->numChannels == 1;
    const int channels = mono ? 1 : 2;
    livemix::KWeightingFilter weightLeft, weightRight;
    weightLeft.prepare (rate);
    weightRight.prepare (rate);
    livemix::LoudnessStats stats;
    std::vector<float> blockPeaks;
    blockPeaks.reserve ((size_t) (result.seconds * 10.0) + 1);

    const int blockLength = juce::jmax (1, (int) std::llround (rate * livemix::LoudnessStats::subBlockSeconds));
    const int chunkLength = blockLength * 5;   // 0.5 s a read
    juce::AudioBuffer<float> buffer (channels, chunkLength);
    double sumLeft = 0.0, sumRight = 0.0;
    float peak = 0.0f;
    int inBlock = 0;

    for (juce::int64 position = start; position < end; position += chunkLength)
    {
        if (keepGoing && ! keepGoing())
            return {};

        const int count = (int) juce::jmin<juce::int64> (chunkLength, end - position);

        if (! reader->read (&buffer, 0, count, position, true, ! mono))
        {
            LoudnessScanResult unread;
            unread.failed = true;
            return unread;
        }

        const float* left = buffer.getReadPointer (0);
        const float* right = mono ? nullptr : buffer.getReadPointer (1);

        for (int i = 0; i < count; ++i)
        {
            const double l = weightLeft.process (left[i]);
            sumLeft += l * l;
            peak = juce::jmax (peak, std::abs (left[i]));

            if (right != nullptr)
            {
                const double r = weightRight.process (right[i]);
                sumRight += r * r;
                peak = juce::jmax (peak, std::abs (right[i]));
            }

            if (++inBlock == blockLength)
            {
                const double meanLeft = sumLeft / blockLength;
                stats.addSubBlock (meanLeft, right != nullptr ? sumRight / blockLength : meanLeft);
                blockPeaks.push_back (std::isfinite (peak) ? peak : 0.0f);
                sumLeft = sumRight = 0.0;
                peak = 0.0f;
                inBlock = 0;
            }
        }

        if (pace)
            pace();
    }

    const auto integrated = stats.integrated();

    if (! integrated.valid || blockPeaks.empty())
        return result;

    // one stray spike is the limiter's job: the peak that recurs is the 99th percentile of the block peaks
    const size_t at = (size_t) std::floor (0.99 * (double) (blockPeaks.size() - 1));
    std::nth_element (blockPeaks.begin(), blockPeaks.begin() + (std::ptrdiff_t) at, blockPeaks.end());
    const float recurring = blockPeaks[at];
    result.integratedLufs = integrated.value;
    result.peakDb = recurring > 0.0f ? 20.0 * std::log10 ((double) recurring) : -100.0;
    result.valid = true;
    return result;
}

void LoudnessScan::run()
{
    while (! threadShouldExit())
    {
        // a few answered files looked at again before each region: a busy queue never holds the recheck back
        const bool rechecking = recheckSome();

        Key key;
        bool have = false;
        bool save = false;

        {
            const juce::ScopedLock sl (lock);

            if (! queue.empty())
            {
                key = queue.front();
                queue.pop_front();
                measuring.store (1, std::memory_order_relaxed);
                have = true;
            }
            else
            {
                save = dirty;
                dirty = false;
            }
        }

        if (! have)
        {
            if (save)
                saveCache();   // the queue ran dry: keep what was learned

            if (! rechecking)
                work.wait (msToNextRecheck());

            continue;
        }

        const juce::File file (key.path);
        const juce::int64 size = file.existsAsFile() ? file.getSize() : -1;   // on this thread: a share may be slow to answer
        const juce::int64 modified = size >= 0 ? file.getLastModificationTime().toMilliseconds() : 0;
        bool known = false;

        {
            const juce::ScopedLock sl (lock);

            if (const auto it = entries.find (key); it != entries.end())
                known = size >= 0 && it->second.size == size && it->second.modified == modified;
        }

        if (! known)
        {
            LoudnessScanResult result;

            if (size >= 0)
            {
                measuredCount.fetch_add (1, std::memory_order_relaxed);
                result = measure (formats, file, (double) key.startMs / 1000.0, key.endMs < 0 ? -1.0 : (double) key.endMs / 1000.0,
                                  [this] { return ! threadShouldExit(); },
                                  [this]
                                  {
                                      recheckSome();   // a long region does not hold the recheck back either
                                      if (busyFlag.load (std::memory_order_relaxed))
                                          wait (50);   // ~10x real time while cues play
                                  });

                if (threadShouldExit())
                    break;
            }

            const juce::ScopedLock sl (lock);
            auto& entry = entries[key];
            entry.result = result;
            entry.size = size;          // a missing file (-1) is asked about again next session
            entry.modified = modified;
            entry.used = juce::Time::currentTimeMillis();
            entry.retry = result.failed;   // saved as of unknown size: the next session reads it again
            dirty = true;
        }

        {
            const juce::ScopedLock sl (lock);
            checked.insert (key);
            inQueue.erase (key);
            measuring.store (0, std::memory_order_relaxed);
        }

        triggerAsyncUpdate();
    }
}

int LoudnessScan::msToNextRecheck() const noexcept
{
    const juce::int64 interval = busyFlag.load (std::memory_order_relaxed) ? recheckBusy.load (std::memory_order_relaxed)
                                                                            : recheckIdle.load (std::memory_order_relaxed);
    return (int) juce::jlimit<juce::int64> (10, 2000, interval - (juce::Time::currentTimeMillis() - lastRecheck));
}

bool LoudnessScan::recheckSome()
{
    if (recheckNext >= recheckPaths.size())
    {
        const juce::int64 interval = busyFlag.load (std::memory_order_relaxed) ? recheckBusy.load (std::memory_order_relaxed)
                                                                                : recheckIdle.load (std::memory_order_relaxed);

        if (juce::Time::currentTimeMillis() - lastRecheck < interval)
            return false;

        lastRecheck = juce::Time::currentTimeMillis();
        recheckPaths.clear();
        recheckNext = 0;
        const juce::ScopedLock sl (lock);

        for (const auto& key : checked)   // ordered by path: each file once
            if (recheckPaths.empty() || recheckPaths.back() != key.path)
                recheckPaths.push_back (key.path);
    }

    bool changed = false;

    for (int n = 0; n < recheckSliceFiles && recheckNext < recheckPaths.size() && ! threadShouldExit(); ++n)
    {
        const juce::String path = recheckPaths[recheckNext++];
        const juce::File file (path);
        const juce::int64 size = file.existsAsFile() ? file.getSize() : -1;
        const juce::int64 modified = size >= 0 ? file.getLastModificationTime().toMilliseconds() : 0;

        {
            const juce::ScopedLock sl (lock);
            Key first;
            first.path = path;
            first.startMs = first.endMs = std::numeric_limits<juce::int64>::min();
            std::vector<Key> regions;
            bool differs = false;

            for (auto it = checked.lower_bound (first); it != checked.end() && it->path == path; ++it)
            {
                regions.push_back (*it);
                const auto entry = entries.find (*it);
                differs = differs || entry == entries.end() || entry->second.size != size || entry->second.modified != modified;
            }

            if (differs)
            {
                for (const auto& key : regions)   // every region of the file: no answer until measured again - and first
                {
                    checked.erase (key);

                    if (inQueue.insert (key).second)
                        queue.push_front (key);
                }

                changed = true;
            }
        }

        if (busyFlag.load (std::memory_order_relaxed))
            wait (20);   // cues playing: a light touch on the disk or the share
    }

    if (changed && ! threadShouldExit())
        triggerAsyncUpdate();   // the main component drops those matches until they are measured again

    return recheckNext < recheckPaths.size();
}

void LoudnessScan::handleAsyncUpdate()
{
    if (onResults)
        onResults();
}

void LoudnessScan::loadCache()
{
    if (! cacheFile.existsAsFile())
        return;

    const auto parsed = juce::JSON::parse (cacheFile.loadFileAsString());
    const auto* list = parsed.getProperty ("entries", juce::var()).getArray();

    if ((int) parsed.getProperty ("version", 0) != 1 || list == nullptr)
        return;

    const juce::ScopedLock sl (lock);

    for (const auto& item : *list)
    {
        Key key;
        key.path = item.getProperty ("path", juce::String()).toString();
        key.startMs = (juce::int64) item.getProperty ("start", 0);
        key.endMs = (juce::int64) item.getProperty ("end", -1);

        if (key.path.isEmpty())
            continue;

        Entry entry;
        entry.size = (juce::int64) item.getProperty ("size", -1);
        entry.modified = (juce::int64) item.getProperty ("modified", 0);
        entry.used = (juce::int64) item.getProperty ("used", 0);
        entry.result.valid = (bool) item.getProperty ("valid", false);
        entry.result.integratedLufs = (double) item.getProperty ("lufs", 0.0);
        entry.result.peakDb = (double) item.getProperty ("peak", -100.0);
        entry.result.seconds = (double) item.getProperty ("seconds", 0.0);

        if (std::isfinite (entry.result.integratedLufs) && std::isfinite (entry.result.peakDb))
            entries[key] = entry;
    }
}

void LoudnessScan::saveCache()
{
    juce::Array<juce::var> list;

    {
        const juce::ScopedLock sl (lock);
        std::vector<std::pair<juce::int64, const std::pair<const Key, Entry>*>> byUse;

        for (const auto& item : entries)
            byUse.emplace_back (item.second.used, &item);

        // the most recently used first, at most maxCacheEntries of them
        std::sort (byUse.begin(), byUse.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });

        for (size_t i = 0; i < byUse.size() && i < (size_t) maxCacheEntries; ++i)
        {
            const auto& [key, entry] = *byUse[i].second;
            auto* object = new juce::DynamicObject();
            object->setProperty ("path", key.path);
            object->setProperty ("start", key.startMs);
            object->setProperty ("end", key.endMs);
            object->setProperty ("size", entry.retry ? (juce::int64) -1 : entry.size);   // unreadable: measured again next session
            object->setProperty ("modified", entry.modified);
            object->setProperty ("used", entry.used);
            object->setProperty ("valid", entry.result.valid);
            object->setProperty ("lufs", entry.result.integratedLufs);
            object->setProperty ("peak", entry.result.peakDb);
            object->setProperty ("seconds", entry.result.seconds);
            list.add (juce::var (object));
        }
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("entries", list);
    SafeFileWrite::writeTextVerified (cacheFile, juce::JSON::toString (juce::var (root), true));
}

} // namespace gocue
