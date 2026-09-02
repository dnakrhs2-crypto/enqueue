#include "audio/CuePlayer.h"

#include <cmath>

namespace gocue
{

CuePlayer::CuePlayer (const Cue& c, juce::AudioFormatManager& formats,
                      juce::TimeSliceThread* readAheadThread, int readAheadSamples,
                      double startOffset, int busOutputs)
    : cue (c), numOutputs (juce::jlimit (1, LevelMatrix::maxOutputs, busOutputs))
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
    numChannels = juce::jlimit (1, maxChannels, (int) reader->numChannels);
    cue.numChannels = numChannels;
    cue.levels.resize (numChannels, numOutputs);
    cue.trim.resize (numOutputs);
    cue.sanitise();

    currentGains.assign ((size_t) (numChannels * numOutputs), 0.0f);
    computeGains (cue.levels, cue.trim, currentGains);
    targetGains = currentGains;
    publishedGains = currentGains;
    liveGainDb = cue.gainDb;
    liveLevels = cue.levels;
    liveTrim = cue.trim;

    auto regionSource = std::make_unique<RegionLoopSource> (std::move (reader));

    const auto startSample = (juce::int64) std::llround (cue.regionStart() * fileSampleRate);
    const auto endSample = cue.audio.endSeconds >= 0.0 ? (juce::int64) std::llround (cue.regionEnd() * fileSampleRate)
                                                       : regionSource->getFileLength();
    regionSource->setRegion (startSample, endSample);
    regionSource->setPlayCount (cue.audio.playCount, cue.audio.infiniteLoop);
    regionSource->setEnvelope (cue.audio.envelope);

    {
        std::vector<RegionLoopSource::SliceMarker> markers;

        for (const auto& s : cue.audio.slices)
            markers.push_back ({ (juce::int64) std::llround (s.seconds * fileSampleRate), s.playCount });

        regionSource->setSlices (markers, cue.audio.firstSliceCount);
    }

    if (regionSource->getRegionLength() <= 0)
    {
        errorMessage = "The trimmed region of cue \"" + cue.name + "\" is empty";
        return;
    }

    if (regionSource->getTotalLength() <= 0)
    {
        errorMessage = "Every slice of cue \"" + cue.name + "\" is skipped (play count 0)";
        return;
    }

    startOffsetSeconds = std::max (0.0, startOffset);
    startOffsetSamples = std::max<juce::int64> (0, (juce::int64) std::llround (startOffsetSeconds * fileSampleRate));
    regionSource->setNextReadPosition (startOffsetSamples);
    source = std::move (regionSource);

    juce::PositionableAudioSource* tail = source.get();

    if (readAheadSamples > 0 && readAheadThread != nullptr)
    {
        // faster playback pulls more file samples per block: size the read-ahead for twice the cue's rate
        const int scale = juce::jlimit (1, 8, (int) std::ceil (cue.audio.rate * 2.0));
        readAhead = std::make_unique<juce::BufferingAudioSource> (source.get(), *readAheadThread, false, readAheadSamples * scale, numChannels, true);
        readAhead->setNextReadPosition (startOffsetSamples);
        tail = readAhead.get();
    }

    if (cue.audio.preservePitch)
    {
        stretch = std::make_unique<StretchSource> (*tail, numChannels, fileSampleRate);
        stretch->setRate (cue.audio.rate);
        stretch->setNextReadPosition (startOffsetSamples);
        tail = stretch.get();
    }

