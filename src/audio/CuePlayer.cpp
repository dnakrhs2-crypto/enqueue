#include "audio/CuePlayer.h"

namespace gocue
{

CuePlayer::CuePlayer (const Cue& c, juce::AudioFormatManager& formats,
                      juce::TimeSliceThread* readAheadThread, int readAheadSamples)
    : cue (c)
{
    cue.sanitise();
    gainLinear = cue.gainLinear();

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

    const double sourceRate = reader->sampleRate;
    lengthSeconds = sourceRate > 0.0 ? (double) reader->lengthInSamples / sourceRate : 0.0;

    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

    const bool useReadAhead = readAheadSamples > 0 && readAheadThread != nullptr;

    transport.setSource (readerSource.get(),
                         useReadAhead ? readAheadSamples : 0,
                         useReadAhead ? readAheadThread : nullptr,
                         sourceRate,
                         2);
}

CuePlayer::~CuePlayer()
{
    transport.setSource (nullptr);
}

void CuePlayer::prepare (double sampleRate, int blockSize)
{
    if (! isValid())
        return;

    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    transport.prepareToPlay (juce::jmax (1, blockSize), currentSampleRate);
    envelope.prepare (currentSampleRate);
}

void CuePlayer::start()
{
    if (! isValid())
        return;

    transport.setPosition (0.0);
    envelope.setLevel (cue.fadeInMs > 0 ? 0.0f : 1.0f);
    envelope.fadeIn (cue.fadeInMs);
    transport.start();
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

    juce::AudioSourceChannelInfo info (&buffer, 0, numSamples);
    transport.getNextAudioBlock (info);          // silence once the stream has ended

    envelope.applyToBuffer (buffer, 0, numSamples);

    if (gainLinear != 1.0f)
        buffer.applyGain (0, numSamples, gainLinear);

    auto* activeChain = chain.load (std::memory_order_acquire);

    if (activeChain != nullptr)
        activeChain->process (buffer, numSamples);

    positionSeconds.store (transport.getCurrentPosition(), std::memory_order_relaxed);

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
    const bool streamEnded = transport.hasStreamFinished() || ! transport.isPlaying();

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
