#include "audio/AudioEngine.h"

namespace gocue
{

AudioEngine::AudioEngine (int readAhead)
    : readAheadSamples (juce::jmax (0, readAhead))
{
    formatManager.registerBasicFormats();

    mixBuffer.setSize (2, blockSize.load());
    playerBuffer.setSize (2, blockSize.load());
    players.reserve (256);   // push_back under the audio lock must not reallocate

    if (readAheadSamples > 0)
        readAheadThread.startThread();
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

juce::String AudioEngine::initialise (const juce::XmlElement* savedDeviceState)
{
    const auto error = deviceManager.initialise (0, 2, savedDeviceState, true);

    if (! callbackAdded)
    {
        deviceManager.addAudioCallback (this);
        callbackAdded = true;
    }

    return error;
}

void AudioEngine::shutdown()
{
    if (callbackAdded)
    {
        deviceManager.removeAudioCallback (this);
        callbackAdded = false;
    }

    deviceManager.closeAudioDevice();
    cancelPendingUpdate();

    std::vector<std::unique_ptr<CuePlayer>> old;

    {
        const juce::ScopedLock sl (lock);
        old.swap (players);
    }

    old.clear();
    readAheadThread.stopThread (3000);

    cueChains.clear();
    masterChain.clear();
}

//==============================================================================
bool AudioEngine::play (const Cue& cue, const PlayOptions& options, juce::String* errorMessage)
{
    {
        // a loaded instance is waiting: start it instead of opening the file again
        const juce::ScopedLock sl (lock);

        for (auto& existing : players)
        {
            if (existing->getCueId() == cue.id && existing->isLoadedNotStarted())
            {
                for (auto& other : players)
                {
                    if (other.get() != existing.get() && other->getCueId() == cue.id)
                    {
                        other->setChain (nullptr);
                        other->requestStop();
                    }
                }

                existing->setStartOrder (++startCounter);
                existing->start();
                return true;
            }
        }
    }

    auto player = std::make_unique<CuePlayer> (cue, formatManager,
                                               readAheadSamples > 0 ? &readAheadThread : nullptr,
                                               readAheadSamples, options.startSeconds);

    if (! player->isValid())
    {
        if (errorMessage != nullptr)
            *errorMessage = player->getErrorMessage();

        return false;
    }

    player->prepare (getSampleRate(), getBlockSize());
    player->setStartOrder (++startCounter);
    player->setChain (findCueChain (cue.id));
    player->start();

    const juce::ScopedLock sl (lock);

    for (auto& existing : players)
    {
        if (existing->getCueId() == cue.id)
        {
            existing->setChain (nullptr);     // the chain now belongs to the new instance
            existing->requestStop();
        }
    }

    players.push_back (std::move (player));
    return true;
}

bool AudioEngine::load (const Cue& cue, double startSeconds, juce::String* errorMessage)
{
    auto player = std::make_unique<CuePlayer> (cue, formatManager,
                                               readAheadSamples > 0 ? &readAheadThread : nullptr,
                                               readAheadSamples, startSeconds);

    if (! player->isValid())
    {
        if (errorMessage != nullptr)
            *errorMessage = player->getErrorMessage();

        return false;
    }

    player->prepare (getSampleRate(), getBlockSize());
    player->setChain (findCueChain (cue.id));
    player->armLoaded();

    const juce::ScopedLock sl (lock);

    for (auto& existing : players)
        if (existing->getCueId() == cue.id && existing->isLoadedNotStarted())
            existing->requestStop();   // replaced by the new loaded instance

    players.push_back (std::move (player));
    return true;
}

bool AudioEngine::isLoaded (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && p->isLoadedNotStarted())
            return true;

    return false;
}

void AudioEngine::unload (const juce::Uuid& cueId)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->isLoadedNotStarted() && (cueId.isNull() || p->getCueId() == cueId))
            p->requestStop();
}

void AudioEngine::stop (const juce::Uuid& cueId)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->requestStop();
}

void AudioEngine::stopAll()
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (! p->hasFinished())
            p->requestStop();
}

void AudioEngine::fadeOutAndStop (const juce::Uuid& cueId)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->requestFadeOut (p->getCue().fadeOutMs);
}

void AudioEngine::fadeOutAndStopAll()
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (! p->hasFinished())
            p->requestFadeOut (p->getCue().fadeOutMs);
}

