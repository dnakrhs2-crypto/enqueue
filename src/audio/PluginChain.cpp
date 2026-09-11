#include "audio/PluginChain.h"

#include <cmath>
#include <set>

namespace gocue
{

PluginChain::~PluginChain()
{
    clearSlots (false);   // an owner being torn down must not be told about it (a retired chain is not an edit)
}

void PluginChain::prepare (double newSampleRate, int newBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    blockSize = juce::jmax (1, newBlockSize);
    bypassRampSamples = juce::jmax (32, (int) std::lround (sampleRate * bypassRampSeconds));
    midi.ensureSize (4096);   // an effect that emits MIDI must not make the buffer grow on the audio thread

    {
        const juce::ScopedLock sl (lock);

        for (auto& slot : slots)
            if (slot->plugin != nullptr)
                prepareSlot (*slot);
    }

    updateTailCache();
}

bool PluginChain::prepareSlot (Slot& slot)
{
    auto& plugin = *slot.plugin;
    bool ok = true;
    int wanted = 2;
    int latency = 0;

    try
    {
        // Stereo in / stereo out on the main buses, every other bus disabled.
        juce::AudioProcessor::BusesLayout layout;

        for (int i = 0; i < plugin.getBusCount (true); ++i)
            layout.inputBuses.add (i == 0 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());

        for (int i = 0; i < plugin.getBusCount (false); ++i)
            layout.outputBuses.add (i == 0 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());

        if (! plugin.setBusesLayout (layout))
            plugin.enableAllBuses();   // fall back to whatever the plugin insists on; scratch buffers adapt

        // releaseResources first so a re-prepare (a device / sample-rate / buffer change) actually takes effect:
        // JUCE's VST3 prepareToPlay early-returns when the plugin is active and the details already match, and
        // setRateAndBufferSizeDetails has just set them to the new values - without this the DSP keeps its old setup.
        plugin.releaseResources();
        plugin.setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin.prepareToPlay (sampleRate, blockSize);
        wanted = juce::jmax (2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels());
        latency = juce::jmax (0, plugin.getLatencySamples());   // what a look-ahead limiter / linear-phase EQ delays by
    }
    catch (...)
    {
        markFaulted (slot);   // it threw while getting ready: it never runs
        ok = false;
    }

    if (wanted > maxScratchChannels)
    {
        markFaulted (slot);   // a buffer view over that many channels makes JUCE allocate on the audio thread
        ok = false;
    }

    slot.numScratchChannels = juce::jmin (maxScratchChannels, wanted);
    slot.scratch.setSize (slot.numScratchChannels, blockSize, false, false, true);
    sizeDelayLine (slot, latency, blockSize);
    slot.wetMix = slot.bypassed.load() ? 0.0f : 1.0f;   // a fresh preparation starts where the switch is: no ramp
    return ok;
}

void PluginChain::markFaulted (Slot& slot) noexcept
{
    slot.faulted.store (true, std::memory_order_relaxed);
    faultRaised.store (true, std::memory_order_release);
}

juce::StringArray PluginChain::takeNewFaults()
{
    juce::StringArray names;

    if (! faultRaised.exchange (false, std::memory_order_acq_rel))
        return names;

    for (auto& slot : slots)   // message thread only (the one that edits 'slots'): no chain lock, so the callback is never made to skip the chain for this
    {
        if (slot->faulted.load (std::memory_order_relaxed) && ! slot->faultReported)
        {
            slot->faultReported = true;
            names.add (slot->plugin != nullptr ? slot->plugin->getName() : slot->state.name);
        }
    }

    return names;
}

int PluginChain::getLatencySamples() const
{
    int total = 0;

    for (auto& slot : slots)   // message thread only: no chain lock (see takeNewFaults)
        if (slot->plugin != nullptr)
            total += slot->latency.load (std::memory_order_relaxed);   // bypassed or faulted too: their dry signal is delayed by as much

    return total;
}

int PluginChain::getNumSlots() const
{
    // any thread, no lock: the UI asks on every refresh (a chain lock here would make the callback pass the whole chain
    // dry, raw and out of time, whenever the two coincide) and the patch renderer asks from the callback itself
    return slotCount.load (std::memory_order_relaxed);
}

PluginChain::Slot& PluginChain::getSlot (int index)
{
    return *slots[(size_t) index];
}

const PluginChain::Slot& PluginChain::getSlot (int index) const
{
    return *slots[(size_t) index];
}

void PluginChain::insertSlot (std::unique_ptr<Slot> slot, int insertAt)
{
    for (auto& other : slots)   // message thread only: the ids are not the audio thread's business
        if (other->state.slotId == slot->state.slotId)
        {
            slot->state.slotId = juce::Uuid();   // the same preset twice: each slot its own
            break;
        }

    {
        const juce::ScopedLock sl (lock);

        if (insertAt < 0 || insertAt > (int) slots.size())
            insertAt = (int) slots.size();

        slots.insert (slots.begin() + insertAt, std::move (slot));
        slotCount.store ((int) slots.size(), std::memory_order_relaxed);
    }

    notifyChanged();
}

void PluginChain::destroySlot (std::unique_ptr<Slot> slot)
{
    if (slot->plugin != nullptr)
    {
        slot->plugin->removeListener (this);

        if (listener != nullptr)
            listener->pluginAboutToBeRemoved (*this, *slot->plugin);

        try
        {
            slot->plugin->releaseResources();
        }
        catch (...) {}   // a plugin that throws on its way out must not take the app with it
    }

    slot.reset();
}

void PluginChain::addPlugin (std::unique_ptr<juce::AudioPluginInstance> plugin, const PluginSlotState& initialState, int insertAt)
{
    if (plugin == nullptr)
        return;

    auto slot = std::make_unique<Slot>();
    slot->state = initialState;
    slot->bypassed.store (initialState.bypassed);

    if (initialState.stateBase64.isNotEmpty())
    {
        juce::MemoryOutputStream decoded;

        if (juce::Base64::convertFromBase64 (decoded, initialState.stateBase64) && decoded.getDataSize() > 0)
        {
            try
            {
                plugin->setStateInformation (decoded.getData(), (int) decoded.getDataSize());
            }
            catch (...)
            {
                markFaulted (*slot);   // it threw on its saved state: not trusted with the audio
            }
        }
    }

    slot->plugin = std::move (plugin);
    const bool ready = prepareSlot (*slot) && ! slot->faulted.load (std::memory_order_relaxed);

    if (! ready)
    {
        // it threw on its saved state or while getting ready: not trusted with anything more. The slot stays, empty
        // and marked, with the saved state kept, so the operator sees it and can put the plugin back
        try { slot->state.name = slot->plugin->getName(); } catch (...) {}
        auto dead = std::move (slot->plugin);
        try { dead.reset(); } catch (...) {}
        insertSlot (std::move (slot), insertAt);
        return;
    }

    slot->plugin->addListener (this);   // after restore + prepare: only real user edits count as changes
    insertSlot (std::move (slot), insertAt);
}

void PluginChain::addMissingSlot (const PluginSlotState& state, int insertAt)
{
    auto slot = std::make_unique<Slot>();
    slot->state = state;
    slot->bypassed.store (state.bypassed);
    insertSlot (std::move (slot), insertAt);
}

void PluginChain::removePlugin (int index)
{
    std::unique_ptr<Slot> dead;

    {
        const juce::ScopedLock sl (lock);

        if (index < 0 || index >= (int) slots.size())
            return;

        dead = std::move (slots[(size_t) index]);
        slots.erase (slots.begin() + index);
        slotCount.store ((int) slots.size(), std::memory_order_relaxed);
    }

    destroySlot (std::move (dead));
    notifyChanged();
}

bool PluginChain::movePlugin (int from, int to)
{
    {
        const juce::ScopedLock sl (lock);
        const int n = (int) slots.size();

        if (from < 0 || from >= n || to < 0 || to >= n || from == to)
            return false;

        auto moved = std::move (slots[(size_t) from]);
        slots.erase (slots.begin() + from);
        slots.insert (slots.begin() + to, std::move (moved));
    }

    notifyChanged();
    return true;
}

void PluginChain::setBypassed (int index, bool shouldBypass)
{
    // no chain lock here: the flag is atomic and 'slots' is not touched (message thread only), so the callback is not
    // made to pass the whole chain dry for a switch - the slot itself crossfades to / from its delayed dry signal
    if (index < 0 || index >= (int) slots.size())
        return;

    slots[(size_t) index]->bypassed.store (shouldBypass);
    slots[(size_t) index]->state.bypassed = shouldBypass;
    notifyChanged();
}

void PluginChain::clear()
{
    clearSlots (true);
}

void PluginChain::clearSlots (bool notify)
{
    std::vector<std::unique_ptr<Slot>> dead;

    {
        const juce::ScopedLock sl (lock);
        dead.swap (slots);
        slotCount.store (0, std::memory_order_relaxed);
    }

    if (dead.empty())
        return;

    for (auto& slot : dead)
        destroySlot (std::move (slot));

    if (notify)
        notifyChanged();
}

bool PluginChain::matchesStructure (const std::vector<PluginSlotState>& states) const
{
    if (slots.size() != states.size())
        return false;

    for (size_t i = 0; i < slots.size(); ++i)
    {
        const auto& slot = *slots[i];
        const auto& s = states[i];

        // the bypass flag is not structure: applyStates() sets it, so the undo of a bypass toggle keeps the instances
        // (a rebuild would drop every delay / reverb history and reload the plugins mid-show)

        if (slot.plugin != nullptr)
        {
            const auto description = slot.plugin->getPluginDescription();

            if (description.uniqueId != s.uniqueId || description.fileOrIdentifier != s.fileOrIdentifier)
                return false;
        }
        else if (slot.state.uniqueId != s.uniqueId || slot.state.fileOrIdentifier != s.fileOrIdentifier)
        {
            return false;
        }
    }

    return true;
}

void PluginChain::applyStates (const std::vector<PluginSlotState>& states)
{
    if (! matchesStructure (states))
        return;

    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto& slot = *slots[i];
        const auto& s = states[i];
        slot.state = s;
        slot.bypassed.store (s.bypassed);

        if (slot.plugin != nullptr && s.stateBase64.isNotEmpty())
        {
            juce::MemoryOutputStream decoded;

            if (juce::Base64::convertFromBase64 (decoded, s.stateBase64) && decoded.getDataSize() > 0)
            {
                // the same order process() takes: chain lock, then the plugin's own callback lock
                const juce::ScopedLock sl (lock);
                const juce::ScopedLock callbackLock (slot.plugin->getCallbackLock());

                try
                {
                    slot.plugin->setStateInformation (decoded.getData(), (int) decoded.getDataSize());
                }
                catch (...)
                {
                    markFaulted (slot);
                }
            }
        }
    }

