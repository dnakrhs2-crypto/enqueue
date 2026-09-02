#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace gocue
{

/** Disk read-ahead: a background thread keeps a ring of upstream audio filled from the play position on,
    and getNextAudioBlock() serves from the ring (silence for anything not cached yet).

    Unlike juce::BufferingAudioSource this source can be told that the upstream's *content* changed
    (a live trim / slice edit re-maps the virtual timeline): invalidate() drops the whole cache and refills
    from the new position at once, so the old layout's audio is never heard for the same positions. */
class ReadAheadSource : public juce::PositionableAudioSource,
                        private juce::TimeSliceClient
{
public:
    /** @param upstream  the source to read ahead from (not owned; must outlive this). */
    ReadAheadSource (juce::PositionableAudioSource& upstream, juce::TimeSliceThread& thread, int numSamplesToBuffer, int numChannels);
    ~ReadAheadSource() override;

    /** Prefills up to a quarter second (or half the ring) before returning. */
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override;

    /** Moves the play position; what the ring already holds for the new position stays valid. */
    void setNextReadPosition (juce::int64 newPosition) override;
    juce::int64 getNextReadPosition() const override { return playPos.load (std::memory_order_relaxed); }
    juce::int64 getTotalLength() const override { return upstream.getTotalLength(); }
    bool isLooping() const override { return false; }

    /** The upstream's content changed: forgets everything cached and refills from 'fromPosition' right away
        (on the calling thread, one chunk) so playback continues without a gap. */
    void invalidate (juce::int64 fromPosition);

    /** Samples cached and playable from the current play position (tests). */
    int getNumSamplesReady() const;

private:
    int useTimeSlice() override;
    /** Reads the next chunk the ring is missing. False when nothing was needed. */
    bool fillChunk();
    void readIntoRing (juce::int64 start, int length);

    juce::PositionableAudioSource& upstream;
    juce::TimeSliceThread& thread;
    const int numChannels;
    const int ringSize;
    juce::AudioBuffer<float> ring;
    juce::CriticalSection rangeLock;   // validStart / validEnd / playPos consistency
    juce::CriticalSection readLock;    // one upstream read at a time
    juce::int64 validStart = 0, validEnd = 0;   // absolute positions cached in the ring
    std::atomic<juce::int64> playPos { 0 };
    std::atomic<bool> prepared { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReadAheadSource)
};

} // namespace gocue
