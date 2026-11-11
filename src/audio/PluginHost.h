#pragma once

#include "audio/PluginChain.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>

namespace gocue
{

/** Plugin scanning / instantiation: the format manager (VST3 always; VST2 when the build has the SDK and the
    owner switched it on), the known-plugin list, the set of plugins the operator switched off, and the factory used
    to bring saved chains back. Message thread only. */
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
    /** LiveMix only: prefer the copies shipped beside its executable. Unset keeps the original host behaviour. */
    void setBundledPluginsDirectory (const juce::File& directory) { bundledPluginsDirectory = directory; }
    bool isBundledPluginFile (const juce::String& file) const;
    /** A format by JUCE's name ("VST3", "VST"), or null. */
    juce::AudioPluginFormat* getFormat (const juce::String& name) const;

    /** The build can host VST2 plugins (the SDK headers were there). */
    static bool hasVst2Support() noexcept;
    /** VST2 plugins are offered and instantiated only while this is on (the operator's switch; off by default).
        The VST2 format is registered on the first switch-on: an app that never switches it on never scans VST2.
        A build without the SDK stays off whatever it is asked. */
    void setVst2Enabled (bool enabled);
    bool isVst2Enabled() const noexcept { return vst2Enabled; }

    /** The key a plugin is remembered by in the disabled set: format, id and name (not the file: a plugin may move). */
    static juce::String keyFor (const juce::PluginDescription& description);
    /** Plugins the operator switched off stay out of getEffectTypes(). */
    void setDisabledPlugins (const juce::StringArray& keys);
    const juce::StringArray& getDisabledPlugins() const noexcept { return disabledPlugins; }
    void setPluginEnabled (const juce::PluginDescription& description, bool enabled);
    /** In the disabled set (the operator's own choice, whatever its format's switch says). */
    bool isPluginSwitchedOff (const juce::PluginDescription& description) const;
    /** Not switched off, and of a format that is on. */
    bool isPluginEnabled (const juce::PluginDescription& description) const;

    /** The effect plugins the menus offer: known, enabled, of a format that is on (instruments excluded), by name. */
    juce::Array<juce::PluginDescription> getEffectTypes() const;
    /** Every known effect plugin, switched off or not, of any format the build knows (for the manager), by name. */
    juce::Array<juce::PluginDescription> getAllEffectTypes() const;

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
    juce::StringArray getBundledPluginKeys() const;
    bool isShadowedByBundledPlugin (const juce::PluginDescription&, const juce::StringArray& bundledKeys) const;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::File bundledPluginsDirectory;
    bool vst2Enabled = false;
    juce::StringArray disabledPlugins;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace gocue
