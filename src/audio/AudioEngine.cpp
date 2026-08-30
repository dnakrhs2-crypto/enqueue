#include "audio/AudioEngine.h"

namespace gocue
{

AudioEngine::AudioEngine (int readAhead)
    : readAheadSamples (juce::jmax (0, readAhead))
{
    formatManager.registerBasicFormats();

    mixBuffer.setSize (2, blockSize.load());
    playerBuffer.setSize (2, blockSize.load());

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
bool AudioEngine::play (const Cue& cue, juce::String* errorMessage)
{
    auto player = std::make_unique<CuePlayer> (cue, formatManager,
                                               readAheadSamples > 0 ? &readAheadThread : nullptr,
                                               readAheadSamples);

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

bool AudioEngine::isPlaying (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished())
            return true;

    return false;
}

std::vector<AudioEngine::PlayingCue> AudioEngine::getPlayingCues() const
{
    std::vector<PlayingCue> result;
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
    {
        if (p->hasFinished())
            continue;

        PlayingCue info;
        info.id = p->getCueId();
        info.positionSeconds = p->getPositionSeconds();
        info.lengthSeconds = p->getLengthSeconds();
        info.fadingOut = p->isFadingOut();
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
        if (p->hasFinished() || (ignoreFadingOut && p->isFadingOut()))
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
        if (! p->hasFinished())
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

    if (mixBuffer.getNumSamples() < numSamples)
    {
        // Only happens if a driver delivers more samples than it announced.
        mixBuffer.setSize (2, numSamples, false, false, true);
        playerBuffer.setSize (2, numSamples, false, false, true);
    }

    mixBuffer.clear (0, numSamples);
    bool anyFinished = false;

    {
        const juce::ScopedLock sl (lock);

        for (auto& p : players)
        {
            if (p->hasFinished())
                continue;

            const bool stillRunning = p->renderNextBlock (playerBuffer, numSamples);

            for (int ch = 0; ch < 2; ++ch)
                mixBuffer.addFrom (ch, 0, playerBuffer, ch, 0, numSamples);

            if (! stillRunning)
                anyFinished = true;
        }
    }

    masterChain.process (mixBuffer, numSamples);

    for (int ch = 0; ch < output.getNumChannels(); ++ch)
    {
        if (ch < 2)
            output.copyFrom (ch, 0, mixBuffer, ch, 0, numSamples);
        else
            output.clear (ch, 0, numSamples);
    }

    if (anyFinished)
        triggerAsyncUpdate();
}

void AudioEngine::reapFinishedPlayers()
{
    std::vector<std::unique_ptr<CuePlayer>> dead;

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