    resampler = std::make_unique<juce::ResamplingAudioSource> (tail, false, numChannels);

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

void CuePlayer::requestFinishCurrentPass (bool stopAfter) noexcept
{
    if (source != nullptr)
        source->finishCurrentPass ((juce::int64) virtualPosition.load (std::memory_order_relaxed), stopAfter);
}

juce::int64 CuePlayer::getCurrentPassEnd() const noexcept
{
    if (source == nullptr)
        return -1;

    const auto pos = (juce::int64) virtualPosition.load (std::memory_order_relaxed);
    const auto loc = source->locate (pos);

    if (loc.beyondEnd)
        return -1;

    return pos - loc.offset + source->getRunLength (loc.run);
}

void CuePlayer::setLiveRegion (double startSeconds, double endSeconds) noexcept
{
    if (source == nullptr)
        return;

    const auto startSample = (juce::int64) std::llround (std::max (0.0, startSeconds) * fileSampleRate);
    const auto endSample = endSeconds >= 0.0 ? (juce::int64) std::llround (endSeconds * fileSampleRate) : source->getFileLength();

    if (endSample - startSample < 1)   // an empty region would divide by zero; ignore the edit until it is valid again
        return;

    // keep the audible file position: find it again in the new layout (same pass if it still exists)
    const auto pos = (juce::int64) virtualPosition.load (std::memory_order_relaxed);
    const auto where = source->locate (pos);
    const juce::int64 filePos = juce::jlimit (startSample, endSample - 1, where.fileSample);

    source->setRegion (startSample, endSample);

    const juce::int64 newPos = source->virtualPositionFor (filePos, where.pass);
    jumpTo (newPos);
}

void CuePlayer::jumpTo (juce::int64 newPos) noexcept
{
    // drop the read-ahead cache: it holds audio of the old layout for the same virtual positions
    if (readAhead != nullptr)
    {
        readAhead->setNextReadPosition (RegionLoopSource::infiniteLength);   // outside any cached range: invalidates it
        readAhead->setNextReadPosition (newPos);
    }
    else
    {
        source->setNextReadPosition (newPos);
    }

    if (stretch != nullptr)
        stretch->setNextReadPosition (newPos);   // re-seeks with a pre-roll on the audio thread

    pendingVirtualPosition.store (newPos, std::memory_order_release);
}

void CuePlayer::setLiveSlices (const std::vector<Slice>& slices, int firstSliceCount) noexcept
{
    if (source == nullptr)
        return;

    const auto pos = (juce::int64) virtualPosition.load (std::memory_order_relaxed);
    const auto where = source->locate (pos);

    std::vector<RegionLoopSource::SliceMarker> markers;

    for (const auto& s : slices)
        markers.push_back ({ (juce::int64) std::llround (s.seconds * fileSampleRate), s.playCount });

    source->setSlices (markers, firstSliceCount);

    if (source->getTotalLength() <= 0)
    {
        requestStop();   // every slice skipped: nothing left to play
        return;
    }

    const juce::int64 newPos = source->virtualPositionFor (where.fileSample, where.pass);
    jumpTo (newPos);
}

void CuePlayer::seekToFraction (double fraction) noexcept
{
    if (source == nullptr || source->isInfinite())
        return;

    const auto total = source->getTotalLength();

    if (total <= 0)
        return;

    const auto newPos = juce::jlimit<juce::int64> (0, total - 1, (juce::int64) std::llround (juce::jlimit (0.0, 1.0, fraction) * (double) total));

    // elapsed time follows the new position at the current rate (the start offset counts as elapsed)
    const double rate = std::max (AudioCueData::minRate, effectiveRate (liveRate.load (std::memory_order_relaxed)));
    const double elapsedSamples = ((double) newPos / fileSampleRate / rate - startOffsetSeconds) * currentSampleRate;
    pendingElapsedSamples.store (std::max (0.0, elapsedSamples), std::memory_order_relaxed);
    jumpTo (newPos);
}

void CuePlayer::setLiveRate (double rate) noexcept
{
    liveRate.store (juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, rate), std::memory_order_relaxed);
}

void CuePlayer::setLiveGainDb (double gainDb) noexcept
{
    Cue tmp;
    tmp.gainDb = gainDb;
    tmp.sanitise();
    liveGainDb = tmp.gainDb;
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

    const double rate = std::max (AudioCueData::minRate, effectiveRate (liveRate.load (std::memory_order_relaxed)));
    return (double) source->getTotalLength() / fileSampleRate / rate;
}

double CuePlayer::getRemainingSeconds() const noexcept
{
    if (source == nullptr)
        return 0.0;

    if (source->isInfinite())
        return -1.0;

    const double rate = std::max (AudioCueData::minRate, effectiveRate (liveRate.load (std::memory_order_relaxed)));
    const double left = (double) source->getTotalLength() - virtualPosition.load (std::memory_order_relaxed);
    return std::max (0.0, left) / fileSampleRate / rate;
}

