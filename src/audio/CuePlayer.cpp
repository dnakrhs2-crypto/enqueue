#include "audio/CuePlayer.h"

#include <cmath>

namespace gocue
{

CuePlayer::CuePlayer (const Cue& c, juce::AudioFormatManager& formats,
                      juce::TimeSliceThread* readAheadThread, int readAheadSamples,
                      double startOffset)
    : cue (c)
{
    cue.sanitise();
    gainLinear = cue.gainLinear();
    targetGain.store (gainLinear);
    liveRate.store (cue.audio.rate);

    if (cue.file == juce::File())
    {
        errorMessage = "No audio file assigned to cue \"" + cue.name + "\"";
        return;
    }

    if (! cue.file.existsAsFile())
    {
        errorMessage = "File not found: " + cue.file.getFullPathName();
        return;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (cue.file));

    if (reader == nullptr)
    {
        errorMessage = "Unsupported or unreadable audio file: " + cue.file.getFileName();
        return;
    }

    fileSampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    cue.durationSeconds = (double) reader->lengthInSamples / fileSampleRate;   // the region maths need the real length
    cue.sanitise();

    auto regionSource = std::make_unique<RegionLoopSource> (std::move (reader));

    const auto startSample = (juce::int64) std::llround (cue.regionStart() * fileSampleRate);
    const auto endSample = cue.audio.endSeconds >= 0.0 ? (juce::int64) std::llround (cue.regionEnd() * fileSampleRate)
                                                       : regionSource->getFileLength();
    regionSource->setRegion (startSample, endSample);
    regionSource->setPlayCount (cue.audio.playCount, cue.audio.infiniteLoop);
    regionSource->setEnvelope (cue.audio.envelope);

    if (regionSource->getRegionLength() <= 0)
    {
        errorMessage = "The trimmed region of cue \"" + cue.name + "\" is empty";
        return;
    }

    startOffsetSeconds = std::max (0.0, startOffset);
    startOffsetSamples = std::max<juce::int64> (0, (juce::int64) std::llround (startOffsetSeconds * fileSampleRate));
    regionSource->setNextReadPosition (startOffsetSamples);
    source = std::move (regionSource);

    if (readAheadSamples > 0 && readAheadThread != nullptr)
    {
        // faster playback pulls more file samples per block: size the read-ahead for twice the cue's rate
        const int scale = juce::jlimit (1, 8, (int) std::ceil (cue.audio.rate * 2.0));
        readAhead = std::make_unique<juce::BufferingAudioSource> (source.get(), *readAheadThread, false, readAheadSamples * scale, 2, true);
        readAhead->setNextReadPosition (startOffsetSamples);
        resampler = std::make_unique<juce::ResamplingAudioSource> (readAhead.get(), false, 2);
    }
    else
    {
        resampler = std::make_unique<juce::ResamplingAudioSource> (source.get(), false, 2);
    }

    virtualPosition.store ((double) startOffsetSamples);
    updatePositionInfo (liveRate.load());
}

CuePlayer::~CuePlayer()
{
    if (resampler != nullptr)
        resampler->releaseResources();
}

void CuePlayer::prepare (double sampleRate, int blockSize)
{
    if (! isValid())
        return;

    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    // prepare for the fastest rate first so the resampler never has to grow its buffer on the audio thread
    resampler->setResamplingRatio (ratioFor (AudioCueData::maxRate));
    resampler->prepareToPlay (juce::jmax (1, blockSize), currentSampleRate);   // also prefills the read-ahead
    lastRatio = ratioFor (liveRate.load());
    resampler->setResamplingRatio (lastRatio);

    envelope.prepare (currentSampleRate);
    pauseGate.prepare (currentSampleRate);
}

void CuePlayer::start()
{
    if (! isValid())
        return;

    if (! loadedNotStarted.load (std::memory_order_relaxed))
    {
        virtualPosition.store ((double) startOffsetSamples);
        elapsedOutputSamples = 0.0;
        envelope.setLevel (1.0f);
        pauseGate.setLevel (1.0f);
        updatePositionInfo (liveRate.load());
    }

    loadedNotStarted.store (false, std::memory_order_release);   // a loaded player starts from where it was prepared
}

void CuePlayer::requestStop() noexcept
{
    hardStopRequested.store (true, std::memory_order_relaxed);
    pendingFadeOutMs.store (stopDeclickMs, std::memory_order_relaxed);
}

void CuePlayer::requestFadeOut (int milliseconds) noexcept
{
    pendingFadeOutMs.store (juce::jmax (stopDeclickMs, milliseconds), std::memory_order_relaxed);
}

void CuePlayer::requestPause() noexcept
{
    resumeRequested.store (false, std::memory_order_relaxed);
    pauseRequested.store (true, std::memory_order_relaxed);
}

void CuePlayer::requestResume() noexcept
{
    pauseRequested.store (false, std::memory_order_relaxed);
    resumeRequested.store (true, std::memory_order_relaxed);
}

void CuePlayer::requestFinishCurrentPass() noexcept
{
    if (source != nullptr)
        source->setEndAfterPass (source->getPassIndexFor ((juce::int64) virtualPosition.load (std::memory_order_relaxed)));
}

void CuePlayer::setLiveRegion (double startSeconds, double endSeconds) noexcept
{
    if (source == nullptr)
        return;

    const auto startSample = (juce::int64) std::llround (std::max (0.0, startSeconds) * fileSampleRate);
    const auto endSample = endSeconds >= 0.0 ? (juce::int64) std::llround (endSeconds * fileSampleRate) : source->getFileLength();

    if (endSample - startSample < 1)   // an empty region would divide by zero; ignore the edit until it is valid again
        return;

    // keep the audible file position: re-derive pass / offset against the new region
    juce::int64 oldStart, oldLen;
    source->getRegion (oldStart, oldLen);
    const auto pos = (juce::int64) virtualPosition.load (std::memory_order_relaxed);
    const juce::int64 pass = oldLen > 0 ? pos / oldLen : 0;
    const juce::int64 filePos = oldStart + (oldLen > 0 ? pos % oldLen : 0);

    source->setRegion (startSample, endSample);

    const auto newLen = source->getRegionLength();
    const juce::int64 newPos = pass * newLen + juce::jlimit<juce::int64> (0, newLen, filePos - startSample);

    // drop the read-ahead cache: it holds audio of the old region for the same virtual positions
    if (readAhead != nullptr)
    {
        readAhead->setNextReadPosition (RegionLoopSource::infiniteLength);   // outside any cached range: invalidates it
        readAhead->setNextReadPosition (newPos);
    }
    else
    {
        source->setNextReadPosition (newPos);
    }

    pendingVirtualPosition.store (newPos, std::memory_order_release);
}

void CuePlayer::seekToFraction (double fraction) noexcept
{
    if (source == nullptr || source->isInfinite())
        return;

    const auto total = source->getTotalLength();

    if (total <= 0)
        return;

    const auto newPos = juce::jlimit<juce::int64> (0, total - 1, (juce::int64) std::llround (juce::jlimit (0.0, 1.0, fraction) * (double) total));

    if (readAhead != nullptr)
    {
        readAhead->setNextReadPosition (RegionLoopSource::infiniteLength);   // invalidate the cache
        readAhead->setNextReadPosition (newPos);
    }
    else
    {
        source->setNextReadPosition (newPos);
    }

    // elapsed time follows the new position at the current rate (the start offset counts as elapsed)
    const double rate = std::max (AudioCueData::minRate, liveRate.load (std::memory_order_relaxed));
    const double elapsedSamples = ((double) newPos / fileSampleRate / rate - startOffsetSeconds) * currentSampleRate;
    pendingElapsedSamples.store (std::max (0.0, elapsedSamples), std::memory_order_relaxed);
    pendingVirtualPosition.store (newPos, std::memory_order_release);
}

void CuePlayer::setLiveRate (double rate) noexcept
{
    liveRate.store (juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, rate), std::memory_order_relaxed);
}

