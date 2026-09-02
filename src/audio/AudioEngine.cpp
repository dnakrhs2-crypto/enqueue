#include "audio/AudioEngine.h"

#include "audio/MediaFoundationAudioFormat.h"

namespace gocue
{

AudioEngine::AudioEngine (int readAhead)
    : readAheadSamples (juce::jmax (0, readAhead))
{
    formatManager.registerBasicFormats();

    if (MediaFoundationAudioFormat::isAvailable())
        formatManager.registerFormat (new MediaFoundationAudioFormat(), false);   // AAC / M4A / MP4 audio

    mixBuffer.setSize (2, blockSize.load());
    playerBuffer.setSize (CuePlayer::maxChannels, blockSize.load());
    players.reserve (256);   // push_back under the audio lock must not reallocate

    muteRuntime = std::make_unique<PatchRuntime>();
    muteRuntime->patch = AudioPatch::makeDefault ("mute");
    muteRuntime->patch.numCueOutputs = 2;
    muteRuntime->patch.sanitise();
    prepareRuntimeBuffers (*muteRuntime);

    if (readAheadSamples > 0)
        readAheadThread.startThread();
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

juce::String AudioEngine::initialise (const juce::XmlElement* savedDeviceState)
{
    const auto error = deviceManager.initialise (0, maxDeviceOutputs, savedDeviceState, true);

    if (! callbackAdded)
    {
        deviceManager.addAudioCallback (this);
        callbackAdded = true;
    }

    return error;
}

juce::String AudioEngine::setInputsWanted (int channels)
{
    channels = juce::jlimit (0, maxDeviceInputs, channels);

    if (channels <= 0)
        return {};

    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return {};   // no device open: nothing to reconfigure (the mic cues will render silence)

    const int available = device->getInputChannelNames().size();
    const int wanted = juce::jmin (channels, juce::jmax (available, 0));

    if (wanted <= 0)
        return {};

    // the mic cue rows are the first open inputs in device order: inputs 1..wanted must all be open, not just
    // "enough" of them (JUCE packs the active channels, so an open 9-10 would arrive as rows 1-2)
    auto setup = deviceManager.getAudioDeviceSetup();
    bool prefixOpen = ! setup.useDefaultInputChannels;

    for (int i = 0; i < wanted && prefixOpen; ++i)
        prefixOpen = setup.inputChannels[i];

    if (prefixOpen)
        return {};

    const auto previous = setup;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.setRange (0, wanted, true);   // keeps any other inputs the user opened
    const auto error = deviceManager.setAudioDeviceSetup (setup, true);   // restarts the device with the inputs open

    if (error.isNotEmpty())
    {
        deviceManager.setAudioDeviceSetup (previous, true);   // never leave the outputs dead because an input failed
        return error;
    }

    return {};
}

void AudioEngine::reapIfNeeded()
{
    if (reapNeeded.exchange (false, std::memory_order_acq_rel))
        reapFinishedPlayers();
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

    std::vector<std::unique_ptr<PatchRuntime>> oldPatches;

    {
        const juce::ScopedLock sl (lock);
        oldPatches.swap (patchRuntimes);
    }

    oldPatches.clear();
    cueChains.clear();
    masterChain.clear();
}

//==============================================================================
AudioEngine::PatchRuntime* AudioEngine::findRuntime (const juce::Uuid& patchId) const noexcept
{
    for (const auto& r : patchRuntimes)
        if (r != nullptr && r->patch.id == patchId)
            return r.get();

    return nullptr;
}

AudioEngine::PatchRuntime* AudioEngine::runtimeForCue (const Cue& cue) const noexcept
{
    if (auto* r = findRuntime (cue.patchId))
        return r;

    return patchRuntimes.empty() ? nullptr : patchRuntimes.front().get();
}

const AudioPatch* AudioEngine::findPatchForCue (const Cue& cue) const noexcept
{
    const auto* r = runtimeForCue (cue);
    return r != nullptr ? &r->patch : nullptr;
}

const AudioPatch* AudioEngine::findPatch (const juce::Uuid& patchId) const noexcept
{
    const auto* r = findRuntime (patchId);
    return r != nullptr ? &r->patch : nullptr;
}

void AudioEngine::computeRouting (const PatchRuntime& r, std::vector<float>& out) const
{
    const int outs = juce::jmax (2, getNumDeviceOutputs());
    out.assign ((size_t) (r.patch.numCueOutputs * outs), 0.0f);

    for (int k = 0; k < r.patch.numCueOutputs; ++k)
        for (int m = 0; m < outs; ++m)
            out[(size_t) (k * outs + m)] = r.patch.routingGain (k, m);
}

void AudioEngine::prepareRuntimeBuffers (PatchRuntime& r)
{
    const int block = getBlockSize();
    const int outs = juce::jmax (2, getNumDeviceOutputs());
    r.bus.setSize (r.patch.numCueOutputs, block, false, false, true);
    r.routed.setSize (outs, block, false, false, true);
    r.pairScratch.setSize (2, block, false, false, true);

    std::vector<float> routing;
    computeRouting (r, routing);
    r.targetRouting = routing;
    r.currentRouting = std::move (routing);
    r.routingOutputs = outs;
}

template <typename Fn>
void AudioEngine::forEachPatchChain (Fn&& fn) const
{
    for (const auto& r : patchRuntimes)
    {
        if (r == nullptr)
            continue;

        for (const auto& entry : r->cueOutputChains)
            fn (*entry.second);

        for (const auto& entry : r->deviceOutputChains)
            fn (*entry.second);
    }
}

juce::StringArray AudioEngine::setPatches (const std::vector<AudioPatch>& patches, bool applySavedStates)
{
    juce::StringArray errors;
    const auto factory = makePluginFactory();
    std::vector<std::unique_ptr<PatchRuntime>> next;
    std::vector<std::unique_ptr<PatchRuntime>> fresh;   // brand-new runtimes: buffers made outside the lock
    next.reserve (patches.size());
    fresh.reserve (patches.size());

    // Chains that a live runtime is missing are built here and only put into its map under the lock:
    // the audio thread walks those maps.
    struct PendingChain { PatchRuntime* runtime; bool device; std::map<int, std::unique_ptr<PluginChain>> node; };   // the map node is made here, linked under the lock
    std::list<PendingChain> pendingChains;   // a list: no reallocation moves of the map nodes

    // Sanitised copies and routing tables are made outside the lock and swapped in.
    struct Prepared { AudioPatch patch; std::vector<float> routing; };
    std::vector<Prepared> prepared;
    prepared.reserve (patches.size());

    // Chains are (re)built outside the audio lock: plugin creation is slow.
    for (const auto& patch : patches)
    {
        PatchRuntime* existing = findRuntime (patch.id);
        PatchRuntime* target = existing;

        if (existing == nullptr)
        {
            fresh.push_back (std::make_unique<PatchRuntime>());
            target = fresh.back().get();
            target->patch = patch;
            target->patch.sanitise();
            prepareRuntimeBuffers (*target);
        }

        auto restoreChains = [&] (std::map<int, std::unique_ptr<PluginChain>>& chains, const std::vector<std::vector<PluginSlotState>>& saved, bool device)
        {
            for (int i = 0; i < (int) saved.size(); ++i)
            {
                const auto& states = saved[(size_t) i];
                auto it = chains.find (i);

                if (states.empty() && it == chains.end())
                    continue;

                if (it == chains.end())
                {
                    auto chain = std::make_unique<PluginChain>();
                    chain->setListener (chainListener);
                    chain->prepare (getSampleRate(), getBlockSize());
                    errors.addArray (chain->restore (states, factory));

                    if (existing == nullptr)
                    {
                        chains.emplace (i, std::move (chain));                          // not visible to the audio thread yet
                    }
                    else
                    {
                        PendingChain pc { target, device, {} };
                        pc.node.emplace (i, std::move (chain));                         // linked under the lock below, no allocation there
                        pendingChains.push_back (std::move (pc));
                    }

                    continue;
                }

                if (! it->second->matchesStructure (states))
                    errors.addArray (it->second->restore (states, factory));
                else if (applySavedStates)
                    it->second->applyStates (states);   // project open: the file's parameters win over the live ones
            }
        };

        restoreChains (target->cueOutputChains, patch.cueOutputInserts, false);
        restoreChains (target->deviceOutputChains, patch.deviceOutputInserts, true);

        Prepared p;
        p.patch = patch;
        p.patch.sanitise();
        PatchRuntime scratch;
        scratch.patch = p.patch;
        computeRouting (scratch, p.routing);
        prepared.push_back (std::move (p));
    }

    std::vector<std::unique_ptr<PatchRuntime>> dead;
    dead.reserve (patchRuntimes.size());
    std::vector<AudioPatch> oldPatches;   // the runtimes' previous patch copies die outside the lock
    oldPatches.reserve (patches.size());

    {
        const juce::ScopedLock sl (lock);

        for (auto& pc : pendingChains)
            (pc.device ? pc.runtime->deviceOutputChains : pc.runtime->cueOutputChains).insert (pc.node.extract (pc.node.begin()));

        for (size_t i = 0; i < patches.size(); ++i)
        {
            const auto& patch = patches[i];
            std::unique_ptr<PatchRuntime> r;

            for (auto& candidate : patchRuntimes)
                if (candidate != nullptr && candidate->patch.id == patch.id)
                    r = std::move (candidate);

            if (r == nullptr)
                for (auto& candidate : fresh)
                    if (candidate != nullptr && candidate->patch.id == patch.id)
                        r = std::move (candidate);

            if (r == nullptr)
                continue;

            const bool resize = r->patch.numCueOutputs != patch.numCueOutputs || r->routingOutputs != juce::jmax (2, getNumDeviceOutputs());
            std::swap (r->patch, prepared[i].patch);
            oldPatches.push_back (std::move (prepared[i].patch));

            if (resize)
                prepareRuntimeBuffers (*r);   // rare (output count changed): allocation under the lock is accepted
            else if (prepared[i].routing.size() == r->targetRouting.size())
                r->targetRouting.swap (prepared[i].routing);
            else
                computeRouting (*r, r->targetRouting);

            next.push_back (std::move (r));
        }

        for (auto& leftover : patchRuntimes)
            if (leftover != nullptr)
                dead.push_back (std::move (leftover));

        patchRuntimes.swap (next);

        PatchRuntime* defaultRuntime = patchRuntimes.empty() ? nullptr : patchRuntimes.front().get();

        for (auto& p : players)
        {
            bool alive = p->getBusTag() == muteRuntime.get();

            for (const auto& r : patchRuntimes)
                if (r.get() == p->getBusTag())
                    alive = true;

            if (! alive)
                p->setBusTag (defaultRuntime);
        }
    }

    dead.clear();   // removed patches' plugin instances are released outside the audio lock
    oldPatches.clear();
    return errors;
}

void AudioEngine::updatePatchLevels (const AudioPatch& patch)
{
    PatchRuntime* r = findRuntime (patch.id);

    if (r == nullptr)
        return;

    if (r->patch.numCueOutputs != patch.numCueOutputs)
    {
        // the bus width changes: rebuild through setPatches with the current list
        std::vector<AudioPatch> all;

        for (const auto& existing : patchRuntimes)
            all.push_back (existing->patch.id == patch.id ? patch : existing->patch);

        setPatches (all);
        return;
    }

    PatchRuntime scratch;
    scratch.patch = patch;
    scratch.patch.sanitise();
    std::vector<float> routing;
    computeRouting (scratch, routing);

    {
        const juce::ScopedLock sl (lock);
        std::swap (r->patch, scratch.patch);   // O(1): the previous copy is destroyed after the lock

        if (routing.size() == r->targetRouting.size())
            r->targetRouting.swap (routing);
    }
}

PluginChain& AudioEngine::getPatchCueOutputChain (const juce::Uuid& patchId, int cueOutput)
{
    auto* r = findRuntime (patchId);
    jassert (r != nullptr);

    if (r == nullptr)
        return masterChain;   // never expected; keeps the reference valid

    if (const auto it = r->cueOutputChains.find (cueOutput); it != r->cueOutputChains.end())
        return *it->second;

    // built (and its map node allocated) outside the lock, linked into the map the audio thread walks under it
    auto chain = std::make_unique<PluginChain>();
    chain->setListener (chainListener);
    chain->prepare (getSampleRate(), getBlockSize());
    PluginChain* raw = chain.get();
    std::map<int, std::unique_ptr<PluginChain>> node;
    node.emplace (cueOutput, std::move (chain));

    {
        const juce::ScopedLock sl (lock);
        r->cueOutputChains.insert (node.extract (node.begin()));   // node handle: no allocation here
    }

    return *raw;
}

PluginChain* AudioEngine::findPatchCueOutputChain (const juce::Uuid& patchId, int cueOutput) const
{
    const auto* r = findRuntime (patchId);

    if (r == nullptr)
        return nullptr;

    const auto it = r->cueOutputChains.find (cueOutput);
    return it != r->cueOutputChains.end() ? it->second.get() : nullptr;
}

PluginChain& AudioEngine::getPatchDeviceOutputChain (const juce::Uuid& patchId, int deviceOutput)
{
    auto* r = findRuntime (patchId);
    jassert (r != nullptr);

    if (r == nullptr)
        return masterChain;

    if (const auto it = r->deviceOutputChains.find (deviceOutput); it != r->deviceOutputChains.end())
        return *it->second;

    auto chain = std::make_unique<PluginChain>();
    chain->setListener (chainListener);
    chain->prepare (getSampleRate(), getBlockSize());
    PluginChain* raw = chain.get();
    std::map<int, std::unique_ptr<PluginChain>> node;
    node.emplace (deviceOutput, std::move (chain));

    {
        const juce::ScopedLock sl (lock);
        r->deviceOutputChains.insert (node.extract (node.begin()));
    }

    return *raw;
}

PluginChain* AudioEngine::findPatchDeviceOutputChain (const juce::Uuid& patchId, int deviceOutput) const
{
    const auto* r = findRuntime (patchId);

    if (r == nullptr)
        return nullptr;

    const auto it = r->deviceOutputChains.find (deviceOutput);
    return it != r->deviceOutputChains.end() ? it->second.get() : nullptr;
}

void AudioEngine::capturePatchInsertStates (AudioPatch& patch) const
{
    const auto* r = findRuntime (patch.id);

    if (r == nullptr)
        return;

    patch.cueOutputInserts.assign ((size_t) patch.numCueOutputs, {});

    for (const auto& entry : r->cueOutputChains)
        if (entry.first >= 0 && entry.first < patch.numCueOutputs)
            patch.cueOutputInserts[(size_t) entry.first] = entry.second->getStates();

    int maxDevice = -1;

    for (const auto& entry : r->deviceOutputChains)
        if (entry.second->getNumSlots() > 0)
            maxDevice = juce::jmax (maxDevice, entry.first);

    patch.deviceOutputInserts.assign ((size_t) (maxDevice + 1), {});

    for (const auto& entry : r->deviceOutputChains)
        if (entry.first >= 0 && entry.first <= maxDevice)
            patch.deviceOutputInserts[(size_t) entry.first] = entry.second->getStates();
}

void AudioEngine::processInsert (PluginChain& chain, juce::AudioBuffer<float>& buffer, int first, bool stereo,
                                 juce::AudioBuffer<float>& scratch, int numSamples) noexcept
{
    if (first < 0 || first >= buffer.getNumChannels())
        return;

    if (stereo && first + 1 < buffer.getNumChannels())
    {
        float* chans[2] = { buffer.getWritePointer (first), buffer.getWritePointer (first + 1) };
        juce::AudioBuffer<float> view (chans, 2, numSamples);
        chain.process (view, numSamples);
        return;
    }

    // mono: the stereo chain sees the channel on both sides; its left output is kept
    scratch.copyFrom (0, 0, buffer, first, 0, numSamples);
    scratch.copyFrom (1, 0, buffer, first, 0, numSamples);
    chain.process (scratch, numSamples);
    buffer.copyFrom (first, 0, scratch, 0, 0, numSamples);
}

void AudioEngine::renderPatch (PatchRuntime& r, int numSamples) noexcept
{
    const int cueOuts = juce::jmin (r.patch.numCueOutputs, r.bus.getNumChannels());

    for (int k = 0; k < cueOuts; ++k)
    {
        if (r.patch.isSecondOfPair (k))
            continue;

        const auto it = r.cueOutputChains.find (k);

        if (it == r.cueOutputChains.end() || it->second->getNumSlots() == 0)
            continue;

        processInsert (*it->second, r.bus, k, r.patch.isFirstOfPair (k), r.pairScratch, numSamples);
    }

    const int outs = juce::jmin (r.routingOutputs, r.routed.getNumChannels());
    r.routed.clear (0, numSamples);
    const float alpha = (float) juce::jmin (1.0, (double) numSamples / (0.010 * getSampleRate()));

    for (int k = 0; k < cueOuts; ++k)
    {
        const float* src = r.bus.getReadPointer (k);

        for (int m = 0; m < outs; ++m)
        {
            const size_t idx = (size_t) (k * r.routingOutputs + m);

            if (idx >= r.currentRouting.size() || idx >= r.targetRouting.size())
                continue;

            const float g0 = r.currentRouting[idx];
            const float goal = r.targetRouting[idx];
            float g1 = g0 + (goal - g0) * alpha;

            if (std::abs (g1 - goal) < 1.0e-5f)
                g1 = goal;

            if (g0 != 0.0f || g1 != 0.0f)
                r.routed.addFromWithRamp (m, 0, src, numSamples, g0, g1);

            r.currentRouting[idx] = g1;
        }
    }

    for (const auto& entry : r.deviceOutputChains)
        if (entry.first >= 0 && entry.first < outs && entry.second->getNumSlots() > 0)
            processInsert (*entry.second, r.routed, entry.first, false, r.pairScratch, numSamples);

    for (int m = 0; m < juce::jmin (outs, mixBuffer.getNumChannels()); ++m)
        mixBuffer.addFrom (m, 0, r.routed, m, 0, numSamples);
}

//==============================================================================
bool AudioEngine::play (const Cue& cue, const PlayOptions& options, juce::String* errorMessage)
{
    const bool wantsAudition = options.audition || options.silent || ! options.patchOverride.isNull();

    if (! wantsAudition)
    {
        // a loaded instance is waiting: start it instead of opening the file again (an audition needs its own
        // routing / flag, so it starts a fresh instance and the loaded one is dropped below)
        const juce::ScopedLock sl (lock);

        for (auto& existing : players)
        {
            if (existing->getCueId() == cue.id && existing->isLoadedNotStarted() && ! existing->hasFinished() && ! existing->isStopPending())
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

    PatchRuntime* runtime = runtimeForCue (cue);

    if (options.silent)
        runtime = muteRuntime.get();
    else if (! options.patchOverride.isNull())
    {
        if (auto* alternate = findRuntime (options.patchOverride))
            runtime = alternate;
        else
            runtime = muteRuntime.get();   // the alternate patch is gone: an audition must not reach the real outputs
    }

    auto player = std::make_unique<CuePlayer> (cue, formatManager,
                                               readAheadSamples > 0 ? &readAheadThread : nullptr,
                                               readAheadSamples, options.startSeconds,
                                               runtime != nullptr ? runtime->patch.numCueOutputs : 2);

    if (! player->isValid())
    {
        if (errorMessage != nullptr)
            *errorMessage = player->getErrorMessage();

        return false;
    }

    player->prepare (getSampleRate(), getBlockSize());
    player->setStartOrder (++startCounter);
    player->setChain (findCueChain (cue.id));
    player->setBusTag (runtime);
    player->setAudition (options.audition || options.silent || ! options.patchOverride.isNull());
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
    auto* runtime = runtimeForCue (cue);
    auto player = std::make_unique<CuePlayer> (cue, formatManager,
                                               readAheadSamples > 0 ? &readAheadThread : nullptr,
                                               readAheadSamples, startSeconds,
                                               runtime != nullptr ? runtime->patch.numCueOutputs : 2);

    if (! player->isValid())
    {
        if (errorMessage != nullptr)
            *errorMessage = player->getErrorMessage();

        return false;
    }

    player->prepare (getSampleRate(), getBlockSize());
    player->setChain (findCueChain (cue.id));
    player->setBusTag (runtime);
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
        if (p->getCueId() == cueId && p->isLoadedNotStarted() && ! p->hasFinished() && ! p->isStopPending())
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

double AudioEngine::finishCurrentPass (const juce::Uuid& cueId, bool stopAfter)
{
    const juce::ScopedLock sl (lock);
    CuePlayer* newest = nullptr;

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted() && ! p->isStopPending())
            if (newest == nullptr || p->getStartOrder() > newest->getStartOrder())
                newest = p.get();

    if (newest == nullptr)
        return -1.0;

    const auto boundary = newest->requestFinishCurrentPass (stopAfter);

    if (boundary < 0)
        return -1.0;

    const double left = (double) boundary - (double) newest->controlPosition();
    return juce::jmax (0.0, left) / newest->getFileSampleRate() / juce::jmax (AudioCueData::minRate, newest->getLiveRate());
}

double AudioEngine::getLiveRate (const juce::Uuid& cueId) const
{
    LiveState live;
    return getLiveState (cueId, live) ? live.rate : 1.0;
}

double AudioEngine::getFileSampleRate (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);
    const CuePlayer* newest = nullptr;

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted())
            if (newest == nullptr || p->getStartOrder() > newest->getStartOrder())
                newest = p.get();

    return newest != nullptr ? newest->getFileSampleRate() : 44100.0;
}

juce::int64 AudioEngine::getVirtualPosition (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);
    const CuePlayer* newest = nullptr;

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted())
            if (newest == nullptr || p->getStartOrder() > newest->getStartOrder())
                newest = p.get();

