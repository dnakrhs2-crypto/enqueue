#include "audio/CuePlayer.h"

#include <cmath>

namespace gocue
{

CuePlayer::CuePlayer (const Cue& c, juce::AudioFormatManager& formats,
                      juce::TimeSliceThread* readAheadThread, int readAheadSamples,
                      double startOffsetSeconds)
    : cue (c)
{
    cue.sanitise();
    gainLinear = cue.gainLinear();
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

    startOffsetSamples = std::max<juce::int64> (0, (juce::int64) std::llround (std::max (0.0, startOffsetSeconds) * fileSampleRate));
    regionSource->setNextReadPosition (startOffsetSamples);
    source = std::move (regionSource);

    if (readAheadSamples > 0 && readAheadThread != nullptr)
    {
        readAhead = std::make_unique<juce::BufferingAudioSource> (source.get(), *readAheadThread, false, readAheadSamples, 2, true);
        readAhead->setNextReadPosition (startOffsetSamples);
        resampler = std::make_unique<juce::ResamplingAudioSource> (readAhead.get(), false, 2);
    }
    else
    {
        resampler = std::make_unique<juce::ResamplingAudioSource> (source.get(), false, 2);
    }
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
    lastRatio = fileSampleRate * liveRate.load() / currentSampleRate;
    resampler->setResamplingRatio (lastRatio);
    resampler->prepareToPlay (juce::jmax (1, blockSize), currentSampleRate);   // also prefills the read-ahead
    envelope.prepare (currentSampleRate);
    pauseGate.prepare (currentSampleRate);
}

void CuePlayer::start()
{
    if (! isValid())
        return;

    virtualPosition = (double) startOffsetSamples;
    envelope.setLevel (1.0f);
    pauseGate.setLevel (1.0f);
    updatePositionInfo (liveRate.load());
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
        source->setEndAfterPass (source->getPassIndexFor ((juce::int64) virtualPosition));
}

void CuePlayer::setLiveRegion (double startSeconds, double endSeconds) noexcept
{
    if (source == nullptr)
        return;

    const auto startSample = (juce::int64) std::llround (std::max (0.0, startSeconds) * fileSampleRate);
    const auto endSample = endSeconds >= 0.0 ? (juce::int64) std::llround (endSeconds * fileSampleRate) : source->getFileLength();

    if (endSample - startSample < 1)   // an empty region would divide by zero; ignore the edit until it is valid again
        return;

    source->setRegion (startSample, endSample);
}

void CuePlayer::setLiveRate (double rate) noexcept
{
    liveRate.store (juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, rate), std::memory_order_relaxed);
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

void CuePlayer::updatePositionInfo (double rate) noexcept
{
    const auto pos = (juce::int64) virtualPosition;
    positionSeconds.store (virtualPosition / fileSampleRate / std::max (AudioCueData::minRate, rate), std::memory_order_relaxed);
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

    const int fadeMs = pendingFadeOutMs.exchange (-1, std::memory_order_relaxed);

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

    if (paused && ! inTail)
    {
        // Frozen: feed silence through the chain so delays / reverbs keep their timing.
        buffer.clear (0, numSamples);

        if (activeChain != nullptr)
            activeChain->process (buffer, numSamples);

        return true;
    }

    const double rate = liveRate.load (std::memory_order_relaxed);
    const double ratio = fileSampleRate * rate / currentSampleRate;

    if (! inTail)
    {
        if (! juce::approximatelyEqual (ratio, lastRatio))
        {
            resampler->setResamplingRatio (ratio);
            lastRatio = ratio;
        }

        juce::AudioSourceChannelInfo info (&buffer, 0, numSamples);
        resampler->getNextAudioBlock (info);   // silence once the source is past its end
        virtualPosition += (double) numSamples * ratio;

        envelope.applyToBuffer (buffer, 0, numSamples);
        pauseGate.applyToBuffer (buffer, 0, numSamples);

        if (pausing && pauseGate.hasReachedSilence())
        {
            pausing = false;
            paused = true;
            pausedFlag.store (true, std::memory_order_relaxed);
        }

        if (gainLinear != 1.0f)
            buffer.applyGain (0, numSamples, gainLinear);
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
    const bool streamEnded = virtualPosition >= (double) source->getTotalLength();

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