void CuePlayer::processChain (PluginChain& activeChain, juce::AudioBuffer<float>& fullBuffer, int numSamples) noexcept
{
    // The chain is stereo: a mono cue is duplicated into channel 1 for the plugins and channel 0 is kept
    // afterwards; cues with more channels run channels 0-1 through the chain, the rest pass untouched.
    if (fullBuffer.getNumChannels() < 2)
        return;

    if (numChannels == 1)
        fullBuffer.copyFrom (1, 0, fullBuffer, 0, 0, numSamples);

    juce::AudioBuffer<float> stereo (fullBuffer.getArrayOfWritePointers(), 2, 0, numSamples);
    activeChain.process (stereo, numSamples);

    if (numChannels == 1)
    {
        // only channel 0 feeds the matrix: fold the plugins' right channel back in so stereo effects
        // (ping-pong delays, wideners) are not half lost
        fullBuffer.addFrom (0, 0, fullBuffer, 1, 0, numSamples);
        fullBuffer.applyGain (0, 0, numSamples, 0.5f);
    }
}

void CuePlayer::computeGains (const LevelMatrix& levels, const TrimLevels& trim, std::vector<float>& out) const
{
    out.resize ((size_t) (numChannels * numOutputs));

    for (int in = 0; in < numChannels; ++in)
        for (int o = 0; o < numOutputs; ++o)
            out[(size_t) (in * numOutputs + o)] = levels.gainFor (in, o) * trim.gainForOutput (o);
}

void CuePlayer::setLiveLevels (const LevelMatrix& levels, const TrimLevels& trim)
{
    if (! isValid())
        return;

    LevelMatrix sized = levels;
    sized.resize (numChannels, numOutputs);
    TrimLevels sizedTrim = trim;
    sizedTrim.resize (numOutputs);
    liveLevels = sized;
    liveTrim = sizedTrim;

    std::vector<float> gains;
    computeGains (sized, sizedTrim, gains);

    gainsVersion.fetch_add (1, std::memory_order_acq_rel);   // odd: writing
    std::copy (gains.begin(), gains.end(), publishedGains.begin());
    gainsVersion.fetch_add (1, std::memory_order_acq_rel);   // even: stable
}

void CuePlayer::adoptPublishedGains() noexcept
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const auto v1 = gainsVersion.load (std::memory_order_acquire);

        if (v1 == adoptedGainsVersion || (v1 & 1u) != 0)
            return;   // nothing new, or the writer is mid-update: try again next block

        std::copy (publishedGains.begin(), publishedGains.end(), targetGains.begin());

        if (gainsVersion.load (std::memory_order_acquire) == v1)
        {
            adoptedGainsVersion = v1;
            return;
        }
    }
}

void CuePlayer::mixIntoBus (juce::AudioBuffer<float>& bus, const juce::AudioBuffer<float>& rendered, int numSamples) noexcept
{
    if (! isValid() || numSamples <= 0)
        return;

    adoptPublishedGains();

    const int ins = juce::jmin (numChannels, rendered.getNumChannels());
    const int outs = juce::jmin (numOutputs, bus.getNumChannels());
    const float alpha = (float) juce::jmin (1.0, (double) numSamples / (0.010 * currentSampleRate));   // ~10 ms level ramps

    for (int in = 0; in < ins; ++in)
    {
        const float* src = rendered.getReadPointer (in);

        for (int o = 0; o < outs; ++o)
        {
            const size_t idx = (size_t) (in * numOutputs + o);
            const float g0 = currentGains[idx];
            const float goal = targetGains[idx];
            float g1 = g0 + (goal - g0) * alpha;

            if (std::abs (g1 - goal) < 1.0e-5f)
                g1 = goal;

            if (g0 != 0.0f || g1 != 0.0f)
                bus.addFromWithRamp (o, 0, src, numSamples, g0, g1);

            currentGains[idx] = g1;
        }
    }
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

bool CuePlayer::renderNextBlock (juce::AudioBuffer<float>& fullBuffer, int numSamples)
{
    // the player's channels only (no allocation: the channel-pointer array lives inside the view)
    juce::AudioBuffer<float> buffer (fullBuffer.getArrayOfWritePointers(), juce::jmin (numChannels, fullBuffer.getNumChannels()), 0, numSamples);

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
            processChain (*activeChain, fullBuffer, numSamples);

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

        if (stretch != nullptr)
            stretch->setRate (rate);

        juce::AudioSourceChannelInfo info (&buffer, 0, numSamples);
        resampler->getNextAudioBlock (info);   // silence once the source is past its end
        virtualPosition.store (virtualPosition.load (std::memory_order_relaxed) + (double) numSamples * advanceFor (rate), std::memory_order_relaxed);
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
        processChain (*activeChain, fullBuffer, numSamples);

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