void CuePlayer::setLiveGainDb (double gainDb) noexcept
{
    Cue tmp;
    tmp.gainDb = gainDb;
    targetGain.store (tmp.gainLinear(), std::memory_order_relaxed);
}

void CuePlayer::setDuckDb (double db, double rampSeconds) noexcept
{
    Cue tmp;
    tmp.gainDb = db;
    duckDb.store (db, std::memory_order_relaxed);
    duckRampSeconds.store (std::max (0.0, rampSeconds), std::memory_order_relaxed);
    duckTarget.store (tmp.gainLinear(), std::memory_order_relaxed);
}

double CuePlayer::getLengthSeconds() const noexcept
{
    if (source == nullptr)
        return 0.0;

    if (source->isInfinite())
        return -1.0;

    const double rate = std::max (AudioCueData::minRate, liveRate.load (std::memory_order_relaxed));
    return (double) source->getTotalLength() / fileSampleRate / rate;
}

double CuePlayer::getRemainingSeconds() const noexcept
{
    if (source == nullptr)
        return 0.0;

    if (source->isInfinite())
        return -1.0;

    const double rate = std::max (AudioCueData::minRate, liveRate.load (std::memory_order_relaxed));
    const double left = (double) source->getTotalLength() - virtualPosition.load (std::memory_order_relaxed);
    return std::max (0.0, left) / fileSampleRate / rate;
}