    notifyChanged();
}

std::vector<PluginSlotState> PluginChain::getStates (bool* complete) const
{
    std::vector<PluginSlotState> result;

    for (auto& slot : slots)   // only the message thread edits 'slots', so no lock is needed here
    {
        PluginSlotState s = slot->state;
        s.bypassed = slot->bypassed.load();

        if (slot->plugin != nullptr)
        {
            bool captured = true;

            try
            {
                const auto description = slot->plugin->getPluginDescription();
                s.format = description.pluginFormatName;
                s.name = description.name;
                s.fileOrIdentifier = description.fileOrIdentifier;
                s.uniqueId = description.uniqueId;

                if (const auto xml = description.createXml())
                    s.descriptionXml = xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());

                juce::MemoryBlock block;

                {
                    const juce::ScopedLock callbackLock (slot->plugin->getCallbackLock());   // not concurrently with processBlock()
                    slot->plugin->getStateInformation (block);
                }

                s.stateBase64 = block.getSize() > 0 ? juce::Base64::toBase64 (block.getData(), block.getSize()) : juce::String();
            }
            catch (...)
            {
                captured = false;   // the last state that was read stays in 's' (the slot's cache): the caller is told
            }

            if (captured)
                slot->state = s;   // the last good state, should a later read fail
            else if (complete != nullptr)
                *complete = false;
        }

        result.push_back (std::move (s));
    }

    return result;
}

