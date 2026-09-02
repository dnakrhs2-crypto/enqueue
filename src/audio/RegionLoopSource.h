#pragma once

#include "model/Envelope.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <limits>
#include <memory>

namespace gocue
{

/** Streams a trimmed region of an audio file 'playCount' times (or forever), with the cue's
    integrated fade envelope applied in file time on every pass.

    Positions live on a virtual timeline in file samples: pass * regionLength + offset. Region and
    loop settings may change while playing (live trim); the region pair is published with a
    sequence counter so a reader never sees a torn start / length combination.
    The envelope is fixed for the life of the source (QLab: envelope edits apply on the next start).

    Reads happen on whichever thread pulls audio: the disk read-ahead thread in the app, or the
    caller in offline tests. Only that thread touches the reader. */
class RegionLoopSource : public juce::PositionableAudioSource
{
public:
    /** Takes ownership of the reader. */
    explicit RegionLoopSource (std::unique_ptr<juce::AudioFormatReader> readerToUse);
    ~RegionLoopSource() override;

    double getFileSampleRate() const noexcept { return reader->sampleRate; }
    juce::int64 getFileLength() const noexcept { return reader->lengthInSamples; }
    int getFileNumChannels() const noexcept { return (int) reader->numChannels; }

    /** Region in file samples. endSample < 0 or beyond the file means the file end. Any thread. */
    void setRegion (juce::int64 startSample, juce::int64 endSample) noexcept;
    void setPlayCount (int count, bool shouldLoopForever) noexcept;
    /** Ends playback once the given 0-based pass has completed (devamp: "finish this loop"). Any thread. */
    void setEndAfterPass (int pass) noexcept;
    /** Set before playback starts; not read live. */
    void setEnvelope (const Envelope& newEnvelope);

    /** Consistent snapshot of the region (start, length) in file samples. Any thread. */
    void getRegion (juce::int64& startSample, juce::int64& lengthSamples) const noexcept;
    juce::int64 getRegionStart() const noexcept;
    juce::int64 getRegionLength() const noexcept;
    bool isInfinite() const noexcept;
    int getPassIndexFor (juce::int64 position) const noexcept;
    juce::int64 getOffsetFor (juce::int64 position) const noexcept;
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
    /** regionLength * passes, or infiniteLength while looping forever. */
    juce::int64 getTotalLength() const override;
    bool isLooping() const override { return false; }   // looping is handled internally

private:
    juce::int64 totalLengthFor (juce::int64 regionLen) const noexcept;
    void applyEnvelope (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                        juce::int64 offsetInRegion, juce::int64 regionLen) const;

    std::unique_ptr<juce::AudioFormatReader> reader;
    std::atomic<juce::int64> regionStart { 0 }, regionLength { 0 }, nextPosition { 0 };
    std::atomic<juce::uint32> regionVersion { 0 };   // seqlock: odd while a write is in progress
    std::atomic<int> playCount { 1 }, endAfterPass { -1 };
    std::atomic<bool> loopForever { false }, reachedEnd { false };
    Envelope envelope;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RegionLoopSource)
};

} // namespace gocue
