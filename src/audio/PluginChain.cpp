#include "audio/PluginChain.h"

#include <cmath>

namespace gocue
{

PluginChain::~PluginChain()
{
    clear();
}

void PluginChain::prepare (double newSampleRate, int newBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    blockSize = juce::jmax (1, newBlockSize);

    const juce::ScopedLock sl (lock);

    for (auto& slot : slots)
        if (slot->plugin != nullptr)
            prepareSlot (*slot);
}

void PluginChain::prepareSlot (Slot& slot)
{
    auto& plugin = *slot.plugin;

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

    slot.numScratchChannels = juce::jmax (2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels());
    slot.scratch.setSize (slot.numScratchChannels, blockSize, false, false, true);
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

        slot->plugin->releaseResources();
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
            plugin->setStateInformation (decoded.getData(), (int) decoded.getDataSize());
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
    std::vector<std::unique_ptr<Slot>> dead;

    {
        const juce::ScopedLock sl (lock);
        dead.swap (slots);
    }

    if (dead.empty())
        return;

    for (auto& slot : dead)
        destroySlot (std::move (slot));

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
            slot->plugin->getStateInformation (block);
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
                    instance->setStateInformation (decoded.getData(), (int) decoded.getDataSize());
            }

            slot->plugin = std::move (instance);
            prepareSlot (*slot);
            slot->plugin->addListener (this);   // after restore + prepare: only real user edits count as changes
        }
        else
        {
            errors.add (state.name + ": " + (error.isNotEmpty() ? error : juce::String ("plugin not available")));
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
        if (slot->plugin == nullptr || slot->bypassed.load())
            continue;

        const double t = slot->plugin->getTailLengthSeconds();

        if (! std::isfinite (t))
            return maxTailSeconds;

        // Plugins run in series: a reverb tail feeding a delay rings for the sum of both.
        tail = juce::jmin (maxTailSeconds, tail + juce::jmax (0.0, t));
    }

    return juce::jlimit (0.0, maxTailSeconds, tail);
}

void PluginChain::process (juce::AudioBuffer<float>& buffer, int numSamples)
{
    const juce::ScopedLock sl (lock);

    for (auto& slot : slots)
    {
        if (slot->plugin == nullptr)
            continue;

        auto& plugin = *slot->plugin;
        const bool bypassed = slot->bypassed.load (std::memory_order_relaxed);
        const int ins = plugin.getTotalNumInputChannels();
        const int outs = plugin.getTotalNumOutputChannels();
        auto& scratch = slot->scratch;

        if (scratch.getNumSamples() < numSamples)
            scratch.setSize (slot->numScratchChannels, numSamples, false, false, true);

        midi.clear();

        // Same contract as juce::AudioProcessorPlayer: hold the plugin's callback lock while it runs,
        // and leave it alone (dry pass) while it has suspended itself, e.g. to load a preset.
        const juce::ScopedLock callbackLock (plugin.getCallbackLock());

        if (plugin.isSuspended())
            continue;

        if (bypassed || slot->numScratchChannels != 2 || ins > 2 || outs > 2)
        {
            scratch.clear (0, numSamples);

            for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                scratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);

            juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), slot->numScratchChannels, 0, numSamples);
            plugin.processBlock (view, midi);

            if (bypassed)
                continue;   // the plugin kept time (delay lines, reverb tails stay current); output discarded

            for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                buffer.copyFrom (ch, 0, scratch, ch, 0, numSamples);
        }
        else
        {
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, 0, numSamples);
            plugin.processBlock (view, midi);
        }

        if (outs == 1)
            buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);   // mono-out plugin: mirror to the right channel
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