juce::StringArray PluginChain::restore (const std::vector<PluginSlotState>& states, const Factory& factory)
{
    // Build every new slot first (plugin creation can take a while), then swap the whole list under
    // the lock so a running cue is never heard dry or half-chained meanwhile.
    juce::StringArray errors;
    std::vector<std::unique_ptr<Slot>> fresh;
    std::set<juce::String> ids;

    for (const auto& state : states)
    {
        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> instance;

        if (factory)
            instance = factory (state, error);

        auto slot = std::make_unique<Slot>();
        slot->state = state;
        slot->bypassed.store (state.bypassed);

        if (slot->state.slotId.isNull() || ! ids.insert (slot->state.slotId.toString()).second)
        {
            slot->state.slotId = juce::Uuid();   // a file with two slots of one id (or none): each its own
            ids.insert (slot->state.slotId.toString());
        }

        if (instance != nullptr)
        {
            if (state.stateBase64.isNotEmpty())
            {
                juce::MemoryOutputStream decoded;

                if (juce::Base64::convertFromBase64 (decoded, state.stateBase64) && decoded.getDataSize() > 0)
                {
                    try
                    {
                        instance->setStateInformation (decoded.getData(), (int) decoded.getDataSize());
                    }
                    catch (...)
                    {
                        markFaulted (*slot);
                    }
                }
            }

            slot->plugin = std::move (instance);

            if (prepareSlot (*slot) && ! slot->faulted.load (std::memory_order_relaxed))
            {
                slot->plugin->addListener (this);   // after restore + prepare: only real user edits count as changes
            }
            else
            {
                // it threw on its saved state or while getting ready: the slot stays empty and marked, the state kept
                auto dead = std::move (slot->plugin);
                try { dead.reset(); } catch (...) {}
                errors.add (state.name + ": " + juce::String::fromUTF8 ("플러그인이 준비 중 오류를 내 비워 두었습니다 (저장된 설정은 그대로 둡니다)"));
            }
        }
        else
        {
            errors.add (state.name + ": " + (error.isNotEmpty() ? error : juce::String::fromUTF8 ("이 PC에 없는 플러그인입니다 (자리는 비워 두고 저장된 설정은 그대로 둡니다)")));
        }

        fresh.push_back (std::move (slot));
    }

    std::vector<std::unique_ptr<Slot>> old;

    {
        const juce::ScopedLock sl (lock);
        old.swap (slots);
        slots.swap (fresh);
        slotCount.store ((int) slots.size(), std::memory_order_relaxed);
    }

    for (auto& slot : old)
        destroySlot (std::move (slot));

    notifyChanged();
    return errors;
}

