#include "CameraSettingsPanel.h"
#include <chrono>

namespace gocue::recorder
{
CameraSettingsPanel::CameraSettingsPanel(const UserSettings& s, const RecorderProject& p) : initial(s)
{
    for (unsigned i = 0; i < 2; ++i)
    {
        enabled[i].setButtonText(ko(i ? "캠2 사용" : "캠1 사용")); enabled[i].setToggleState(s.cameraEnabled[i], juce::dontSendNotification);
        addAndMakeVisible(enabled[i]); addAndMakeVisible(devices[i]); addAndMakeVisible(modes[i]); addAndMakeVisible(modeLabels[i]); addAndMakeVisible(ids[i]);
        modeLabels[i].setText(ko("입력 모드"), juce::dontSendNotification); devices[i].setTextWhenNothingSelected(ko("장치 선택")); modes[i].setTextWhenNothingSelected(ko("1080p 입력 모드 선택"));
        devices[i].onChange = [this, i] { modesFor(i); }; enabled[i].onClick = [this, i] { devices[i].setEnabled(enabled[i].getToggleState()); modes[i].setEnabled(enabled[i].getToggleState()); };
    }
    for (auto* l : {&fps, &status, &calibration}) { addAndMakeVisible(l); l->setFont(juce::Font(juce::FontOptions(17))); }
    fps.setText(ko("프로젝트 ") + juce::String(p.fps.numerator) + (p.media->assets.empty() ? ko(" fps · 첫 미디어 후 고정") : ko(" fps · 고정")), juce::dontSendNotification);
    status.setText(ko("카메라 장치를 확인하는 중입니다."), juce::dontSendNotification);
    const bool changed = s.calibration.asioDeviceId != s.asioDeviceId || s.calibration.cameraDeviceIds != s.cameraDeviceIds || s.calibration.cameraModes != s.cameraModes;
    calibration.setText(ko("동기 보정 · ") + (s.calibration.calibrationDate.isEmpty() ? ko("보정 결과 없음") : changed ? ko("설정 변경으로 재측정 필요") : ko("측정됨")), juce::dontSendNotification);
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
            modesFor(i); devices[i].setEnabled(enabled[i].getToggleState()); modes[i].setEnabled(enabled[i].getToggleState());
        }
        status.setText(cameras.empty() ? ko("연결된 카메라가 없습니다.") : ko("같은 카메라를 두 번 선택할 수 없습니다."), juce::dontSendNotification);
    }
    catch (const std::exception& e) { status.setText(ko("카메라 목록을 읽을 수 없습니다. ") + juce::String::fromUTF8(e.what()), juce::dontSendNotification); }
    stopTimer();
}
void CameraSettingsPanel::modesFor(unsigned i)
{
    modes[i].clear(juce::dontSendNotification); const int n = devices[i].getSelectedId() - 1;
    if (n < 0 || n >= int(cameras.size())) return;
    const auto& c = cameras[std::size_t(n)]; ids[i].setText(juce::String(c.symbolicLink), juce::dontSendNotification); ids[i].setTooltip(juce::String(c.symbolicLink));
    for (unsigned m = 0; m < c.modes.size(); ++m) if (c.modes[m].width == 1920 && c.modes[m].height == 1080)
    { const auto text = juce::String(c.modes[m].text()); modes[i].addItem(text, int(m) + 1); if (text == initial.cameraModes[i]) modes[i].setSelectedId(int(m) + 1, juce::dontSendNotification); }
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
    auto a = getLocalBounds().reduced(16); fps.setBounds(a.removeFromTop(36)); a.removeFromTop(12);
    for (unsigned i = 0; i < 2; ++i)
    {
        enabled[i].setBounds(a.removeFromTop(34)); devices[i].setBounds(a.removeFromTop(38)); ids[i].setBounds(a.removeFromTop(28));
        auto row = a.removeFromTop(38); modeLabels[i].setBounds(row.removeFromLeft(104)); modes[i].setBounds(row); a.removeFromTop(22);
    }
    status.setBounds(a.removeFromTop(42)); calibration.setBounds(a.removeFromTop(42));
}
}
