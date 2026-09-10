#pragma once
#include "RecorderLookAndFeel.h"
#include "UiState.h"

namespace gocue::recorder
{
class CameraSettingsPanel : public juce::Component, private juce::Timer
{
public:
    using CalibrationMatcher = std::function<CalibrationMatch(const UserSettings&)>;
    CameraSettingsPanel(const UserSettings&, const RecorderProject&, CalibrationMatcher = {});
    ~CameraSettingsPanel() override;
    UserSettings read(UserSettings base) const;
    const std::vector<CameraDevice>& catalog() const { return cameras; }
    bool scanning() const { return work.valid(); }
    void setSettings(const UserSettings&); // refresh calibration with applied audio/arm settings, preserving camera edits
    void resized() override;
private:
    void timerCallback() override;
    void modesFor(unsigned);
    void selectionChanged(unsigned, bool enabling);
    void refreshCalibration();
    UserSettings initial;
    CalibrationMatcher matchCalibration;
    std::vector<CameraDevice> cameras;
    std::future<std::vector<CameraDevice>> work;
    std::array<juce::ToggleButton, 2> enabled;
    std::array<juce::ComboBox, 2> devices, modes;
    std::array<juce::Label, 2> modeLabels, ids;
    std::array<int, 2> acceptedDevices{};
    unsigned projectFps = 30;
    bool replacedMode = false;
    juce::Label fps, status, calibration, nextTake;
};
}
