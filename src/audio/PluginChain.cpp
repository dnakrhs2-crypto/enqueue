#include "audio/PluginChain.h"

#include <cmath>

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
    midi.ensureSize (4096);   // an effect that emits MIDI must not make the buffer grow on the audio thread

    const juce::ScopedLock sl (lock);

    for (auto& slot : slots)
        if (slot->plugin != nullptr)
            prepareSlot (*slot);
}

void PluginChain::prepareSlot (Slot& slot)
{
    auto& plugin = *slot.plugin;

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

        plugin.setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin.prepareToPlay (sampleRate, blockSize);
    }
    catch (...)
    {
        markFaulted (slot);   // it threw while getting ready: it never runs
    }

    slot.numScratchChannels = juce::jmax (2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels());
    slot.scratch.setSize (slot.numScratchChannels, blockSize, false, false, true);
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

    const juce::ScopedLock sl (lock);

    for (auto& slot : slots)
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
    const juce::ScopedLock sl (lock);
    int total = 0;

    for (auto& slot : slots)
        if (slot->plugin != nullptr && ! slot->bypassed.load (std::memory_order_relaxed) && ! slot->faulted.load (std::memory_order_relaxed))
            total += juce::jmax (0, slot->plugin->getLatencySamples());

    return total;
}

int PluginChain::getNumSlots() const
{
    const juce::ScopedLock sl (lock);
    return (int) slots.size();
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
    {
        const juce::ScopedLock sl (lock);

        if (insertAt < 0 || insertAt > (int) slots.size())
            insertAt = (int) slots.size();

        slots.insert (slots.begin() + insertAt, std::move (slot));
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
    prepareSlot (*slot);
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
    {
        const juce::ScopedLock sl (lock);

        if (index < 0 || index >= (int) slots.size())
            return;

        slots[(size_t) index]->bypassed.store (shouldBypass);
        slots[(size_t) index]->state.bypassed = shouldBypass;
    }

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

        if (slot.bypassed.load() != s.bypassed)
            return false;

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

std::vector<PluginSlotState> PluginChain::getStates() const
{
    std::vector<PluginSlotState> result;

    for (auto& slot : slots)   // only the message thread edits 'slots', so no lock is needed here
    {
        PluginSlotState s = slot->state;
        s.bypassed = slot->bypassed.load();

        if (slot->plugin != nullptr)
        {
            const auto description = slot->plugin->getPluginDescription();
            s.format = description.pluginFormatName;
            s.name = description.name;
            s.fileOrIdentifier = description.fileOrIdentifier;
            s.uniqueId = description.uniqueId;

            if (const auto xml = description.createXml())
                s.descriptionXml = xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());

            juce::MemoryBlock block;
            bool captured = true;

            {
                const juce::ScopedLock callbackLock (slot->plugin->getCallbackLock());   // not concurrently with processBlock()

                try
                {
                    slot->plugin->getStateInformation (block);
                }
                catch (...)
                {
                    captured = false;   // the state it was loaded with stays in the file
                }
            }

            if (captured)
                s.stateBase64 = block.getSize() > 0 ? juce::Base64::toBase64 (block.getData(), block.getSize()) : juce::String();
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

    for (const auto& state : states)
    {
        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> instance;

        if (factory)
            instance = factory (state, error);

        auto slot = std::make_unique<Slot>();
        slot->state = state;
        slot->bypassed.store (state.bypassed);

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
            prepareSlot (*slot);
            slot->plugin->addListener (this);   // after restore + prepare: only real user edits count as changes
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
    }

    for (auto& slot : old)
        destroySlot (std::move (slot));

    notifyChanged();
    return errors;
}

double PluginChain::getTailSeconds() const
{
    const juce::ScopedLock sl (lock);
    double tail = 0.0;

    for (auto& slot : slots)
    {
        if (slot->plugin == nullptr || slot->bypassed.load() || slot->faulted.load (std::memory_order_relaxed))
            continue;

        const double t = slot->plugin->getTailLengthSeconds();

        if (! std::isfinite (t))
            return maxTailSeconds;

        // Plugins run in series: a reverb tail feeding a delay rings for the sum of both.
        tail = juce::jmin (maxTailSeconds, tail + juce::jmax (0.0, t));
    }

    return juce::jlimit (0.0, maxTailSeconds, tail);
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
    const juce::ScopedLock sl (lock);

    for (auto& slot : slots)
    {
        if (slot->plugin == nullptr)
            continue;

        if (slot->faulted.load (std::memory_order_relaxed))
            continue;   // dry pass: it threw once, it is not trusted with the audio again

        auto& plugin = *slot->plugin;
        const bool bypassed = slot->bypassed.load (std::memory_order_relaxed);
        const int ins = plugin.getTotalNumInputChannels();
        const int outs = plugin.getTotalNumOutputChannels();
        auto& scratch = slot->scratch;

        if (scratch.getNumSamples() < numSamples)
            scratch.setSize (slot->numScratchChannels, numSamples, false, false, true);

        midi.clear();

        // The plugin's callback lock is held while it runs (as juce::AudioProcessorPlayer does) - but never waited
        // for: the message thread holds it while it captures state for a save, and a slow plugin there must not stall
        // every channel. Busy, or suspended (loading a preset): a dry pass for this block.
        const juce::ScopedTryLock callbackLock (plugin.getCallbackLock());

        if (! callbackLock.isLocked() || plugin.isSuspended())
            continue;

        if (bypassed || slot->numScratchChannels != 2 || ins > 2 || outs > 2)
        {
            scratch.clear (0, numSamples);

            for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                scratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);

            juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), slot->numScratchChannels, 0, numSamples);

            try
            {
                plugin.processBlock (view, midi);
            }
            catch (...)
            {
                markFaulted (*slot);   // the show goes on without this plugin
                continue;
            }

            if (bypassed)
                continue;   // the plugin kept time (delay lines, reverb tails stay current); output discarded

            if (! isFinite (view, numSamples))
            {
                markFaulted (*slot);   // NaN / Inf would poison the buses: the dry input stays in 'buffer'
                continue;
            }

            for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                buffer.copyFrom (ch, 0, scratch, ch, 0, numSamples);
        }
        else
        {
            // the dry input is kept in scratch: a plugin that throws half-way must not leave its partial block behind
            for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                scratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);

            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, 0, numSamples);

            try
            {
                plugin.processBlock (view, midi);
            }
            catch (...)
            {
                markFaulted (*slot);

                for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                    buffer.copyFrom (ch, 0, scratch, ch, 0, numSamples);   // the input passes through untouched

                continue;
            }

            if (! isFinite (view, numSamples))
            {
                markFaulted (*slot);   // NaN / Inf: the dry input goes on instead

                for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                    buffer.copyFrom (ch, 0, scratch, ch, 0, numSamples);

                continue;
            }
        }

        if (outs == 1)
            buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);   // mono-out plugin: mirror to the right channel
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
            slot->faulted.store (true, std::memory_order_relaxed);
        }

        slot->scratch.clear();
    }
}

void PluginChain::notifyChanged()
{
    if (listener != nullptr)
        listener->chainChanged (*this);
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