void AudioEngine::fadeOutAndStop (const juce::Uuid& cueId, int milliseconds)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->requestFadeOut (milliseconds);
}

void AudioEngine::fadeOutAndStopAll (int milliseconds)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (! p->hasFinished())
            p->requestFadeOut (milliseconds);
}

void AudioEngine::pause (const juce::Uuid& cueId)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->requestPause();
}

void AudioEngine::resume (const juce::Uuid& cueId)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->requestResume();
}

void AudioEngine::pauseAll()
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (! p->hasFinished())
            p->requestPause();
}

void AudioEngine::resumeAll()
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (! p->hasFinished())
            p->requestResume();
}

bool AudioEngine::isPaused (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && p->isPaused())
            return true;

    return false;
}

void AudioEngine::finishCurrentPass (const juce::Uuid& cueId)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->requestFinishCurrentPass();
}

void AudioEngine::setLiveRegion (const juce::Uuid& cueId, double startSeconds, double endSeconds)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->setLiveRegion (startSeconds, endSeconds);
}

void AudioEngine::setLiveRate (const juce::Uuid& cueId, double rate)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->setLiveRate (rate);
}

void AudioEngine::setLiveGainDb (const juce::Uuid& cueId, double gainDb)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->setLiveGainDb (gainDb);
}

void AudioEngine::setDuckDb (const juce::Uuid& cueId, double duckDb, double rampSeconds)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            p->setDuckDb (duckDb, rampSeconds);
}

double AudioEngine::getDuckDb (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            return p->getDuckDb();

    return 0.0;
}

std::vector<juce::Uuid> AudioEngine::getPausedCues() const
{
    std::vector<juce::Uuid> result;
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (! p->hasFinished() && p->isPaused())
            result.push_back (p->getCueId());

    return result;
}

bool AudioEngine::isPlaying (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted())
            return true;

    return false;
}

std::vector<AudioEngine::PlayingCue> AudioEngine::getPlayingCues() const
{
    std::vector<PlayingCue> result;
    result.reserve (64);   // no allocation while the audio lock is held
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
    {
        if (p->hasFinished())
            continue;

        PlayingCue info;
        info.id = p->getCueId();
        info.positionSeconds = p->getPositionSeconds();
        info.lengthSeconds = p->getLengthSeconds();
        info.remainingSeconds = p->getRemainingSeconds();
        info.filePositionSeconds = p->getFilePositionSeconds();
        info.passIndex = p->getPassIndex();
        info.fadingOut = p->isFadingOut();
        info.paused = p->isPaused();
        info.loaded = p->isLoadedNotStarted();
        result.push_back (info);
    }

    return result;
}

juce::Uuid AudioEngine::getMostRecentlyStartedCue (bool ignoreFadingOut) const
{
    const juce::ScopedLock sl (lock);
    const CuePlayer* best = nullptr;

    for (auto& p : players)
    {
        if (p->hasFinished() || p->isLoadedNotStarted() || (ignoreFadingOut && p->isFadingOut()))
            continue;

        if (best == nullptr || p->getStartOrder() > best->getStartOrder())
            best = p.get();
    }

    return best != nullptr ? best->getCueId() : juce::Uuid::null();
}

int AudioEngine::getNumPlaying() const
{
    const juce::ScopedLock sl (lock);
    int count = 0;

    for (auto& p : players)
        if (! p->hasFinished() && ! p->isLoadedNotStarted())
            ++count;

    return count;
}

//==============================================================================
PluginChain& AudioEngine::getCueChain (const juce::Uuid& cueId)
{
    auto& slot = cueChains[cueId.toString()];

    if (slot == nullptr)
    {
        slot = std::make_unique<PluginChain>();
        slot->setListener (chainListener);
        slot->prepare (getSampleRate(), getBlockSize());
    }

    return *slot;
}

PluginChain* AudioEngine::findCueChain (const juce::Uuid& cueId) const
{
    const auto it = cueChains.find (cueId.toString());
    return it != cueChains.end() ? it->second.get() : nullptr;
}

std::vector<juce::Uuid> AudioEngine::getCueChainIds() const
{
    std::vector<juce::Uuid> ids;

    for (const auto& entry : cueChains)
        ids.push_back (juce::Uuid (entry.first));

    return ids;
}

