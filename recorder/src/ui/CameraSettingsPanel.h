#pragma once
#include "RecorderLookAndFeel.h"
#include "UiState.h"

namespace gocue::recorder
{
class CameraSettingsPanel : public juce::Component, private juce::Timer
{
public:
    CameraSettingsPanel(const UserSettings&, const RecorderProject&);
    ~CameraSettingsPanel() override;
    UserSettings read(UserSettings base) const;
    const std::vector<CameraDevice>& catalog() const { return cameras; }
    bool scanning() const { return work.valid(); }
    void resized() override;
private:
    void timerCallback() override;
    void modesFor(unsigned);
    void selectionChanged(unsigned, bool enabling);
    UserSettings initial;
    std::vector<CameraDevice> cameras;
    std::future<std::vector<CameraDevice>> work;
    std::array<juce::ToggleButton, 2> enabled;
    std::array<juce::ComboBox, 2> devices, modes;
    std::array<juce::Label, 2> modeLabels, ids;
    std::array<int, 2> acceptedDevices{};
    juce::Label fps, status, calibration, nextTake;
};
}
