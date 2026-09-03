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

    if (cue.isMic())
    {
        // live input: no file, no timeline; the rows are device input channels
        micMode = true;
        numChannels = juce::jlimit (1, maxChannels, cue.mic.numInputs);
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
        return;
    }

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
    startOffsetSamples = 0;   // a plain GO starts the layout at its beginning, whatever the first slice is

    if (startOffsetSeconds > 0.0)
    {
        // 'startOffset' is file seconds after the region start: place it in the first pass of the layout
        // (slices / a skipped first slice make virtual and file positions differ). When that file place is not
        // played at all (a skipped slice), fall back to the plain offset, clamped inside the layout.
        const auto fileSample = (juce::int64) std::llround ((cue.regionStart() + startOffsetSeconds) * fileSampleRate);
        const auto mapped = regionSource->virtualPositionFor (fileSample, RegionLoopSource::Location{});
        const auto check = regionSource->locationFor (mapped);
        const auto total = regionSource->getTotalLength();
        const auto plain = juce::jlimit<juce::int64> (0, juce::jmax<juce::int64> (0, total - 1), (juce::int64) std::llround (startOffsetSeconds * fileSampleRate));
        startOffsetSamples = (! check.beyondEnd && std::abs (check.fileSample - fileSample) <= 1) ? mapped : plain;
    }

    regionSource->setNextReadPosition (startOffsetSamples);
    source = std::move (regionSource);

    juce::PositionableAudioSource* tail = source.get();

    if (readAheadSamples > 0 && readAheadThread != nullptr)
    {
        // faster playback pulls more file samples per block: size the read-ahead for twice the cue's rate
        const int scale = juce::jlimit (1, 8, (int) std::ceil (cue.audio.rate * 2.0));
        readAhead = std::make_unique<ReadAheadSource> (*source, *readAheadThread, readAheadSamples * scale, numChannels);
        // the stretcher pre-rolls before the start: have that in the cache too
        readAhead->setNextReadPosition (cue.audio.preservePitch ? juce::jmax<juce::int64> (0, startOffsetSamples - (stretch != nullptr ? stretch->getPreRollSamples() : 8192)) : startOffsetSamples);
        tail = readAhead.get();
    }

    if (cue.audio.preservePitch)
    {
        stretch = std::make_unique<StretchSource> (*tail, numChannels, fileSampleRate);
        stretch->setRate (cue.audio.rate);
        stretch->setNextReadPosition (startOffsetSamples);
        tail = stretch.get();
    }

    srcConverter = std::make_unique<HighQualityResampler> (*tail, numChannels, fileSampleRate);
    resampler = std::make_unique<juce::ResamplingAudioSource> (srcConverter.get(), false, numChannels);

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

    if (micMode)
    {
        envelope.prepare (currentSampleRate);
        pauseGate.prepare (currentSampleRate);
        juce::ignoreUnused (blockSize);
        return;
    }

    // prepare for the fastest rate first so the resampler never has to grow its buffer on the audio thread
    srcConverter->setDeviceRate (currentSampleRate);   // before the chain prepares (the speed stage passes a scaled rate down)
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
    pendingFadeOutMs.store (juce::jmax (stopDeclickMs, milliseconds), std::memory_order_release);   // after skipTailOnStop: whoever sees the time sees the flag
}

