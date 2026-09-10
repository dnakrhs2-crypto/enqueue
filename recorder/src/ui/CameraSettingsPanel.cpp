#include "CameraSettingsPanel.h"
#include <chrono>

namespace gocue::recorder
{
CameraSettingsPanel::CameraSettingsPanel(const UserSettings& s, const RecorderProject& p) : initial(s), projectFps(p.fps.numerator)
{
    for (unsigned i = 0; i < 2; ++i)
    {
        enabled[i].setButtonText(ko(i ? "캠2 사용" : "캠1 사용")); enabled[i].setToggleState(s.cameraEnabled[i], juce::dontSendNotification);
        addAndMakeVisible(enabled[i]); addAndMakeVisible(devices[i]); addAndMakeVisible(modes[i]); addAndMakeVisible(modeLabels[i]); addAndMakeVisible(ids[i]);
        modeLabels[i].setText(ko("입력 모드"), juce::dontSendNotification); devices[i].setTextWhenNothingSelected(ko("장치 선택")); modes[i].setTextWhenNothingSelected(ko("1080p 입력 모드 선택"));
        devices[i].onChange = [this, i] { selectionChanged(i, false); }; enabled[i].onClick = [this, i] { selectionChanged(i, true); };
    }
    for (auto* l : {&fps, &status, &calibration, &nextTake}) { addAndMakeVisible(l); l->setFont(juce::Font(juce::FontOptions(17))); }
    nextTake.setText(ko("캠2 해제는 다음 테이크부터 적용됩니다."), juce::dontSendNotification);
    fps.setText(ko("프로젝트 ") + juce::String(p.fps.numerator) + (p.media->assets.empty() ? ko(" fps · 첫 미디어 후 고정") : ko(" fps · 고정")), juce::dontSendNotification);
    status.setText(ko("카메라 장치를 확인하는 중입니다."), juce::dontSendNotification);
    refreshCalibration();
    work = std::async(std::launch::async, [] { ComApartment apartment; MfRuntime runtime; return CameraCatalog::enumerate(); }); startTimer(100);
}
CameraSettingsPanel::~CameraSettingsPanel() { stopTimer(); }
void CameraSettingsPanel::timerCallback()
{
    if (!work.valid() || work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    try
    {
        cameras = work.get();
        for (unsigned i = 0; i < 2; ++i)
        {
            for (int n = 0; n < int(cameras.size()); ++n) { devices[i].addItem(juce::String::fromUTF8(cameras[std::size_t(n)].friendlyName.c_str()), n + 1); if (cameras[std::size_t(n)].symbolicLink == initial.cameraDeviceIds[i].toStdString()) devices[i].setSelectedId(n + 1, juce::dontSendNotification); }
            if (!devices[i].getSelectedId() && initial.cameraDeviceIds[i].isNotEmpty()) devices[i].setText(ko("저장된 카메라 · 연결 안 됨"), juce::dontSendNotification);
            acceptedDevices[i] = devices[i].getSelectedId();
            modesFor(i); devices[i].setEnabled(enabled[i].getToggleState()); modes[i].setEnabled(enabled[i].getToggleState());
        }
        status.setText(cameras.empty() ? ko("연결된 카메라가 없습니다.") : replacedMode ? ko("입력 모드를 프로젝트 fps에 맞춰 다시 골랐습니다. 적용을 누르면 저장됩니다.") : ko("같은 카메라를 두 번 선택할 수 없습니다."), juce::dontSendNotification);
        refreshCalibration();
    }
    catch (const std::exception& e) { status.setText(ko("카메라 목록을 읽을 수 없습니다. ") + juce::String::fromUTF8(e.what()), juce::dontSendNotification); }
    stopTimer();
}
void CameraSettingsPanel::modesFor(unsigned i)
{
    modes[i].clear(juce::dontSendNotification); const int n = devices[i].getSelectedId() - 1;
    if (n < 0 || n >= int(cameras.size()))
    { ids[i].setText(initial.cameraDeviceIds[i], juce::dontSendNotification); modes[i].setText(initial.cameraModes[i], juce::dontSendNotification); return; }
    const auto& c = cameras[std::size_t(n)]; ids[i].setText(juce::String(c.symbolicLink), juce::dontSendNotification); ids[i].setTooltip(juce::String(c.symbolicLink));
    // Friendly labels; the stored form stays mode.text(). A saved choice below the project fps (a 5 fps leftover) is replaced by the sensible default.
    for (unsigned m = 0; m < c.modes.size(); ++m) if (c.modes[m].width == 1920 && c.modes[m].height == 1080)
    {
        modes[i].addItem(friendlyModeText(c.modes[m]), int(m) + 1);
        if (juce::String(c.modes[m].text()) == initial.cameraModes[i] && reachesProjectFps(c.modes[m], projectFps)) modes[i].setSelectedId(int(m) + 1, juce::dontSendNotification);
    }
    if (const auto best = preferred1080pMode(c.modes, projectFps); !modes[i].getSelectedId() && best >= 0)
    {
        modes[i].setSelectedId(best + 1, juce::dontSendNotification);
        if (initial.cameraModes[i].isNotEmpty() && juce::String(c.modes[std::size_t(best)].text()) != initial.cameraModes[i]) replacedMode = true;
    }
}
void CameraSettingsPanel::selectionChanged(unsigned i, bool enabling)
{
    const auto selection = read(initial); CameraCatalog model;
    const auto valid = model.configure(selection);
    if (valid.failed())
    {
        if (enabling) enabled[i].setToggleState(false, juce::dontSendNotification);
        else devices[i].setSelectedId(acceptedDevices[i], juce::dontSendNotification);
        status.setText(valid.getErrorMessage(), juce::dontSendNotification);
    }
    else
    {
        acceptedDevices[i] = devices[i].getSelectedId();
        if (!enabling) modesFor(i);
        status.setText(ko("선택한 장치와 입력 모드를 저장합니다."), juce::dontSendNotification);
    }
    devices[i].setEnabled(enabled[i].getToggleState()); modes[i].setEnabled(enabled[i].getToggleState());
    refreshCalibration();
}
void CameraSettingsPanel::refreshCalibration()
{
    // Computed from the current selection (auto-picked modes included), not from the settings the panel opened with.
    const auto s = read(initial);
    const bool changed = s.calibration.asioDeviceId != s.asioDeviceId || s.calibration.cameraDeviceIds != s.cameraDeviceIds || s.calibration.cameraModes != s.cameraModes;
    calibration.setText(ko("동기 보정 · ") + (s.calibration.calibrationDate.isEmpty() ? ko("보정 결과 없음") : changed ? ko("설정 변경으로 재측정 필요") : ko("측정됨")), juce::dontSendNotification);
}
UserSettings CameraSettingsPanel::read(UserSettings s) const
{
    for (unsigned i = 0; i < 2; ++i)
    {
        s.cameraEnabled[i] = enabled[i].getToggleState(); const auto n = devices[i].getSelectedId() - 1, m = modes[i].getSelectedId() - 1;
        if (n >= 0 && n < int(cameras.size())) { s.cameraDeviceIds[i] = cameras[std::size_t(n)].symbolicLink; s.cameraModes[i] = m >= 0 && m < int(cameras[std::size_t(n)].modes.size()) ? juce::String(cameras[std::size_t(n)].modes[std::size_t(m)].text()) : juce::String(); }
    }
    return s;
}
void CameraSettingsPanel::resized()
{
    auto a = getLocalBounds().reduced(12); fps.setBounds(a.removeFromTop(28)); a.removeFromTop(4);
    for (unsigned i = 0; i < 2; ++i)
    {
        enabled[i].setBounds(a.removeFromTop(28)); devices[i].setBounds(a.removeFromTop(32)); ids[i].setBounds(a.removeFromTop(22));
        auto row = a.removeFromTop(32); modeLabels[i].setBounds(row.removeFromLeft(104)); modes[i].setBounds(row); a.removeFromTop(8);
    }
    status.setBounds(a.removeFromTop(36)); calibration.setBounds(a.removeFromTop(28)); nextTake.setBounds(a.removeFromTop(30));
}
}