double CuePlayer::getProgressFraction() const noexcept
{
    if (! isValid())
        return 0.0;

    const auto total = source->getTotalLength();

    if (total <= 0 || total >= RegionLoopSource::infiniteLength / 2)
        return -1.0;

    return juce::jlimit (0.0, 1.0, virtualPosition.load (std::memory_order_relaxed) / (double) total);
}

void CuePlayer::updatePositionInfo (double rate) noexcept
{
    juce::ignoreUnused (rate);
    const auto total = source->getTotalLength();
    auto pos = (juce::int64) virtualPosition.load (std::memory_order_relaxed);

    if (! source->isInfinite() && total > 0)
        pos = std::min (pos, total - 1);   // during the plugin tail the playhead stays on the last sample

    positionSeconds.store (startOffsetSeconds + elapsedOutputSamples / currentSampleRate, std::memory_order_relaxed);
    filePositionSeconds.store ((double) (source->getRegionStart() + source->getOffsetFor (pos)) / fileSampleRate, std::memory_order_relaxed);
    passIndex.store (source->getPassIndexFor (pos), std::memory_order_relaxed);
}

bool CuePlayer::renderNextBlock (juce::AudioBuffer<float>& buffer, int numSamples)
{
    if (finished.load (std::memory_order_relaxed) || ! isValid())
    {
        buffer.clear (0, numSamples);
        return false;
    }

    if (const auto seek = pendingVirtualPosition.exchange (-1, std::memory_order_acq_rel); seek >= 0)
    {
        virtualPosition.store ((double) seek, std::memory_order_relaxed);

        if (const double elapsed = pendingElapsedSamples.exchange (-1.0, std::memory_order_relaxed); elapsed >= 0.0)
            elapsedOutputSamples = elapsed;

        resampler->flushBuffers();   // drop the interpolator's history so the old position is not heard after the jump
    }

    const int fadeMs = pendingFadeOutMs.exchange (-1, std::memory_order_relaxed);

    if (loadedNotStarted.load (std::memory_order_acquire))
    {
        // waiting for start(): silent, no position change; a stop request drops the loaded instance
        buffer.clear (0, numSamples);

        if (fadeMs >= 0 || hardStopRequested.load (std::memory_order_relaxed))
        {
            finished.store (true, std::memory_order_relaxed);
            return false;
        }

        return true;
    }

    if (fadeMs >= 0)
    {
        envelope.fadeOut (fadeMs);
        stopRequested.store (true, std::memory_order_relaxed);
        fadingOut.store (true, std::memory_order_relaxed);
    }

    const bool hardStop = hardStopRequested.load (std::memory_order_relaxed);

    if (inTail && hardStop)
    {
        buffer.clear (0, numSamples);
        finished.store (true, std::memory_order_relaxed);
        return false;
    }

    if (pauseRequested.exchange (false, std::memory_order_relaxed) && ! paused && ! pausing)
    {
        pauseGate.fadeOut (stopDeclickMs);
        pausing = true;
    }

    if (resumeRequested.exchange (false, std::memory_order_relaxed) && (paused || pausing))
    {
        paused = false;
        pausing = false;
        pausedFlag.store (false, std::memory_order_relaxed);
        pauseGate.fadeIn (stopDeclickMs);
    }

    auto* activeChain = chain.load (std::memory_order_acquire);
    const double rate = liveRate.load (std::memory_order_relaxed);

    if (paused && ! inTail)
    {
        // Frozen and already silent: a stop / fade request or a trim that ended before the position ends it now.
        const bool ended = virtualPosition.load (std::memory_order_relaxed) >= (double) source->getTotalLength();

        if (stopRequested.load (std::memory_order_relaxed) || hardStop || ended)
        {
            buffer.clear (0, numSamples);
            finished.store (true, std::memory_order_relaxed);
            return false;
        }

        buffer.clear (0, numSamples);   // feed silence through the chain so delays / reverbs keep their timing

        if (activeChain != nullptr)
            activeChain->process (buffer, numSamples);

        return true;
    }

    const double ratio = ratioFor (rate);

    if (! inTail)
    {
        if (! juce::approximatelyEqual (ratio, lastRatio))
        {
            resampler->setResamplingRatio (ratio);
            lastRatio = ratio;
        }

        juce::AudioSourceChannelInfo info (&buffer, 0, numSamples);
        resampler->getNextAudioBlock (info);   // silence once the source is past its end
        virtualPosition.store (virtualPosition.load (std::memory_order_relaxed) + (double) numSamples * ratio, std::memory_order_relaxed);
        elapsedOutputSamples += numSamples;

        envelope.applyToBuffer (buffer, 0, numSamples);
        pauseGate.applyToBuffer (buffer, 0, numSamples);

        if (pausing && pauseGate.hasReachedSilence())
        {
            pausing = false;
            paused = true;
            pausedFlag.store (true, std::memory_order_relaxed);
        }

        const float newGain = targetGain.load (std::memory_order_relaxed);

        if (! juce::approximatelyEqual (newGain, gainLinear))
        {
            buffer.applyGainRamp (0, numSamples, gainLinear, newGain);   // live gain change, click-free
            gainLinear = newGain;
        }
        else if (gainLinear != 1.0f)
        {
            buffer.applyGain (0, numSamples, gainLinear);
        }

        const float duckGoal = duckTarget.load (std::memory_order_relaxed);

        if (! juce::approximatelyEqual (duckGoal, duckGoalSeen))
        {
            // a new goal: a linear ramp that lands exactly after the requested time
            duckGoalSeen = duckGoal;
            duckSamplesLeft = (juce::int64) std::llround (duckRampSeconds.load (std::memory_order_relaxed) * currentSampleRate);
        }

        if (! juce::approximatelyEqual (duckGoal, duckLevel))
        {
            const int step = (int) juce::jmin<juce::int64> (numSamples, juce::jmax<juce::int64> (0, duckSamplesLeft));
            const float next = step > 0 && duckSamplesLeft > 0 ? duckLevel + (duckGoal - duckLevel) * (float) ((double) step / (double) duckSamplesLeft) : duckGoal;
            buffer.applyGainRamp (0, numSamples, duckLevel, next);
            duckSamplesLeft -= step;
            duckLevel = (duckSamplesLeft <= 0 || std::abs (next - duckGoal) < 1.0e-5f) ? duckGoal : next;
        }
        else if (duckLevel != 1.0f)
        {
            buffer.applyGain (0, numSamples, duckLevel);
        }
    }
    else
    {
        buffer.clear (0, numSamples);
    }

    if (activeChain != nullptr)
        activeChain->process (buffer, numSamples);

    updatePositionInfo (rate);

    if (inTail)
    {
        tailSamplesLeft -= numSamples;

        if (tailSamplesLeft <= 0)
        {
            finished.store (true, std::memory_order_relaxed);
            return false;
        }

        return true;
    }

    const bool stoppedAfterFade = stopRequested.load (std::memory_order_relaxed) && envelope.hasReachedSilence();
    const bool streamEnded = virtualPosition.load (std::memory_order_relaxed) >= (double) source->getTotalLength();

    if (stoppedAfterFade || streamEnded)
    {
        const double tailSeconds = (hardStop || activeChain == nullptr) ? 0.0 : activeChain->getTailSeconds();
        tailSamplesLeft = (juce::int64) (tailSeconds * currentSampleRate);

        if (tailSamplesLeft > 0)
        {
            inTail = true;
            return true;
        }

        finished.store (true, std::memory_order_relaxed);
        return false;
    }

    return true;
}

} // namespace gocue
