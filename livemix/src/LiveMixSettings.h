#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace gocue::livemix
{

/** Per-user settings (%APPDATA%\LiveMix\LiveMix.settings): device state, plugin list, last session, window, tray,
    backup target. */
class LiveMixSettings
{
public:
    LiveMixSettings();

    std::unique_ptr<juce::XmlElement> getAudioDeviceState() const;
    void setAudioDeviceState (const juce::XmlElement* xml);
    std::unique_ptr<juce::XmlElement> getPluginList() const;
    void setPluginList (const juce::XmlElement* xml);

    juce::File getLastSessionFile() const;
    void setLastSessionFile (const juce::File& file);
    juce::StringArray getRecentSessions() const;
    void addRecentSession (const juce::File& file);

    juce::String getWindowState() const;
    void setWindowState (const juce::String& state);

    bool getMinimiseToTray() const;
    void setMinimiseToTray (bool on);
    bool getCloseToTray() const;
    void setCloseToTray (bool on);
    bool getStartWithWindows() const;
    void setStartWithWindows (bool on);
    /** A mic that is OFF skips its plugins (see MixEngine::setSkipChainWhenOff). */
    bool getSkipPluginsWhenOff() const;
    void setSkipPluginsWhenOff (bool on);

    juce::String getBackupUrl() const;       // WebDAV base, e.g. https://parkdoomin.synology.me:5006
    void setBackupUrl (const juce::String& url);
    juce::String getBackupFolder() const;    // e.g. /LiveMix 백업
    void setBackupFolder (const juce::String& folder);
    juce::String getBackupUser() const;
    void setBackupUser (const juce::String& user);
    juce::String getBackupCreator() const;   // the last creator name typed
    void setBackupCreator (const juce::String& name);
    bool getBackupRememberPassword() const;
    void setBackupRememberPassword (bool on);
    /** The password lives in the Windows credential store, never in the settings file. */
    juce::String getBackupPassword() const;
    void setBackupPassword (const juce::String& password);

    juce::String getLastRunVersion() const;
    void setLastRunVersion (const juce::String& version);

    void saveIfNeeded();

private:
    juce::ApplicationProperties properties;
    juce::PropertiesFile* settings = nullptr;
};

} // namespace gocue::livemix
