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

    if (dead->plugin != nullptr)
    {
        if (listener != nullptr)
            listener->pluginAboutToBeRemoved (*this, *dead->plugin);

        dead->plugin->releaseResources();
    }

    dead.reset();
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
    {
        if (slot->plugin != nullptr)
        {
            if (listener != nullptr)
                listener->pluginAboutToBeRemoved (*this, *slot->plugin);

            slot->plugin->releaseResources();
        }
    }

    dead.clear();
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
            slot->plugin->getStateInformation (block);
            s.stateBase64 = block.getSize() > 0 ? juce::Base64::toBase64 (block.getData(), block.getSize()) : juce::String();
        }

        result.push_back (std::move (s));
    }

    return result;
}

juce::StringArray PluginChain::restore (const std::vector<PluginSlotState>& states, const Factory& factory)
{
    juce::StringArray errors;
    clear();

    for (const auto& state : states)
    {
        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> instance;

        if (factory)
            instance = factory (state, error);

        if (instance != nullptr)
        {
            addPlugin (std::move (instance), state);
        }
        else
        {
            addMissingSlot (state);
            errors.add (state.name + ": " + (error.isNotEmpty() ? error : juce::String ("plugin not available")));
        }
    }

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

        tail = juce::jmax (tail, t);
    }

    return juce::jlimit (0.0, maxTailSeconds, tail);
}

void PluginChain::process (juce::AudioBuffer<float>& buffer, int numSamples)
{
    const juce::ScopedLock sl (lock);

    for (auto& slot : slots)
    {
        if (slot->plugin == nullptr || slot->bypassed.load (std::memory_order_relaxed))
            continue;

        auto& plugin = *slot->plugin;
        const int ins = plugin.getTotalNumInputChannels();
        const int outs = plugin.getTotalNumOutputChannels();
        midi.clear();

        if (slot->numScratchChannels == 2 && ins <= 2 && outs <= 2)
        {
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, 0, numSamples);
            plugin.processBlock (view, midi);
        }
        else
        {
            auto& scratch = slot->scratch;

            if (scratch.getNumSamples() < numSamples)
                scratch.setSize (slot->numScratchChannels, numSamples, false, false, true);

            scratch.clear (0, numSamples);

            for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                scratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);

            juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), slot->numScratchChannels, 0, numSamples);
            plugin.processBlock (view, midi);

            for (int ch = 0; ch < 2 && ch < scratch.getNumChannels(); ++ch)
                buffer.copyFrom (ch, 0, scratch, ch, 0, numSamples);
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

} // namespace gocue
