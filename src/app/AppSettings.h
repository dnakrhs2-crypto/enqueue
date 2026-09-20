#pragma once
#include "app/MidiShortcutProfile.h"
#include "app/ReopenLastProject.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <memory>
#include <optional>

namespace gocue
{

/** Per-user application settings (%APPDATA%\Enqueue\Enqueue.settings):
    audio device state, plugin list, last used paths and the window state. */
class AppSettings
{
public:
    AppSettings();
    /** Uses an externally owned file (e.g. an isolated test file), without touching AppData or migration. */
    explicit AppSettings (juce::PropertiesFile& storage);

    std::unique_ptr<juce::XmlElement> getAudioDeviceState() const;
    void setAudioDeviceState (const juce::XmlElement* xml);

    std::unique_ptr<juce::XmlElement> getPluginList() const;
    void setPluginList (const juce::XmlElement* xml);

    juce::File getLastProjectFile() const;
    void setLastProjectFile (const juce::File& file);

    /** This PC's last session, independent of the file chooser path; empty means an unsaved new project.
        Saved immediately on project changes so it survives a crash before shutdown.
        Failure leaves the new path pending for the next save attempt. */
    juce::File getLastSessionProject() const;
    bool setLastSessionProject (const juce::File& file);
    ReopenLastProjectPolicy getReopenLastProjectPolicy() const;
    /** Saves immediately; failure restores the raw policy and previous dirty state. */
    bool setReopenLastProjectPolicy (ReopenLastProjectPolicy policy);

    /** The project that was open when an update closed the app: opened again once, right after the update. */
    juce::File getReopenProjectAfterUpdate() const;
    void setReopenProjectAfterUpdate (const juce::File& file);

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

    /** This PC's next-cue loudness average window (5, 10, 20, 30 or 60 seconds). */
    int getLufsAverageSeconds() const;
    void setLufsAverageSeconds (int seconds);

    /** 설정 > 글씨·화면 크기: the scale of the whole UI on this PC, in percent (one of
        UiScale::allowedPercents; anything else reads as 100). Per PC on purpose, never in the project file. */
    int getUiScalePercent() const;
    void setUiScalePercent (int percent);

    /** 유튜브 다운로드: put the downloaded file into the cue list right away (on by default). */
    bool getYouTubeAddToQueue() const;
    void setYouTubeAddToQueue (bool add);

    /** 플러그인 관리: the plugins whose "사용" switch is off (PluginHost::keyFor keys), out of every '+ 추가' menu. */
    juce::StringArray getDisabledPlugins() const;
    void setDisabledPlugins (const juce::StringArray& keys);

    /** Returns the exact XML (missing and present-but-empty are distinct). On disk both values
        use enqueue-shortcuts-base64-v1: plus Base64 UTF-8, preventing PropertiesFile XML parsing.
        Legacy raw XML is read compatibly and wrapped on construction before any later save;
        invalid encodings are returned unchanged so profile validation rejects them. */
    std::optional<juce::String> getKeyboardShortcutsXml() const;
    std::optional<juce::String> getKeyboardShortcutsLastGoodXml() const;
    /** Immediately saves the pair. Failure restores both in-memory properties and their dirty state,
        so the normal delayed save/exit flush cannot commit the rejected candidate later. */
    bool saveKeyboardShortcuts (const juce::String& currentXml, const juce::String& lastGoodXml);
    std::optional<juce::String> getMidiShortcutsXml() const;
    std::optional<juce::String> getMidiShortcutsLastGoodXml() const;
    std::optional<juce::String> getMidiInputSettingsXml() const;
    std::optional<juce::String> getMidiInputSettingsLastGoodXml() const;
    bool saveInputSettings (const InputSettingsTransaction&);

    /** Writes pending changes to disk now and reports PropertiesFile's result. */
    bool saveNow();
    /** Successful saves include the pending session path, regardless of which setting triggered them. */
    std::function<void()> onSaveSucceeded;
    /** Existing shutdown/automatic-save paths remain available. */
    void flush();

    /** The underlying file, for JUCE components that persist their own settings (plugin scanner). */
    juce::PropertiesFile* getPropertiesFile() noexcept { return settings; }
    /** File the plugin scanner uses to blacklist plugins that crashed a previous scan. */
    juce::File getDeadMansPedalFile() const;

private:
    void protectShortcutXml();
    juce::File getFileValue (const char* key) const;
    void setFileValue (const char* key, const juce::File& file);

    juce::ApplicationProperties properties;
    juce::PropertiesFile* settings = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppSettings)
};

} // namespace gocue
