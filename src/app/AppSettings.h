#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace gocue
{

/** Per-user application settings (%APPDATA%\Enqueue\Enqueue.settings):
    audio device state, plugin list, last used paths and the window state. */
class AppSettings
{
public:
    AppSettings();

    std::unique_ptr<juce::XmlElement> getAudioDeviceState() const;
    void setAudioDeviceState (const juce::XmlElement* xml);

    std::unique_ptr<juce::XmlElement> getPluginList() const;
    void setPluginList (const juce::XmlElement* xml);

    juce::File getLastProjectFile() const;
    void setLastProjectFile (const juce::File& file);

    juce::File getLastAudioDirectory() const;
    void setLastAudioDirectory (const juce::File& directory);

    juce::String getWindowState() const;
    void setWindowState (const juce::String& state);

    /** The app version that ran last time (empty on a fresh install): a change means an update just landed. */
    juce::String getLastRunVersion() const;
    void setLastRunVersion (const juce::String& version);

    /** Layout: the inspector's share of the height below the transport, the active-cues panel's share of the
        width, and whether each is folded away. */
    double getInspectorFraction() const;
    void setInspectorFraction (double fraction);
    bool getInspectorCollapsed() const;
    void setInspectorCollapsed (bool collapsed);
    double getActiveCuesFraction() const;
    void setActiveCuesFraction (double fraction);
    bool getActiveCuesCollapsed() const;
    void setActiveCuesCollapsed (bool collapsed);

    /** Writes pending changes to disk now. */
    void flush();

    /** The underlying file, for JUCE components that persist their own settings (plugin scanner). */
    juce::PropertiesFile* getPropertiesFile() noexcept { return settings; }
    /** File the plugin scanner uses to blacklist plugins that crashed a previous scan. */
    juce::File getDeadMansPedalFile() const;

private:
    juce::File getFileValue (const char* key) const;
    void setFileValue (const char* key, const juce::File& file);

    juce::ApplicationProperties properties;
    juce::PropertiesFile* settings = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppSettings)
};

} // namespace gocue
