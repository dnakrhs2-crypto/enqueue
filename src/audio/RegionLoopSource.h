#pragma once

#include "model/Envelope.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <limits>
#include <memory>
#include <vector>

namespace gocue
{

/** Streams a trimmed region of an audio file, optionally split into slices (QLab "slices"), each played
    its own number of times (0 = skipped, -1 = forever), the whole sequence 'playCount' times (or forever),
    with the cue's integrated fade envelope applied in file time.

    Positions live on a virtual timeline in file samples that concatenates every pass of every slice:
    run 0 pass 0, run 0 pass 1, ..., run 1 pass 0, ... An endless run keeps the timeline inside it (positions
    beyond its start cycle through it) until finishCurrentPass() resolves it to "this pass is the last", after
    which the later runs follow. The layout (region, runs, counts) may change while playing (live trim, devamp);
    it is published with a sequence counter so a reader never sees a torn layout.
    The envelope can be replaced while playing (setLiveEnvelope): a live edit is heard at once.

    Reads happen on whichever thread pulls audio: the disk read-ahead thread in the app, or the caller in
    offline tests. Only that thread touches the reader. */
class RegionLoopSource : public juce::PositionableAudioSource
{
public:
    /** A slice marker: the slice starting at this file sample plays 'playCount' times (0 = skip, -1 = forever). */
    struct SliceMarker
    {
        juce::int64 fileSample = 0;
        int playCount = 1;
    };

    /** Where a virtual position falls. */
    struct Location
    {
        int run = 0;                 // index into the layout's runs
        int pass = 0;                // 0-based pass within that run
        int sequencePass = 0;        // 0-based pass of the whole slice sequence
        juce::int64 offset = 0;      // samples into the run's slice
        juce::int64 fileSample = 0;  // absolute file position
        bool beyondEnd = false;
    };

    static constexpr int maxRuns = 66;

    /** Takes ownership of the reader. */
    explicit RegionLoopSource (std::unique_ptr<juce::AudioFormatReader> readerToUse);
    ~RegionLoopSource() override;

    double getFileSampleRate() const noexcept { return reader->sampleRate; }
    juce::int64 getFileLength() const noexcept { return reader->lengthInSamples; }
    int getFileNumChannels() const noexcept { return (int) reader->numChannels; }

    /** Region in file samples. endSample < 0 or beyond the file means the file end. Message thread. */
    void setRegion (juce::int64 startSample, juce::int64 endSample) noexcept;
    /** Whole-sequence play count (1 = once) or forever. Message thread. */
    void setPlayCount (int count, bool shouldLoopForever) noexcept;
    /** Slice markers inside the region (sorted, deduplicated here); an empty list = one slice.
        'firstSliceCount' is the play count of the slice before the first marker. Message thread. */
    void setSlices (const std::vector<SliceMarker>& markers, int firstSliceCount = 1);
    /** Ends the endless run (or the endless sequence) that 'virtualPosition' is in after its current pass
        (devamp: "finish this loop"). Returns the virtual position where that pass ends in the new layout,
        or -1 when there was nothing endless to finish (and stopAfterThisPass was off). Message thread. */
    juce::int64 finishCurrentPass (juce::int64 virtualPosition, bool stopAfterThisPass = false) noexcept;
    /** Where the pass that 'virtualPosition' is in ends (the run's pass, or the sequence pass when the runs are
        finite but the sequence is endless); -1 beyond the end. What finishCurrentPass() would resolve to. */
    juce::int64 passEndFor (juce::int64 virtualPosition) const noexcept;
    /** Compatibility: ends run 0 after the given 0-based pass. */
    void setEndAfterPass (int pass) noexcept;
    /** Set before playback starts. */
    void setEnvelope (const Envelope& newEnvelope);
    /** Live envelope edit: swapped in under a spin lock, heard from the next block. Message thread. */
    void setLiveEnvelope (Envelope newEnvelope);