double PluginChain::getTailSeconds() const
{
    // the audio thread asks: the value was worked out on the message thread (no lock, no plugin call here)
    return (double) tailSecondsCache.load (std::memory_order_relaxed);
}

void PluginChain::updateTailCache()
{
    // message thread only: 'slots' is iterated without the chain lock (see takeNewFaults), and each plugin is asked
    // under its own callback lock - not concurrently with its processBlock, and the callback passes that one slot
    // through its delay line meanwhile instead of skipping the whole chain out of time
    double tail = 0.0;

    for (auto& slot : slots)
    {
        if (slot->plugin == nullptr)
            continue;

        // the plugin's latency is in flight on either path (its own delay when active, the dry line when bypassed or
        // faulted): a cue must play on for that long after its file ends or the last samples are cut
        tail = juce::jmin (maxTailSeconds, tail + (double) slot->latency.load (std::memory_order_relaxed) / sampleRate);

        if (slot->bypassed.load() || slot->faulted.load (std::memory_order_relaxed))
            continue;   // its output is discarded: its tail does not ring

        double t = maxTailSeconds;

        try
        {
            const juce::ScopedLock callbackLock (slot->plugin->getCallbackLock());
            t = slot->plugin->getTailLengthSeconds();
        }
        catch (...) {}   // a plugin that throws here counts as the longest tail

        if (! std::isfinite (t))
        {
            tail = maxTailSeconds;
            break;
        }

        // Plugins run in series: a reverb tail feeding a delay rings for the sum of both.
        tail = juce::jmin (maxTailSeconds, tail + juce::jmax (0.0, t));
    }

    tailSecondsCache.store ((float) juce::jlimit (0.0, maxTailSeconds, tail), std::memory_order_relaxed);
}