void AudioEngine::removeCueChain (const juce::Uuid& cueId)
{
    std::unique_ptr<PluginChain> dead;

    {
        const juce::ScopedLock sl (lock);
        const auto it = cueChains.find (cueId.toString());

        if (it == cueChains.end())
            return;

        for (auto& p : players)
            if (p->getChain() == it->second.get())
                p->setChain (nullptr);

        dead = std::move (it->second);
        cueChains.erase (it);
    }

    dead.reset();   // plugin instances are released outside the audio lock
}

void AudioEngine::clearCueChains()
{
    std::map<juce::String, std::unique_ptr<PluginChain>> dead;

    {
        const juce::ScopedLock sl (lock);

        for (auto& p : players)
            p->setChain (nullptr);

        dead.swap (cueChains);
    }

    dead.clear();
}

void AudioEngine::setChainListener (PluginChain::Listener* listener)
{
    chainListener = listener;
    masterChain.setListener (listener);

    for (auto& entry : cueChains)
        entry.second->setListener (listener);
}

PluginChain::Factory AudioEngine::makePluginFactory()
{
    return pluginHost.makeFactory (getSampleRate(), getBlockSize());
}

bool AudioEngine::consumePluginStateChanges()
{
    bool changed = masterChain.consumeStateChanged();

    for (auto& entry : cueChains)
        if (entry.second->consumeStateChanged())   // every flag must be cleared, so no short-circuit
            changed = true;

    return changed;
}

//==============================================================================
void AudioEngine::prepare (double newSampleRate, int newBlockSize)
{
    sampleRate.store (newSampleRate > 0.0 ? newSampleRate : 44100.0);
    blockSize.store (juce::jmax (1, newBlockSize));

    {
        const juce::ScopedLock sl (lock);

        mixBuffer.setSize (2, blockSize.load(), false, false, true);
        playerBuffer.setSize (2, blockSize.load(), false, false, true);

        for (auto& p : players)
            p->prepare (sampleRate.load(), blockSize.load());
    }

    masterChain.prepare (sampleRate.load(), blockSize.load());

    for (auto& entry : cueChains)
        entry.second->prepare (sampleRate.load(), blockSize.load());
}

void AudioEngine::renderBlock (juce::AudioBuffer<float>& output, int numSamples)
{
    if (numSamples <= 0)
        return;

    // A driver may deliver more samples than it announced: process in prepared-size chunks instead of
    // growing buffers on the audio thread.
    const int chunkSize = juce::jmax (1, mixBuffer.getNumSamples());
    bool anyFinished = false;

    for (int offset = 0; offset < numSamples; offset += chunkSize)
    {
        const int n = juce::jmin (chunkSize, numSamples - offset);
        mixBuffer.clear (0, n);

        {
            const juce::ScopedLock sl (lock);

            for (auto& p : players)
            {
                if (p->hasFinished())
                    continue;

                const bool stillRunning = p->renderNextBlock (playerBuffer, n);

                for (int ch = 0; ch < 2; ++ch)
                    mixBuffer.addFrom (ch, 0, playerBuffer, ch, 0, n);

                if (! stillRunning)
                    anyFinished = true;
            }
        }

        masterChain.process (mixBuffer, n);

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
        {
            if (ch < 2)
                output.copyFrom (ch, offset, mixBuffer, ch, 0, n);
            else
                output.clear (ch, offset, n);
        }
    }

    if (anyFinished)
        triggerAsyncUpdate();
}

void AudioEngine::reapFinishedPlayers()
{
    std::vector<std::unique_ptr<CuePlayer>> dead;
    dead.reserve (16);   // no allocation while the audio lock is held

    {
        const juce::ScopedLock sl (lock);

        for (auto it = players.begin(); it != players.end();)
        {
            if ((*it)->hasFinished())
            {
                dead.push_back (std::move (*it));
                it = players.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    dead.clear();   // file handles are released outside the audio lock
}

//==============================================================================
void AudioEngine::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    if (numOutputChannels <= 0 || numSamples <= 0)
        return;

    juce::AudioBuffer<float> output (outputChannelData, numOutputChannels, numSamples);
    renderBlock (output, numSamples);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void AudioEngine::audioDeviceStopped()
{
}

void AudioEngine::audioDeviceError (const juce::String& errorMessage)
{
    DBG ("Audio device error: " << errorMessage);
    juce::ignoreUnused (errorMessage);
}

void AudioEngine::handleAsyncUpdate()
{
    reapFinishedPlayers();
}

} // namespace gocue