    /** Consistent snapshot of the region (start, length) in file samples. Any thread. */
    void getRegion (juce::int64& startSample, juce::int64& lengthSamples) const noexcept;
    juce::int64 getRegionStart() const noexcept;
    juce::int64 getRegionLength() const noexcept;
    /** True while the timeline has no end yet (an unresolved endless run or sequence). */
    bool isInfinite() const noexcept;
    Location locate (juce::int64 virtualPosition) const noexcept;
    /** Length in file samples of one pass of run 'index' (0 when out of range). */
    juce::int64 getRunLength (int index) const noexcept;
    /** Pass index (within its run) of a virtual position. */
    int getPassIndexFor (juce::int64 position) const noexcept { return locate (position).pass; }
    /** Where a virtual position falls (run, pass, file sample). */
    Location locationFor (juce::int64 position) const noexcept { return locate (position); }
    /** File offset from the region start of a virtual position. */
    juce::int64 getOffsetFor (juce::int64 position) const noexcept;
    /** The first virtual position that plays 'fileSample' in the run pass / sequence pass closest to 'where'
        (live trim / slice edits keep the audible place and the pass). */
    juce::int64 virtualPositionFor (juce::int64 fileSample, const Location& where) const noexcept;
    /** True once a read went past the end of the final pass. */
    bool hasReachedEnd() const noexcept { return reachedEnd.load (std::memory_order_relaxed); }

    /** Reported total length of an endless source. */
    static constexpr juce::int64 infiniteLength = std::numeric_limits<juce::int64>::max() / 4;

    //==========================================================================
    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override;

    void setNextReadPosition (juce::int64 newPosition) override { nextPosition.store (newPosition, std::memory_order_relaxed); }
    juce::int64 getNextReadPosition() const override { return nextPosition.load (std::memory_order_relaxed); }
    /** Every pass of every slice, times the sequence count; infiniteLength while endless. */
    juce::int64 getTotalLength() const override;
    bool isLooping() const override { return false; }   // looping is handled internally

private:
    struct Run
    {
        juce::int64 fileStart = 0, length = 0;
        int count = 1;   // -1 = forever
    };

    struct Layout
    {
        Run runs[maxRuns];
        int numRuns = 0;
        int sequenceCount = 1;   // -1 = forever
        juce::int64 regionStart = 0, regionLength = 0;

        juce::int64 sequenceLength() const noexcept;   // finite runs only
        bool hasEndlessRun() const noexcept;
        juce::int64 totalLength() const noexcept;
        Location locate (juce::int64 pos) const noexcept;
    };

    void rebuildLayout();
    void publish (const Layout& l) noexcept;
    Layout snapshot() const noexcept;
    void applyEnvelope (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                        juce::int64 offsetInRegion, juce::int64 regionLen) const;

    std::unique_ptr<juce::AudioFormatReader> reader;

    // editing state (message thread)
    juce::int64 editRegionStart = 0, editRegionEnd = 0;
    int editPlayCount = 1;
    bool editLoopForever = false;
    std::vector<SliceMarker> editSlices;
    int editFirstSliceCount = 1;
    /** Counts fixed by a devamp, keyed by the run's file start so a marker edit in front does not move them. */
    struct Resolved { juce::int64 fileStart; int count; };
    std::vector<Resolved> resolvedRuns;
    int resolvedSequenceCount = -1;
    juce::int64 stopAfterStart = -1;   // devamp with stop: every run starting after this file position is skipped

    // published layout: a tiny copy under a spin lock (the writer holds it for a memcpy; readers on the
    // read-ahead / audio thread wait microseconds at most)
    Layout published;
    mutable juce::SpinLock layoutLock;

    std::atomic<juce::int64> nextPosition { 0 };
    std::atomic<bool> reachedEnd { false };
    Envelope envelope;                    // read on the audio thread under envelopeLock
    mutable juce::SpinLock envelopeLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RegionLoopSource)
};

} // namespace gocue
