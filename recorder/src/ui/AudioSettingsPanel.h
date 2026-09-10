#pragma once
#include "RecorderLookAndFeel.h"
#include "UiState.h"

namespace gocue::recorder
{
class RecorderSession;
class AudioSettingsPanel : public juce::Component
{
public:
    AudioSettingsPanel(const UserSettings&, const RecorderProject&, const RecorderAudioEngine::DeviceInfo&);
    UserSettings read(UserSettings base) const;
    void setDeviceInfo(const RecorderAudioEngine::DeviceInfo&);
    void setSettings(const UserSettings&); // sync the visible choices with what the session applied
    juce::Result configure(RecorderSession&, UserSettings, const UserSettings& applied); // immediate or queued edit; restore on synchronous rejection
    void setBusy(bool configuring);       // only the ASIO control panel is held back while a change is applying
    void resized() override;
    std::function<void(UserSettings)> onChanged; // any edit applies immediately (no connect button)
    std::function<void()> onControlPanel;
private:
    UserSettings initial;
    bool fixed;
    unsigned projectFs;
    RecorderAudioEngine::DeviceInfo actual;
    bool busy = false;
    void selectRate(unsigned fs);
    juce::String inputName(int physicalInput) const;
    juce::String inputConflict(unsigned slot, int leftInput, bool stereo, const UserSettings&) const;
    void refreshInputChoices();
    juce::Label deviceLabel, rateLabel, actualLabel, bufferLabel, latencyLabel, leftLabel, rightLabel;
    juce::ComboBox devices, rate, buffer, left, right;
    juce::ToggleButton mono {ko("모노 출력")};
    juce::TextButton controlPanel {ko("ASIO 제어판")};
    std::array<juce::Label, 8> inputLabels;
    std::array<juce::Label, 8> inputHints;
    std::array<juce::ComboBox, 8> inputs;
    std::array<juce::TextButton, 8> inputMono, inputStereo;
    std::array<juce::TextButton, 2> inputPages;
    unsigned inputPage = 0;
};
}
