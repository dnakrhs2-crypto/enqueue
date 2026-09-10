#include "RecordView.h"
#include <cmath>

namespace gocue::recorder
{
RecordView::CameraCard::CameraCard() { addChildComponent(host); }
void RecordView::CameraCard::ensureHost()
{
    if (host.getHWND() || !getPeer()) return;
    auto hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP | WS_DISABLED, 0, 0, 16, 9, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd) { host.setHWND(hwnd); host.setVisible(showVideo); }
}
void RecordView::CameraCard::paint(juce::Graphics& g)
{
    g.setColour(Palette::card); g.fillRoundedRectangle(getLocalBounds().toFloat(), 12);
    g.setColour(Palette::text); g.setFont(juce::Font(juce::FontOptions(17, juce::Font::bold)));
    g.drawFittedText(caption, getLocalBounds().reduced(10, 0).removeFromTop(34), juce::Justification::centredLeft, 1);
    g.setColour(Palette::meterBg); g.fillRect(host.getBounds());
    if (!showVideo) { g.setColour(Palette::dimText); g.setFont(juce::Font(juce::FontOptions(17))); g.drawFittedText(placeholder, host.getBounds().reduced(8), juce::Justification::centred, 2); }
}
void RecordView::CameraCard::resized()
{
    const auto a = getLocalBounds().reduced(8).withTrimmedTop(28);
    const auto w = juce::jmax(0, juce::jmin(a.getWidth(), a.getHeight() * 16 / 9)), h = w * 9 / 16;
    host.setBounds(juce::Rectangle<int>(w, h).withCentre(a.getCentre())); ensureHost();
}
RecordView::Microphone::Microphone()
{
    for (auto* l : {&name, &physical}) { addAndMakeVisible(l); l->setFont(juce::Font(juce::FontOptions(17))); }
    name.setEditable(false, true); addAndMakeVisible(arm); addAndMakeVisible(monitor);
}
void RecordView::Microphone::paint(juce::Graphics& g)
{
    g.setColour(Palette::card); g.fillRoundedRectangle(getLocalBounds().reduced(3).toFloat(), 10);
    auto meter = juce::Rectangle<float>(12, 80, float(getWidth() - 24), 10); g.setColour(Palette::meterBg); g.fillRoundedRectangle(meter, 3);
    const auto level = peak > 0 ? juce::jlimit(0.0f, 1.0f, (60 + 20 * std::log10(peak)) / 60) : 0.0f;
    g.setColour(peak >= .98f ? Palette::meterRed : peak >= .8f ? Palette::meterYellow : Palette::meterGreen); g.fillRoundedRectangle(meter.withWidth(meter.getWidth() * level), 3);
}
void RecordView::Microphone::resized()
{ name.setBounds(8, 6, getWidth() - 16, 26); physical.setBounds(8, 32, getWidth() - 16, 22); arm.setBounds(8, 54, getWidth() - 16, 24); monitor.setBounds(8, 96, getWidth() - 16, 28); }
RecordView::RecordView()
{
    for (auto* b : {&projectButton, &recordTab, &timelineTab, &normalButton, &dubButton, &settingsButton, &exportButton, &startButton, &stopButton, &markerButton, &latestButton}) { addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    for (auto* l : {&projectName, &statusLabel, &errorLabel, &noMicrophones}) { addAndMakeVisible(l); l->setFont(juce::Font(juce::FontOptions(17))); }
    projectName.setFont(juce::Font(juce::FontOptions(20, juce::Font::bold))); errorLabel.setColour(juce::Label::textColourId, Palette::danger);
    for (auto& cam : cameras) addAndMakeVisible(cam);
    setCamera(0, ko("캠1"), ko("캠1 연결 안 됨 · 설정에서 연결"), false);
    setCamera(1, ko("캠2"), ko("캠2 사용 안 함 · 설정에서 연결"), false);
    addAndMakeVisible(microphoneViewport); microphoneViewport.setViewedComponent(&strips, false); microphoneViewport.setScrollBarsShown(false, true);
    for (unsigned i = 0; i < 8; ++i)
    {
        auto& mic = microphones[i]; strips.addAndMakeVisible(mic);
        mic.arm.onClick = [this, i] { if (onArm) onArm(i, microphones[i].arm.getToggleState()); };
        mic.monitor.onClick = [this, i] { if (onMonitor) onMonitor(i, microphones[i].monitor.getToggleState()); };
        mic.name.onTextChange = [this, i] { if (onName) onName(i, microphones[i].name.getText()); };
    }
    exportButton.setEnabled(false); exportButton.setTooltip(ko("내보내기 준비 전"));
    dubButton.setEnabled(false); dubButton.setTooltip(ko("더빙 녹화 준비 전"));
    startButton.setColour(juce::TextButton::buttonColourId, Palette::brand);
}
std::array<void*, 2> RecordView::nativeHosts()
{ for (auto& cam : cameras) cam.ensureHost(); return {cameras[0].host.getHWND(), cameras[1].host.getHWND()}; }
void RecordView::setCamera(unsigned i, const juce::String& caption, const juce::String& placeholder, bool visible)
{
    auto& c = cameras.at(i);
    if (visible && placeholder != ko("영상 없음")) c.liveSeen = true;
    const auto state = i == 1 && placeholder == ko("카메라 연결 준비 전")
        ? (c.liveSeen ? ko("캠2 연결 끊김") : ko("캠2 사용 안 함 · 설정에서 연결")) : placeholder;
    if (c.caption == caption && c.placeholder == state && c.showVideo == visible) return;
    c.caption = caption; c.placeholder = state; c.showVideo = visible; c.host.setVisible(visible); c.repaint();
}
void RecordView::update(const RecorderUiState& ui, const RecorderProject& p, const UserSettings& s, const juce::String& status,
                        const juce::String& banner, Sample elapsed, juce::int64 remaining, bool isTimeline)
{
    const auto oldCount = stripCount; const auto wasTimeline = timeline; timeline = isTimeline;
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto key = s.cameraEnabled[i] ? s.cameraDeviceIds[i] + "\n" + s.cameraModes[i] : juce::String();
        if (cameraConfiguration[i] != key) { cameraConfiguration[i] = key; cameras[i].liveSeen = false; }
    }
    projectName.setText(p.name, juce::dontSendNotification); projectName.setTooltip(p.name);
    statusLabel.setText((ui.live ? ko("녹화 중   ·   ") : juce::String()) + formatRecorderTime(elapsed, p.Fs) + "   ·   "
        + (remaining < 0 ? ko("남은 공간 확인 중") : ko("남은 공간 ") + juce::String(double(remaining) / 1000000000.0, 1) + "GB") + "   ·   " + status, juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, ui.live ? Palette::danger : Palette::dimText);
    errorLabel.setText(banner.isNotEmpty() ? banner : ui.warning, juce::dontSendNotification); errorLabel.setTooltip(banner);
    projectButton.setEnabled(!ui.structureLocked); settingsButton.setEnabled(!ui.structureLocked);
    normalButton.setToggleState(true, juce::dontSendNotification); normalButton.setEnabled(!ui.structureLocked);
    recordTab.setToggleState(!timeline, juce::dontSendNotification); timelineTab.setToggleState(timeline, juce::dontSendNotification);
    startButton.setEnabled(ui.canRecord); stopButton.setEnabled(ui.canStop); markerButton.setEnabled(ui.live || !ui.structureLocked);
    latestButton.setEnabled(ui.canTransport || ui.takeStatus == ko("정지 중")); latestButton.setVisible(true);
    microphoneViewport.setVisible(!timeline); noMicrophones.setVisible(!timeline && ui.armedMicrophones == 0);
    noMicrophones.setText(ko("녹음 중인 마이크가 없습니다"), juce::dontSendNotification);
    stripCount = 0; for (unsigned i = 0; i < s.physicalInputs.size(); ++i) if (s.physicalInputs[i] >= 0) stripCount = i + 1;
    for (unsigned i = 0; i < 8; ++i)
    {
        auto& mic = microphones[i]; const auto input = i < s.physicalInputs.size() ? s.physicalInputs[i] : -1;
        mic.setVisible(i < stripCount); mic.name.setText(s.microphoneNames[i].isEmpty() ? ko("마이크 ") + juce::String(i + 1) : s.microphoneNames[i], juce::dontSendNotification);
        mic.name.setEditable(false, !ui.structureLocked); mic.physical.setText(input >= 0 ? ko("물리 입력 ") + juce::String(input + 1) : ko("입력 선택 안 함"), juce::dontSendNotification);
        mic.arm.setToggleState(input >= 0 && s.microphoneArmed[i], juce::dontSendNotification); mic.arm.setEnabled(!ui.structureLocked && input >= 0); mic.monitor.setEnabled(input >= 0);
    }
    if (oldCount != stripCount || wasTimeline != timeline) resized();
}
void RecordView::updateMeters(const std::array<float, 8>& values)
{ for (unsigned i = 0; i < 8; ++i) { auto& m = microphones[i]; m.peak = juce::jmax(values[i], m.peak * .82f); m.repaint(8, 78, m.getWidth() - 16, 16); } }
void RecordView::paint(juce::Graphics& g) { g.fillAll(Palette::background); }
void RecordView::resized()
{
    auto a = getLocalBounds().reduced(12); auto top = a.removeFromTop(40);
    projectButton.setBounds(top.removeFromLeft(88).reduced(2)); exportButton.setBounds(top.removeFromRight(105).reduced(2)); settingsButton.setBounds(top.removeFromRight(70).reduced(2));
    dubButton.setBounds(top.removeFromRight(58).reduced(2)); normalButton.setBounds(top.removeFromRight(58).reduced(2)); timelineTab.setBounds(top.removeFromRight(94).reduced(2)); recordTab.setBounds(top.removeFromRight(66).reduced(2)); projectName.setBounds(top.reduced(6, 0));
    statusLabel.setBounds(a.removeFromTop(28)); errorLabel.setBounds(a.removeFromTop(32)); a.removeFromTop(6);
    const int cameraHeight = timeline ? juce::jlimit(120, 220, a.getHeight() / 3)
        : juce::jlimit(140, (a.getWidth() - 16) * 9 / 32 + 44, a.getHeight() - 228);
    auto cameraArea = a.removeFromTop(cameraHeight); auto l = cameraArea.removeFromLeft((cameraArea.getWidth() - 12) / 2); cameraArea.removeFromLeft(12); cameras[0].setBounds(l); cameras[1].setBounds(cameraArea);
    a.removeFromTop(8); auto controls = a.removeFromTop(38);
    startButton.setBounds(controls.removeFromLeft(128).reduced(2)); stopButton.setBounds(controls.removeFromLeft(76).reduced(2)); markerButton.setBounds(controls.removeFromLeft(120).reduced(2)); latestButton.setBounds(controls.removeFromLeft(178).reduced(2));
    a.removeFromTop(8); lowerBounds = a.withTrimmedBottom(18);
    noMicrophones.setBounds(a.removeFromBottom(25)); microphoneViewport.setBounds(a);
    strips.setSize(juce::jmax(a.getWidth() - 2, int(stripCount) * 176), 132);
    for (unsigned i = 0; i < 8; ++i) microphones[i].setBounds(int(i) * 176, 0, 172, 132);
}
}
