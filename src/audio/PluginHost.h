#pragma once

#include "audio/PluginChain.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>

namespace gocue
{

/** VST3 scanning / instantiation: the format manager, the known-plugin list and the
    factory used to bring saved chains back. Message thread only. */
class PluginHost : private juce::ChangeListener
{
public:
    PluginHost();
    ~PluginHost() override;

    /** Safe mode (Shift at launch / --safe-mode): no plugin is instantiated; slots stay empty with an error. */
    static void setSafeMode (bool enabled) noexcept { safeMode = enabled; }
    static bool isSafeMode() noexcept { return safeMode; }

    juce::AudioPluginFormatManager& getFormatManager() noexcept { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() noexcept { return knownPlugins; }
    juce::AudioPluginFormat* getVST3Format() const;

    /** Known effect plugins (instruments excluded), sorted by name. */
    juce::Array<juce::PluginDescription> getEffectTypes() const;

    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription& description,
                                                               double sampleRate, int blockSize, juce::String& error);

    /** Re-creates a saved slot: exact description XML when present, otherwise file + id.
        The known-plugin list wins when it has a matching entry (plugins may have moved). */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const PluginSlotState& state,
                                                               double sampleRate, int blockSize, juce::String& error);

    PluginChain::Factory makeFactory (double sampleRate, int blockSize);

    void loadKnownPluginsFromXml (const juce::XmlElement* xml);
    std::unique_ptr<juce::XmlElement> createKnownPluginsXml() const;

    /** Fires (message thread) whenever the known-plugin list changes, e.g. after a scan. */
    std::function<void()> onKnownPluginsChanged;

private:
    static inline bool safeMode = false;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace gocue
