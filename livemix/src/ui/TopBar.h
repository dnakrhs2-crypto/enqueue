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

    void refresh();                                   // session name / dirty flag / device
    void setDevices (const juce::StringArray& asioDeviceNames, const juce::String& current);
    void setStatus (double sampleRate, int bufferSize, double latencyMs, double dspLoad, bool running);
    void setFxCount (int count);

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
    juce::Label logoMark, logoText, sessionName, sessionState, asioLabel, statusLabel, dspLabel;
    juce::ComboBox deviceCombo;
    DspMeter dspMeter;
    juce::TextButton sessionButton, fxButton, backupButton, settingsButton, helpButton;
    bool refreshing = false;
};

} // namespace gocue::livemix
