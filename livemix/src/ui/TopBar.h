#pragma once

#include "MixDocument.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::livemix
{

/** The bar under the menu bar: logo, session name and state, audio device, rate / buffer / latency, CPU load, the
    mute badges, and two buttons (FX 채널, 플러그인 관리). The session, backup, settings and help live in the menu bar. */
class TopBar : public juce::Component
{
public:
    explicit TopBar (MixDocument& document);

    /** One row in a wide window; two below 1000 px (the device row folds under); three below 700 px (the buttons
        take a row of their own) - the portrait mode of a tall, narrow window. */
    enum class Mode { wide, compact, narrow };
    Mode modeFor (int width) const noexcept;        // the single row needs more room while a mute badge shows
    int preferredHeight (int width) const noexcept;
    /** The bar's height changed with its content (a badge came or went): the owner lays out again. */
    std::function<void()> onHeightChanged;

    void refresh();                                   // session name / dirty flag / device
    void setDevices (const juce::StringArray& deviceNames, const juce::String& current, const juce::String& typeName);
    void setStatus (double sampleRate, int bufferSize, double latencyMs, double dspLoad, bool running);
    void setFxCount (int count);
    /** The mute groups' state: a red badge each while one is muted. */
    void setMuteGroups (bool micMuted, bool fxMuted);

    std::function<void (const juce::String& deviceName)> onDeviceChosen;
    std::function<void()> onFxPanel;
    std::function<void()> onPluginManager;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct DspMeter : public juce::Component
    {
        void paint (juce::Graphics& g) override;
        double load = 0.0;
    };

    MixDocument& document;
    juce::Label logoMark, logoText, sessionName, sessionState, deviceLabel, statusLabel, dspLabel, micMuteBadge, fxMuteBadge;
    juce::ComboBox deviceCombo;
    DspMeter dspMeter;
    juce::TextButton fxButton, pluginsButton;
    bool refreshing = false;
};

} // namespace gocue::livemix
