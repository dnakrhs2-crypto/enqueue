#pragma once
#include "RecorderLookAndFeel.h"
#include "UiState.h"

namespace gocue::recorder
{
class AudioSettingsPanel : public juce::Component
{
public:
    AudioSettingsPanel(const UserSettings&, const RecorderProject&, const RecorderAudioEngine::DeviceInfo&);
    UserSettings read(UserSettings base) const;
    void setDeviceInfo(const RecorderAudioEngine::DeviceInfo&);
    void setSettings(const UserSettings& s) { initial = s; }
    void resized() override;
    std::function<void(UserSettings)> onConnect;
    std::function<void()> onControlPanel;
private:
    UserSettings initial;
    bool fixed;
    unsigned projectFs;
    RecorderAudioEngine::DeviceInfo actual;
    juce::Label deviceLabel, rateLabel, actualLabel, bufferLabel, latencyLabel, leftLabel, rightLabel;
    juce::ComboBox devices, rate, buffer, left, right;
    juce::ToggleButton mono {ko("모노 출력")};
    juce::TextButton connect {ko("장치 연결")}, controlPanel {ko("ASIO 제어판")};
    std::array<juce::Label, 8> inputLabels;
    std::array<juce::ComboBox, 8> inputs;
};
}