    return newest != nullptr ? (juce::int64) newest->getVirtualPosition() : -1;
}

void AudioEngine::setLiveRegion (const juce::Uuid& cueId, double startSeconds, double endSeconds)
{
    CuePlayer* found[16];
    int count = 0;

    {
        const juce::ScopedLock sl (lock);

        for (auto& p : players)
            if (p->getCueId() == cueId && ! p->hasFinished() && count < 16)
                found[count++] = p.get();
    }

    for (int i = 0; i < count; ++i)
        found[i]->setLiveRegion (startSeconds, endSeconds);
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

double AudioEngine::getSecondsToPassEnd (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);
    const CuePlayer* newest = nullptr;

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted() && ! p->isStopPending())
            if (newest == nullptr || p->getStartOrder() > newest->getStartOrder())
                newest = p.get();

    if (newest == nullptr)
        return -1.0;

    const auto end = newest->getCurrentPassEnd();

    if (end < 0)
        return -1.0;

    const double left = (double) end - (double) newest->controlPosition();
    return juce::jmax (0.0, left) / newest->getFileSampleRate() / juce::jmax (AudioCueData::minRate, newest->getLiveRate());
}

void AudioEngine::setLiveSlices (const juce::Uuid& cueId, const std::vector<Slice>& slices, int firstSliceCount)
{
    CuePlayer* found[16];
    int count = 0;

    {
        const juce::ScopedLock sl (lock);

        for (auto& p : players)
            if (p->getCueId() == cueId && ! p->hasFinished() && count < 16)
                found[count++] = p.get();
    }

    for (int i = 0; i < count; ++i)
        found[i]->setLiveSlices (slices, firstSliceCount);   // layout rebuild + read-ahead invalidate outside the lock
}