void CuePlayer::requestPanicFadeOut (int milliseconds) noexcept
{
    // an instance already in its insert tail fades that tail over the same time (published before the flag, which is
    // read with acquire: whoever sees the flag sees the time)
    pendingTailFadeMs.store (juce::jmax (0, milliseconds), std::memory_order_relaxed);
    skipTailOnStop.store (true, std::memory_order_release);
    requestFadeOut (milliseconds);
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

juce::int64 CuePlayer::requestFinishCurrentPass (bool stopAfter) noexcept
{
    if (source == nullptr)
        return -1;

    const juce::int64 boundary = source->finishCurrentPass (controlPosition(), stopAfter);

    if (boundary >= 0 && readAhead != nullptr)
        readAhead->invalidate (controlPosition());   // the cache may hold the next pass of the loop that no longer comes

    return boundary;
}

juce::int64 CuePlayer::getCurrentPassEnd() const noexcept
{
    return source != nullptr ? source->passEndFor (controlPosition()) : -1;
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
    const auto pos = controlPosition();
    const auto where = source->locate (pos);
    const juce::int64 filePos = juce::jlimit (startSample, endSample - 1, where.fileSample);

    source->setRegion (startSample, endSample);

    const juce::int64 newPos = source->virtualPositionFor (filePos, where);
    jumpTo (newPos);
}

void CuePlayer::jumpTo (juce::int64 newPos) noexcept
{
    // drop the read-ahead cache: it holds audio of the old layout for the same virtual positions
    // (the stretcher pre-rolls before the new position: start the refill early enough for that)
    if (readAhead != nullptr)
        readAhead->invalidate (stretch != nullptr ? juce::jmax<juce::int64> (0, newPos - stretch->getPreRollSamples()) : newPos);
    else
        source->setNextReadPosition (newPos);

    if (stretch != nullptr)
        stretch->setNextReadPosition (newPos);   // re-seeks with a pre-roll on the audio thread

    pendingVirtualPosition.store (newPos, std::memory_order_release);
}

void CuePlayer::setLivePlayCount (int playCount, bool infiniteLoop) noexcept
{
    if (source == nullptr)
        return;

    const auto pos = controlPosition();
    const auto where = source->locate (pos);

    source->setPlayCount (playCount, infiniteLoop);

    // the same file sample in the same pass (clamped to the last pass when the sequence is finite now); the
    // read-ahead is dropped either way: it may hold the old layout's end (silence) or the old loop-back
    const juce::int64 newPos = source->virtualPositionFor (where.fileSample, where);
    jumpTo (newPos);   // the next block reports the new position / length
}

void CuePlayer::setLiveSlices (const std::vector<Slice>& slices, int firstSliceCount) noexcept
{
    if (source == nullptr)
        return;

    const auto pos = controlPosition();
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

    const juce::int64 newPos = source->virtualPositionFor (where.fileSample, where);
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

bool CuePlayer::seekToFileSeconds (double fileSeconds) noexcept
{
    if (source == nullptr || micMode)
        return false;

    const auto regionEndSample = (juce::int64) std::llround (cue.regionEnd() * fileSampleRate);
    const auto fileSample = juce::jlimit<juce::int64> (0, juce::jmax<juce::int64> (0, regionEndSample - 1),
                                                       (juce::int64) std::llround (juce::jmax (0.0, fileSeconds) * fileSampleRate));
    const auto where = source->locationFor (controlPosition());          // the pass we are in now
    const auto newPos = source->virtualPositionFor (fileSample, where);   // that file place inside this pass
    const auto check = source->locationFor (newPos);

    if (newPos < 0 || check.beyondEnd || std::abs (check.fileSample - fileSample) > 1)
        return false;   // a skipped slice / outside the region: nothing to jump to

    const double rate = std::max (AudioCueData::minRate, effectiveRate (liveRate.load (std::memory_order_relaxed)));
    const double elapsedSamples = ((double) newPos / fileSampleRate / rate - startOffsetSeconds) * currentSampleRate;
    pendingElapsedSamples.store (std::max (0.0, elapsedSamples), std::memory_order_relaxed);
    jumpTo (newPos);
    return true;
}

void CuePlayer::setLiveEnvelope (const Envelope& cueEnvelope) noexcept
{
    if (source == nullptr)
        return;

    source->setLiveEnvelope (cueEnvelope);

    // the read-ahead holds audio shaped by the old envelope: refill it from where playback is (a few ms)
    if (readAhead != nullptr)
        readAhead->invalidateCurrent();
}

void CuePlayer::setLiveRate (double rate) noexcept
{
    liveRate.store (juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, rate), std::memory_order_relaxed);
}

void CuePlayer::setInitialGainDb (double gainDb) noexcept
{
    // the instance is not rendering yet (or renders silence as a loaded instance): the smoothed gain starts here too
    liveGainDb = juce::jlimit (Cue::minGainDb, Cue::maxGainDb, gainDb);
    const float linear = juce::Decibels::decibelsToGain ((float) liveGainDb, (float) Cue::minGainDb);
    targetGain.store (linear, std::memory_order_relaxed);
    gainLinear = linear;
}

void CuePlayer::setLiveGainDb (double gainDb) noexcept
{
    // no Cue temporary here: this runs under the engine lock at fade rate, and Cue::sanitise() allocates
    liveGainDb = juce::jlimit (Cue::minGainDb, Cue::maxGainDb, gainDb);   // the same clamp as Cue::gainLinear
    targetGain.store (juce::Decibels::decibelsToGain ((float) liveGainDb, (float) Cue::minGainDb), std::memory_order_relaxed);
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
    if (micMode)
        return -1.0;   // until stopped

    if (source == nullptr)
        return 0.0;

    if (source->isInfinite())
        return -1.0;

    const double rate = std::max (AudioCueData::minRate, effectiveRate (liveRate.load (std::memory_order_relaxed)));
    return (double) source->getTotalLength() / fileSampleRate / rate;
}

double CuePlayer::getRemainingSeconds() const noexcept
{
    if (micMode)
        return -1.0;

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

    {
        const juce::SpinLock::ScopedLockType sl (gainsLock);   // a few microseconds; the audio thread only try-locks
        std::copy (gains.begin(), gains.end(), publishedGains.begin());
        gainsVersion.fetch_add (2, std::memory_order_acq_rel);
    }
}

void CuePlayer::adoptPublishedGains() noexcept
{
    const auto v = gainsVersion.load (std::memory_order_acquire);

    if (v == adoptedGainsVersion)
        return;   // nothing new

    const juce::SpinLock::ScopedTryLockType sl (gainsLock);

    if (! sl.isLocked())
        return;   // the writer is mid-update: the old gains stay for one more block

    std::copy (publishedGains.begin(), publishedGains.end(), targetGains.begin());
    adoptedGainsVersion = gainsVersion.load (std::memory_order_acquire);
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
    if (! isValid() || micMode)
        return micMode ? -1.0 : 0.0;

    const auto total = source->getTotalLength();

    if (total <= 0 || total >= RegionLoopSource::infiniteLength / 2)
        return -1.0;

    return juce::jlimit (0.0, 1.0, virtualPosition.load (std::memory_order_relaxed) / (double) total);
}

void CuePlayer::updatePositionInfo (double rate) noexcept
{
    juce::ignoreUnused (rate);

    if (source == nullptr)
    {
        positionSeconds.store (elapsedOutputSamples / currentSampleRate, std::memory_order_relaxed);   // a mic cue: elapsed time only
        return;
    }

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

    if (! isValid())
    {
        buffer.clear (0, numSamples);
        return false;
    }

    if (const auto seek = pendingVirtualPosition.exchange (-1, std::memory_order_acq_rel); seek >= 0)
    {
        const bool stopping = stopRequested.load (std::memory_order_relaxed) || hardStopRequested.load (std::memory_order_relaxed)
                              || skipTailOnStop.load (std::memory_order_relaxed);

        if (endedNaturally.exchange (false, std::memory_order_acq_rel) && ! stopping)   // never past a stop or a panic
        {
            // a live edit landed after the stream had ended (longer region, more passes, endless loop): it goes on
            finished.store (false, std::memory_order_relaxed);
            inTail = false;
            tailSamplesLeft = 0;
        }

        virtualPosition.store ((double) seek, std::memory_order_relaxed);

        if (const double elapsed = pendingElapsedSamples.exchange (-1.0, std::memory_order_relaxed); elapsed >= 0.0)
            elapsedOutputSamples = elapsed;

        resampler->flushBuffers();   // drop the interpolator's history so the old position is not heard after the jump
        srcConverter->reset();
    }

    if (finished.load (std::memory_order_relaxed))
    {
        buffer.clear (0, numSamples);
        return false;
    }

    const int fadeMs = pendingFadeOutMs.exchange (-1, std::memory_order_acq_rel);

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

        if (! inTail && skipTailOnStop.load (std::memory_order_acquire))
            panicFadeSamplesLeft = (juce::int64) (fadeMs * currentSampleRate / 1000.0);   // a soft panic: remembered in case the source ends first
    }

    const bool hardStop = hardStopRequested.load (std::memory_order_relaxed);

    if (inTail && hardStop)   // a hard stop ends a ringing tail at once
    {
        buffer.clear (0, numSamples);
        finished.store (true, std::memory_order_relaxed);
        return false;
    }

    if (inTail && tailFadeSamplesLeft < 0 && skipTailOnStop.load (std::memory_order_acquire))
    {
        // a soft panic while the insert tail rings: the tail fades over the panic time (after the chain, below)
        // instead of cutting to silence while the output gate is still open
        const int ms = juce::jmax (0, pendingTailFadeMs.exchange (-1, std::memory_order_relaxed));
        tailFadeSamplesLeft = (juce::int64) (ms * currentSampleRate / 1000.0);
        tailFadeGain = 1.0f;
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
        const bool ended = ! micMode && source != nullptr && virtualPosition.load (std::memory_order_relaxed) >= (double) source->getTotalLength();
        const bool stopping = stopRequested.load (std::memory_order_relaxed);

        if (stopping && ! hardStop && ! ended && activeChain != nullptr && skipTailOnStop.load (std::memory_order_acquire))
        {
            // a soft panic: the insert may still ring (or generate) - it fades after the chain over the panic time,
            // like a ringing tail (the tail branch below takes over from here)
            inTail = true;
            tailSamplesLeft = tailFadeSamplesLeft = juce::jmax<juce::int64> ((juce::int64) (0.2 * currentSampleRate), panicFadeSamplesLeft);
            tailFadeGain = 1.0f;
        }
        else if (stopping || hardStop || ended)
        {
            buffer.clear (0, numSamples);
            finished.store (true, std::memory_order_relaxed);
            return false;
        }
        else
        {
            buffer.clear (0, numSamples);   // feed silence through the chain so delays / reverbs keep their timing

            if (activeChain != nullptr)
                processChain (*activeChain, fullBuffer, numSamples);

            return true;
        }
    }

    const double ratio = micMode ? 1.0 : ratioFor (rate);

    if (! inTail)
    {
        if (micMode)
        {
            // the rows are device input channels (silence for channels the device does not provide)
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const int input = cue.mic.firstInput + ch;

                if (inputData != nullptr && input < inputChannels && inputData[input] != nullptr)
                    buffer.copyFrom (ch, 0, inputData[input] + inputOffset, numSamples);
                else
                    buffer.clear (ch, 0, numSamples);
            }
        }
        else
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
        }

        elapsedOutputSamples += numSamples;

        if (panicFadeSamplesLeft >= 0)
            panicFadeSamplesLeft -= numSamples;

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
        if (tailFadeSamplesLeft >= 0)
        {
            // the panic fade of a ringing tail: a linear ramp that lands on zero exactly when the panic time is over
            const int step = (int) juce::jmin<juce::int64> ((juce::int64) numSamples, tailFadeSamplesLeft);
            const float next = step < tailFadeSamplesLeft ? tailFadeGain * (float) (1.0 - (double) step / (double) tailFadeSamplesLeft) : 0.0f;

            // through fullBuffer, per channel: the 'buffer' view was cleared above, and JUCE skips gain changes on a
            // buffer object it believes is still clear (the chain wrote through other views)
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                fullBuffer.applyGainRamp (ch, 0, step, tailFadeGain, next);

                if (step < numSamples)
                    fullBuffer.clear (ch, step, numSamples - step);
            }

            tailFadeGain = next;
            tailFadeSamplesLeft -= step;

            if (tailFadeSamplesLeft <= 0)
            {
                finished.store (true, std::memory_order_relaxed);
                return false;
            }

            return true;   // the panic fade decides when this instance ends, not the declared tail
        }

        tailSamplesLeft -= numSamples;

        if (tailSamplesLeft <= 0)
        {
            finished.store (true, std::memory_order_relaxed);
            return false;
        }

        return true;
    }

    const bool stoppedAfterFade = stopRequested.load (std::memory_order_relaxed) && envelope.hasReachedSilence();
    const bool streamEnded = ! micMode && virtualPosition.load (std::memory_order_relaxed) >= (double) source->getTotalLength();

    if (stoppedAfterFade || streamEnded)
    {
        if (streamEnded && ! stoppedAfterFade)
            endedNaturally.store (true, std::memory_order_relaxed);

        if (! hardStop && activeChain != nullptr && skipTailOnStop.load (std::memory_order_acquire))
        {
            // a soft panic: the insert rings on and fades after itself - over what is left of the panic time when the
            // source ended early, else over the output gate's own close ramp (the envelope took the whole panic time):
            // the chain's output is never cut while the gate is still open
            inTail = true;
            tailSamplesLeft = tailFadeSamplesLeft = panicFadeSamplesLeft > 0 ? panicFadeSamplesLeft : (juce::int64) (0.2 * currentSampleRate);
            tailFadeGain = 1.0f;
            return true;
        }

        const double tailSeconds = (hardStop || skipTailOnStop.load (std::memory_order_relaxed) || activeChain == nullptr) ? 0.0 : activeChain->getTailSeconds();
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
