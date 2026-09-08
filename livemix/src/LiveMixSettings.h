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
    /** An empty directory uses the normal per-user location; tests can use an isolated directory. */
    explicit LiveMixSettings (const juce::File& directory = {});

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
    /** Ask on the close button whether to quit or go to the tray (default true). When false, the close button uses
        getCloseToTray() silently. */
    bool getCloseAsk() const;
    void setCloseAsk (bool on);
    bool getStartWithWindows() const;
    void setStartWithWindows (bool on);
    /** A mic that is OFF skips its plugins (see MixEngine::setSkipChainWhenOff). */
    bool getSkipPluginsWhenOff() const;
    void setSkipPluginsWhenOff (bool on);

    /** Local Stream Deck / external control is opt-in. Only this preference is persisted. */
    bool getExternalControlEnabled() const;
    void setExternalControlEnabled (bool on);

    /** The global hotkeys as KeyPress descriptions ("F9", "ctrl + alt + M"); empty = none. */
    juce::String getMicMuteHotkey() const;
    void setMicMuteHotkey (const juce::String& description);
    juce::String getFxMuteHotkey() const;
    void setFxMuteHotkey (const juce::String& description);
    juce::String getWindowHotkey() const;
    void setWindowHotkey (const juce::String& description);

    /** VST2 plugins offered and used (off unless the operator switched it on). */
    bool getVst2Enabled() const;
    void setVst2Enabled (bool on);
    /** The plugins switched off in the manager (PluginHost::keyFor keys). */
    juce::StringArray getDisabledPlugins() const;
    void setDisabledPlugins (const juce::StringArray& keys);

    /** The LUFS meter's target (-14 unless chosen: YouTube / Spotify) and whether its window stays over the others. */
    double getLufsTarget() const;
    void setLufsTarget (double lufs);
    bool getLufsAlwaysOnTop() const;
    void setLufsAlwaysOnTop (bool on);

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