void AudioEngine::setLiveLevels (const juce::Uuid& cueId, const LevelMatrix& levels, const TrimLevels& trim)
{
    // the matrix copies and gain tables allocate: done outside the lock the audio callback shares
    // (players are only destroyed on this thread, so the pointers stay valid)
    CuePlayer* found[16];
    int count = 0;

    {
        const juce::ScopedLock sl (lock);

        for (auto& p : players)
            if (p->getCueId() == cueId && ! p->hasFinished() && count < 16)
                found[count++] = p.get();
    }

    for (int i = 0; i < count; ++i)
        found[i]->setLiveLevels (levels, trim);
}

juce::int64 AudioEngine::getStartOrder (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);
    juce::int64 best = -1;

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted())
            best = juce::jmax (best, p->getStartOrder());

    return best;
}

bool AudioEngine::getLiveState (const juce::Uuid& cueId, LiveState& out) const
{
    const CuePlayer* best = nullptr;

    {
        const juce::ScopedLock sl (lock);

        for (auto& p : players)
            if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted())
                if (best == nullptr || p->getStartOrder() > best->getStartOrder())
                    best = p.get();
    }

    if (best == nullptr)
        return false;

    // the matrix copy allocates: outside the lock (the player lives until this thread reaps it)
    out.gainDb = best->getLiveGainDb();
    out.levels = best->getLiveLevels();
    out.trim = best->getLiveTrim();
    out.rate = best->getLiveRate();
    return true;
}