void PluginChain::refreshPluginCaches()
{
    updateTailCache();
    updateDelayLines();
}

void PluginChain::sizeDelayLine (Slot& slot, int newLatency, int block)
{
    newLatency = juce::jmax (0, newLatency);
    slot.dryDelay.setSize (2, newLatency + juce::jmax (1, block), false, true, true);
    slot.dryDelay.clear();
    slot.dryDelayWrite = 0;
    slot.latency.store (newLatency, std::memory_order_relaxed);
}

void PluginChain::updateDelayLines()
{
    // message thread only. The latencies are read first without the chain lock (the plugin under its callback lock,
    // as in updateTailCache); the lock is taken - and the callback made to pass the chain dry for a block - only for a
    // line that really has to be resized, which happens when a plugin's look-ahead / oversampling changed
    std::vector<std::pair<Slot*, int>> resize;

    for (auto& slot : slots)
    {
        if (slot->plugin == nullptr)
            continue;

        const int known = slot->latency.load (std::memory_order_relaxed);
        int latency = known;

        try
        {
            const juce::ScopedLock callbackLock (slot->plugin->getCallbackLock());
            latency = juce::jmax (0, slot->plugin->getLatencySamples());
        }
        catch (...) {}   // a plugin that throws here keeps the latency it last reported

        if (latency != known || slot->dryDelay.getNumSamples() < latency + blockSize)
            resize.emplace_back (slot.get(), latency);
    }

    if (resize.empty())
        return;

    const juce::ScopedLock sl (lock);   // the callback reads and writes the lines: resized only while it is out

    for (auto& [slot, latency] : resize)
        sizeDelayLine (*slot, latency, blockSize);   // the line starts over: one silent gap of the new length, as the plugin's own buffers do
}

void PluginChain::delayDryInPlace (Slot& slot, juce::AudioBuffer<float>& dry, int numSamples) noexcept
{
    const int latency = slot.latency.load (std::memory_order_relaxed);
    const int capacity = slot.dryDelay.getNumSamples();

    if (latency <= 0 || slot.dryDelay.getNumChannels() < 2 || capacity <= latency)
        return;   // nothing to delay by (or a line that is not ready): the signal passes as it is

    const int start = slot.dryDelayWrite;

    for (int ch = 0; ch < 2 && ch < dry.getNumChannels(); ++ch)
    {
        float* d = dry.getWritePointer (ch);
        float* ring = slot.dryDelay.getWritePointer (ch);
        int w = start;

        for (int i = 0; i < numSamples; ++i)
        {
            int r = w - latency;

            if (r < 0)
                r += capacity;

            const float in = d[i];
            d[i] = ring[r];
            ring[w] = in;

            if (++w == capacity)
                w = 0;
        }
    }

    slot.dryDelayWrite = (start + numSamples) % capacity;
}

