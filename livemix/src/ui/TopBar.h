#pragma once

#include "MixDocument.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::livemix
{

/** The bar under the title: logo, session name and state, ASIO device, rate / buffer / latency, CPU load,
    and the buttons (세션, FX 채널, 온라인 백업, 설정, 도움말). */
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
    void setDevices (const juce::StringArray& asioDeviceNames, const juce::String& current);
    void setStatus (double sampleRate, int bufferSize, double latencyMs, double dspLoad, bool running);
    void setFxCount (int count);
    /** The mute groups' state: a red badge each while one is muted. */
    void setMuteGroups (bool micMuted, bool fxMuted);

    std::function<void (const juce::String& deviceName)> onDeviceChosen;
    std::function<void (juce::Component* anchor)> onSessionMenu;
    std::function<void()> onFxPanel;
    std::function<void()> onBackup;
    std::function<void()> onSettings;
    std::function<void (juce::Component* anchor)> onHelpMenu;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct DspMeter : public juce::Component
    {
        void paint (juce::Graphics& g) override;
        double load = 0.0;
    };

    MixDocument& document;
    juce::Label logoMark, logoText, sessionName, sessionState, asioLabel, statusLabel, dspLabel, micMuteBadge, fxMuteBadge;
    juce::ComboBox deviceCombo;
    DspMeter dspMeter;
    juce::TextButton sessionButton, fxButton, backupButton, settingsButton, helpButton;
    bool refreshing = false;
};

} // namespace gocue::livemix