void AudioEngine::seekToFraction (const juce::Uuid& cueId, double fraction)
{
    const juce::ScopedLock sl (lock);

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted())
            p->seekToFraction (fraction);
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
        info.audition = p->isAudition();
        info.progress = p->getProgressFraction();
        info.startOrder = p->getStartOrder();
        result.push_back (info);
    }

    return result;
}

bool AudioEngine::isAuditioning (const juce::Uuid& cueId) const
{
    const juce::ScopedLock sl (lock);
    const CuePlayer* newest = nullptr;

    for (auto& p : players)
        if (p->getCueId() == cueId && ! p->hasFinished() && ! p->isLoadedNotStarted())
            if (newest == nullptr || p->getStartOrder() > newest->getStartOrder())
                newest = p.get();   // a restart overlaps the old instance for a few ms: the newest one decides

    return newest != nullptr && newest->isAudition();
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

    forEachPatchChain ([listener] (PluginChain& chain) { chain.setListener (listener); });
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

    forEachPatchChain ([&changed] (PluginChain& chain)
    {
        if (chain.consumeStateChanged())
            changed = true;
    });

    return changed;
}

//==============================================================================
void AudioEngine::prepare (double newSampleRate, int newBlockSize, int newNumDeviceOutputs)
{
    sampleRate.store (newSampleRate > 0.0 ? newSampleRate : 44100.0);
    blockSize.store (juce::jmax (1, newBlockSize));

    if (newNumDeviceOutputs >= 1)
        numDeviceOutputs.store (juce::jlimit (1, LevelMatrix::maxOutputs, newNumDeviceOutputs));

    {
        const juce::ScopedLock sl (lock);

        mixBuffer.setSize (juce::jmax (2, getNumDeviceOutputs()), blockSize.load(), false, false, true);
        playerBuffer.setSize (CuePlayer::maxChannels, blockSize.load(), false, false, true);
        deviceScratch.setSize (getNumDeviceOutputs() >= 32 ? getNumDeviceOutputs() : 1, blockSize.load(), false, false, true);

        for (auto& r : patchRuntimes)
            prepareRuntimeBuffers (*r);

        if (muteRuntime != nullptr)
            prepareRuntimeBuffers (*muteRuntime);

        for (auto& p : players)
            p->prepare (sampleRate.load(), blockSize.load());
    }

    masterChain.prepare (sampleRate.load(), blockSize.load());

    for (auto& entry : cueChains)
        entry.second->prepare (sampleRate.load(), blockSize.load());

    forEachPatchChain ([this] (PluginChain& chain) { chain.prepare (sampleRate.load(), blockSize.load()); });
}