bool PluginChain::isFinite (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    for (int ch = 0; ch < 2 && ch < buffer.getNumChannels(); ++ch)
    {
        const float* data = buffer.getReadPointer (ch);

        for (int i = 0; i < numSamples; ++i)
            if (! std::isfinite (data[i]))
                return false;
    }

    return true;
}

void PluginChain::process (juce::AudioBuffer<float>& buffer, int numSamples)
{
    // the slot list is edited on the message thread under this lock (an add, a swap, a state restore that can take a
    // while): the callback never waits for it - this block passes dry instead
    const juce::ScopedTryLock sl (lock);

    if (! sl.isLocked())
        return;

    // a block larger than the chain was prepared for goes through in pieces: the scratch buffers never grow here
    const int chunk = juce::jmax (1, blockSize);

    if (numSamples <= chunk)
    {
        processLocked (buffer, numSamples);
        return;
    }

    for (int offset = 0; offset < numSamples; offset += chunk)
    {
        const int n = juce::jmin (chunk, numSamples - offset);
        juce::AudioBuffer<float> part (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), offset, n);   // a view: no allocation
        processLocked (part, n);
    }
}

void PluginChain::processLocked (juce::AudioBuffer<float>& buffer, int numSamples)
{
    for (auto& slot : slots)
    {
        if (slot->plugin == nullptr)
            continue;

        auto& plugin = *slot->plugin;
        auto& scratch = slot->scratch;

        if (scratch.getNumSamples() < numSamples)
            continue;   // a block larger than prepared for: the owner chunks its blocks, so this does not happen - and never allocates here

        // Whatever becomes of this slot, the signal leaves it delayed by the plugin's latency - by the plugin on the
        // wet path, by delayDryInPlace on the dry one - so a bypass, a busy block or a fault never moves the sound in time.
        if (slot->faulted.load (std::memory_order_relaxed))
        {
            delayDryInPlace (*slot, buffer, numSamples);   // dry pass: it threw once, it is not trusted with the audio again
            continue;
        }

        const bool bypassed = slot->bypassed.load (std::memory_order_relaxed);
        const int ins = plugin.getTotalNumInputChannels();
        const int outs = plugin.getTotalNumOutputChannels();
        midi.clear();

        // The plugin's callback lock is held while it runs (as juce::AudioProcessorPlayer does) - but never waited
        // for: the message thread holds it while it captures state for a save, and a slow plugin there must not stall
        // every channel. Busy, or suspended (loading a preset): a dry pass for this block, in time.
        const juce::ScopedTryLock callbackLock (plugin.getCallbackLock());

        if (! callbackLock.isLocked() || plugin.isSuspended())
        {
            if (slot->busyBlocks.fetch_add (1, std::memory_order_relaxed) + 1 == stallBlocks)
                stallRaised.store (true, std::memory_order_release);   // a second or two of dry passes: the operator hears of it

            delayDryInPlace (*slot, buffer, numSamples);
            continue;
        }

        slot->busyBlocks.store (0, std::memory_order_relaxed);

        // The plugin runs on a copy when it needs more than two channels, otherwise in place with the dry input copied
        // aside first. Either way both signals are at hand afterwards - 'wet' (the plugin's output) and 'dry' (the
        // input, then delayed by the plugin's latency) - and the slot hands on the one the bypass switch asks for,
        // crossfading over bypassRampSamples when the switch has just moved: no click, and no jump in time, as the
        // two are aligned. The plugin runs while bypassed too, so its delay lines and reverb tails stay current.
        const bool viaScratch = slot->numScratchChannels != 2 || ins > 2 || outs > 2;
        juce::AudioBuffer<float>& wet = viaScratch ? scratch : buffer;
        juce::AudioBuffer<float>& dry = viaScratch ? buffer : scratch;

        if (viaScratch)
            scratch.clear (0, numSamples);

        for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
            scratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);   // in place: the dry input, kept - a plugin that throws half-way must not leave its partial block behind

        juce::AudioBuffer<float> view (wet.getArrayOfWritePointers(), viaScratch ? slot->numScratchChannels : 2, 0, numSamples);
        bool ok = true;

        try
        {
            plugin.processBlock (view, midi);
        }
        catch (...)
        {
            ok = false;   // the show goes on without this plugin
        }

        if (ok && ! isFinite (view, numSamples))
            ok = false;   // NaN / Inf would poison the buses

        if (! ok)
        {
            markFaulted (*slot);

            if (! viaScratch)
                for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                    buffer.copyFrom (ch, 0, scratch, ch, 0, numSamples);   // the input passes through untouched...

            delayDryInPlace (*slot, buffer, numSamples);   // ...and in time
            continue;
        }

        if (outs == 1)
            wet.copyFrom (1, 0, wet, 0, 0, numSamples);   // mono-out plugin: mirror to the right channel

        delayDryInPlace (*slot, dry, numSamples);   // every block, used or not: the line is current the moment it is needed

        const float target = bypassed ? 0.0f : 1.0f;

        if (slot->wetMix == target)
        {
            // settled: the signal asked for goes on (when it is not in 'buffer' already), the other is discarded
            if ((target > 0.5f) == viaScratch)
                for (int ch = 0; ch < 2 && ch < buffer.getNumChannels() && ch < scratch.getNumChannels(); ++ch)
                    buffer.copyFrom (ch, 0, scratch, ch, 0, numSamples);
        }
        else
        {
            const float step = 1.0f / (float) juce::jmax (1, bypassRampSamples);
            float mix = slot->wetMix;

            for (int ch = 0; ch < 2 && ch < buffer.getNumChannels() && ch < scratch.getNumChannels(); ++ch)
            {
                const float* w = wet.getReadPointer (ch);
                const float* d = dry.getReadPointer (ch);
                float* out = buffer.getWritePointer (ch);   // one of w / d: each sample is read before it is written
                float m = slot->wetMix;

                for (int i = 0; i < numSamples; ++i)
                {
                    m = target > m ? juce::jmin (target, m + step) : juce::jmax (target, m - step);
                    out[i] = w[i] * m + d[i] * (1.0f - m);
                }

                mix = m;
            }

            slot->wetMix = mix;
        }
    }
}

