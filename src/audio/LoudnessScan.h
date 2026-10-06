#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace gocue
{

/** What a scan found in the played region of a file. */
struct LoudnessScanResult
{
    bool valid = false;           // the region was long enough (LoudnessScan::minSeconds) and not silent
    double integratedLufs = 0.0;  // BS.1770-4 integrated loudness of the region as outputs 1-2 hear it (a mono file on both)
    double peakDb = -100.0;       // the recurring sample peak: the 99th percentile of the region's 100 ms block peaks, dBFS
    double seconds = 0.0;         // the region's length
    bool failed = false;          // the file could not be opened or read: measured again next session, not before
};

/** The gain that brings a measured region to the target: target - integrated, at least -20 dB; a boost at most +12 dB
    and at most what puts the recurring peak 2 dB over full scale (the master limiter then works no more than 3 dB on
    it - one stray spike is the limiter's job). 0 dB for a region too short or too quiet to say. */
double loudnessMatchDb (const LoudnessScanResult& result, double targetLufs) noexcept;

/** Measures the loudness of cue regions offline, so a cue can start at the target level instead of being ridden there.
    One low-priority thread (below every playback thread), entirely apart from playback: its own file readers, no lock the audio thread ever takes,
    and while cues play (setBusy) it reads slowly so playback's disk and network reads come first. A region is measured
    once: results are cached by file and region, in memory and in a JSON file beside the settings. A result is answered
    only once confirmed this session - measured, or its file's size and modification time unchanged, checked on this
    thread - and the answered files are looked at again every few seconds, so a file saved over while the app runs is
    measured again rather than started at the old file's match. Message-thread API; results arrive through onResults on
    the message thread. */
class LoudnessScan : private juce::Thread,
                     private juce::AsyncUpdater
{
public:
    static constexpr double minSeconds = 3.0;       // shorter regions (a short effect) are not matched
    static constexpr int maxCacheEntries = 5000;
    static constexpr int recheckIdleMs = 5000, recheckBusyMs = 15000;   // how often answered files are looked at again

    LoudnessScan (juce::AudioFormatManager& formats, const juce::File& cacheFile);
    ~LoudnessScan() override;

    /** The result for this file region once confirmed this session (measured, or the file unchanged since an earlier
        session measured it); otherwise nothing, and the region is queued. Never touches the disk. Message thread. */
    std::optional<LoudnessScanResult> lookup (const juce::File& file, double regionStart, double regionEnd);
    /** How often the files already answered are looked at again (size and modification time), with no cue playing and
        with cues playing. Tests shorten it. Any thread. */
    void setRecheckInterval (int idleMs, int busyMs) noexcept
    {
        recheckIdle.store (juce::jmax (10, idleMs), std::memory_order_relaxed);
        recheckBusy.store (juce::jmax (10, busyMs), std::memory_order_relaxed);
    }
    /** Cues are playing: the scan reads slowly. Any thread. */
    void setBusy (bool busy) noexcept { busyFlag.store (busy, std::memory_order_relaxed); }
    /** Regions queued or being measured. Message thread. */
    int getPendingCount() const;
    /** Regions actually read from disk since this scan started (tests: a cached, unchanged file is not read again). */
    int getMeasuredCount() const noexcept { return measuredCount.load (std::memory_order_relaxed); }
    /** Called on the message thread when results have changed. */
    std::function<void()> onResults;

    /** Measures a region on the calling thread. 'pace' runs between 0.5 s chunks (the worker sleeps there while busy);
        'keepGoing' false stops it (no result). */
    static LoudnessScanResult measure (juce::AudioFormatManager& formats, const juce::File& file, double regionStart,
                                       double regionEnd, const std::function<bool()>& keepGoing = {},
                                       const std::function<void()>& pace = {});

private:
    struct Key
    {
        juce::String path;
        juce::int64 startMs = 0, endMs = -1;   // the region, in whole milliseconds (-1 = the end of the file)
        bool operator< (const Key& other) const noexcept;
    };

    struct Entry
    {
        LoudnessScanResult result;
        juce::int64 size = -1, modified = 0;   // the file when it was measured
        juce::int64 used = 0;                   // last asked for (ms since epoch): the oldest go first when full
        bool retry = false;                     // could not be read: saved as of unknown size, so the next session reads it again
    };

    static Key keyFor (const juce::File& file, double regionStart, double regionEnd);
    void run() override;
    void handleAsyncUpdate() override;
    void loadCache();
    void saveCache();
    /** Looks at the answered files again (size and modification time) and queues those that changed. Scan thread. */
    void recheckFiles();

    juce::AudioFormatManager& formats;
    const juce::File cacheFile;
    juce::CriticalSection lock;             // entries / queue / checked / inQueue - never taken by the audio thread
    std::map<Key, Entry> entries;
    std::deque<Key> queue;
    std::set<Key> inQueue, checked;         // queued now; confirmed this session (measured, or the file is unchanged)
    std::atomic<bool> busyFlag { false };
    std::atomic<int> measuring { 0 };
    std::atomic<int> measuredCount { 0 };
    std::atomic<int> recheckIdle { recheckIdleMs }, recheckBusy { recheckBusyMs };
    bool dirty = false;
    juce::int64 lastRecheck = 0;            // scan thread: when the answered files were last looked at
    juce::WaitableEvent work;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessScan)
};

} // namespace gocue