void AudioEngine::renderBlock (juce::AudioBuffer<float>& output, int numSamples, const float* const* inputs, int numInputs)
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

            for (auto& r : patchRuntimes)
                r->bus.clear (0, n);

            muteRuntime->bus.clear (0, n);   // auditions with no output mix in here and go nowhere

            for (auto& p : players)
            {
                if (p->hasFinished())
                    continue;

                if (p->isMic())
                    p->setInputBlock (inputs, numInputs, offset);

                const bool stillRunning = p->renderNextBlock (playerBuffer, n);
                auto* r = static_cast<PatchRuntime*> (p->getBusTag());
                p->mixIntoBus (r != nullptr ? r->bus : mixBuffer, playerBuffer, n);

                if (! stillRunning)
                    anyFinished = true;
            }

            for (auto& r : patchRuntimes)
                renderPatch (*r, n);
        }

        masterChain.process (mixBuffer, n);   // legacy master inserts on device outputs 1-2

        for (int ch = 0; ch < output.getNumChannels(); ++ch)
        {
            if (ch < mixBuffer.getNumChannels())
                output.copyFrom (ch, offset, mixBuffer, ch, 0, n);
            else
                output.clear (ch, offset, n);
        }
    }

    if (anyFinished)
        reapNeeded.store (true, std::memory_order_release);   // the UI timer reaps: no message posting from the audio thread
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
void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    if (numOutputChannels <= 0 || numSamples <= 0)
        return;

    if (numOutputChannels < 32)   // juce::AudioBuffer keeps its channel table inline below 32 channels: no allocation
    {
        juce::AudioBuffer<float> output (outputChannelData, numOutputChannels, numSamples);
        renderBlock (output, numSamples, inputChannelData, numInputChannels);
        return;
    }

    // 32 channels or more: render into the prepared scratch and copy out
    const int chunk = juce::jmax (1, deviceScratch.getNumSamples());
    const int inputs = juce::jmin (numInputChannels, (int) inputPointers.size());

    for (int offset = 0; offset < numSamples; offset += chunk)
    {
        const int n = juce::jmin (chunk, numSamples - offset);

        // the mic players index the input block themselves: hand them the pointers shifted by 'offset' (fixed array, no allocation)
        for (int ch = 0; ch < inputs; ++ch)
            inputPointers[(size_t) ch] = inputChannelData[ch] != nullptr ? inputChannelData[ch] + offset : nullptr;

        renderBlock (deviceScratch, n, inputs > 0 ? inputPointers.data() : nullptr, inputs);

        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (ch < deviceScratch.getNumChannels())
                juce::FloatVectorOperations::copy (outputChannelData[ch] + offset, deviceScratch.getReadPointer (ch), n);
            else
                juce::FloatVectorOperations::clear (outputChannelData[ch] + offset, n);
        }
    }
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    numDeviceInputs.store (device->getActiveInputChannels().countNumberOfSetBits(), std::memory_order_relaxed);
    prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
             juce::jmax (1, device->getActiveOutputChannels().countNumberOfSetBits()));
}

void AudioEngine::audioDeviceStopped()
{
    numDeviceInputs.store (0, std::memory_order_relaxed);   // until a device starts again there are no inputs
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