void PluginChain::resetProcessing() noexcept
{
    // message thread (the panic gate has closed): reset() may allocate or block, so it never runs in the callback
    const juce::ScopedLock sl (lock);

    for (auto& slot : slots)
    {
        if (slot->plugin == nullptr || slot->faulted.load (std::memory_order_relaxed))
            continue;

        const juce::ScopedLock callbackLock (slot->plugin->getCallbackLock());

        if (slot->plugin->isSuspended())
            continue;

        try
        {
            slot->plugin->reset();
        }
        catch (...)
        {
            markFaulted (*slot);   // and the operator hears of it, like any other fault
        }

        slot->scratch.clear();
        slot->dryDelay.clear();   // the compensation line starts over with the plugin's own
        slot->dryDelayWrite = 0;
    }
}

void PluginChain::notifyChanged()
{
    updateTailCache();   // slots added, removed, moved, bypassed: what the callback reads is current before anyone is told

    if (listener != nullptr)
        listener->chainChanged (*this);
}

juce::StringArray PluginChain::takeNewStalls()
{
    juce::StringArray names;

    if (! stallRaised.exchange (false, std::memory_order_acq_rel))
        return names;

    for (auto& slot : slots)   // message thread only: no chain lock (see takeNewFaults)
    {
        if (slot->plugin != nullptr && ! slot->stallReported && slot->busyBlocks.load (std::memory_order_relaxed) >= stallBlocks)
        {
            slot->stallReported = true;
            names.add (slot->plugin->getName());
        }
    }

    return names;
}

void PluginChain::audioProcessorParameterChanged (juce::AudioProcessor*, int, float)
{
    stateChanged.store (true, std::memory_order_release);
}

void PluginChain::audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&)
{
    stateChanged.store (true, std::memory_order_release);
}

} // namespace gocue
