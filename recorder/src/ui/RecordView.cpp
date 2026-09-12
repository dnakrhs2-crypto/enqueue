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
    const auto card = getLocalBounds().toFloat().reduced(.5f);
    g.setColour(Palette::card); g.fillRoundedRectangle(card, Palette::cardRadius);
    g.setColour(Palette::line); g.drawRoundedRectangle(card, Palette::cardRadius, 1);
    auto heading = getLocalBounds().reduced(10, 0).removeFromTop(34);
    if (recording)
    {
        const auto badge = heading.removeFromRight(42).withSizeKeepingCentre(38, 18).toFloat();
        g.setColour(Palette::recording); g.fillRoundedRectangle(badge, 9);
        g.setColour(juce::Colours::white); g.setFont(recorderFont(10.5f, juce::Font::bold));
        g.drawText("REC", badge, juce::Justification::centred);
    }
    g.setColour(Palette::text); g.setFont(recorderFont(14, juce::Font::bold));
    g.drawFittedText(caption, heading, juce::Justification::centredLeft, 1);
    g.setColour(juce::Colours::black); g.fillRect(host.getBounds());
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
    name.setFont(recorderFont(13.5f, juce::Font::bold));
    physical.setFont(recorderFont(11.5f)); physical.setColour(juce::Label::textColourId, Palette::dimText);
}
void RecordView::Microphone::paint(juce::Graphics& g)
{
    const auto card = getLocalBounds().reduced(3).toFloat().reduced(.5f);
    g.setColour(Palette::card); g.fillRoundedRectangle(card, Palette::cardRadius);
    g.setColour(Palette::line); g.drawRoundedRectangle(card, Palette::cardRadius, 1);
    auto meter = juce::Rectangle<float>(12, 80, float(getWidth() - 24), 10); g.setColour(Palette::meterBg); g.fillRoundedRectangle(meter, 3);
    const auto level = peak > 0 ? juce::jlimit(0.0f, 1.0f, (60 + 20 * std::log10(peak)) / 60) : 0.0f;
    g.setColour(peak >= .98f ? Palette::meterRed : peak >= .8f ? Palette::meterYellow : Palette::meterGreen); g.fillRoundedRectangle(meter.withWidth(meter.getWidth() * level), 3);
    g.setColour(Palette::line); g.drawRoundedRectangle(meter, 3, 1);
}
void RecordView::Microphone::resized()
{ name.setBounds(8, 6, getWidth() - 16, 26); physical.setBounds(8, 32, getWidth() - 16, 22); arm.setBounds(8, 54, getWidth() - 16, 24); monitor.setBounds(8, 96, getWidth() - 16, 28); }
RecordView::RecordView()
{
    for (auto* b : {&projectButton, &recordTab, &timelineTab, &importButton, &settingsButton, &exportButton, &startButton, &stopButton, &markerButton}) { addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    for (auto* l : {&projectName, &statusLabel, &errorLabel, &noMicrophones}) { addAndMakeVisible(l); l->setFont(juce::Font(juce::FontOptions(17))); }
    projectName.setFont(recorderFont(16, juce::Font::bold)); errorLabel.setColour(juce::Label::textColourId, Palette::danger);
    statusLabel.setFont(recorderMonoFont(12)); errorLabel.setFont(recorderFont(12));
    for (auto* b : {&projectButton, &recordTab, &timelineTab, &importButton, &settingsButton, &exportButton, &startButton, &stopButton, &markerButton})
        b->getProperties().set("recorderFontSize", 13.0f);
    projectButton.getProperties().set("recorderMenuArrow", true);
    recordTab.setConnectedEdges(juce::Button::ConnectedOnRight); timelineTab.setConnectedEdges(juce::Button::ConnectedOnLeft);
    recordTab.setColour(juce::TextButton::textColourOffId, Palette::dimText); timelineTab.setColour(juce::TextButton::textColourOffId, Palette::dimText);
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
    startButton.setColour(juce::TextButton::buttonColourId, Palette::brand);
    startButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
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
        const bool recording = ui.live && s.cameraEnabled[i];
        if (cameras[i].recording != recording) { cameras[i].recording = recording; cameras[i].repaint(); }
    }
    projectName.setText(p.name, juce::dontSendNotification); projectName.setTooltip(p.name);
    statusLabel.setText((ui.live ? ko("녹화 중   ·   ") : juce::String()) + formatRecorderTime(elapsed, p.Fs) + "   ·   "
        + (remaining < 0 ? ko("남은 공간 확인 중") : ko("남은 공간 ") + juce::String(double(remaining) / 1000000000.0, 1) + "GB") + "   ·   " + status, juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, ui.live ? Palette::danger : Palette::dimText);
    const bool hadNotice = errorLabel.getText().isNotEmpty();
    errorLabel.setText(banner.isNotEmpty() ? banner : ui.warning, juce::dontSendNotification); errorLabel.setTooltip(banner);
    projectButton.setEnabled(!ui.structureLocked); settingsButton.setEnabled(!ui.structureLocked);
    recordTab.setToggleState(!timeline, juce::dontSendNotification); timelineTab.setToggleState(timeline, juce::dontSendNotification);
    startButton.setEnabled(ui.canRecord); stopButton.setEnabled(ui.canStop); markerButton.setEnabled(ui.live || !ui.structureLocked);
    stopButton.setColour(RecorderLookAndFeel::buttonOutlineColourId, ui.live ? Palette::recording : Palette::line);
    stopButton.setColour(juce::TextButton::textColourOffId, ui.live ? Palette::recording : Palette::text);
    stopButton.setColour(juce::TextButton::buttonColourId, ui.live ? Palette::bar.overlaidWith(Palette::recording.withAlpha(.14f)) : Palette::bar);
    microphoneViewport.setVisible(!timeline); noMicrophones.setVisible(!timeline && ui.armedMicrophones == 0);
    noMicrophones.setText(ko("녹음 중인 마이크가 없습니다"), juce::dontSendNotification);
    stripCount = 0; for (unsigned i = 0; i < s.physicalInputs.size(); ++i) if (s.physicalInputs[i] >= 0) stripCount = i + 1;
    for (unsigned i = 0; i < 8; ++i)
    {
        auto& mic = microphones[i]; const auto input = i < s.physicalInputs.size() ? s.physicalInputs[i] : -1;
        mic.setVisible(i < stripCount); mic.name.setText(s.microphoneNames[i].isEmpty() ? ko("마이크 ") + juce::String(i + 1) : s.microphoneNames[i], juce::dontSendNotification);
        mic.name.setEditable(false, !ui.structureLocked); mic.physical.setText(input >= 0 ? ko("물리 입력 ") + juce::String(input + 1)
            + (s.stereoSlots[i] ? "+" + juce::String(input + 2) + ko(" (스테레오)") : juce::String()) : ko("입력 선택 안 함"), juce::dontSendNotification);
        mic.physical.setTooltip(s.stereoSlots[i] ? ko("입력 미터: L/R 중 큰 값") : ko("입력 미터: 모노"));
        mic.arm.setToggleState(input >= 0 && s.microphoneArmed[i], juce::dontSendNotification); mic.arm.setEnabled(!ui.structureLocked && input >= 0); mic.monitor.setEnabled(input >= 0);
    }
    // The notice row is 0 px tall while empty: a notice that appears (or clears) later must re-run the layout so the row
    // shows and timelineBounds() moves with it. JUCE skips a child's resized() when the parent re-applies identical bounds.
    if (oldCount != stripCount || wasTimeline != timeline || hadNotice != errorLabel.getText().isNotEmpty()) resized();
}
void RecordView::updateMeters(const std::array<float, 8>& values)
{ for (unsigned i = 0; i < 8; ++i) { auto& m = microphones[i]; m.peak = juce::jmax(values[i], m.peak * .82f); m.repaint(8, 78, m.getWidth() - 16, 16); } }
void RecordView::paint(juce::Graphics& g)
{
    g.fillAll(Palette::background); g.setColour(Palette::card); g.fillRect(0, 0, getWidth(), 46);
    g.setColour(Palette::line); g.fillRect(0, 45, getWidth(), 1);
}
void RecordView::paintOverChildren(juce::Graphics& g)
{
    g.setColour(Palette::line);
    g.drawRoundedRectangle(recordTab.getBounds().getUnion(timelineTab.getBounds()).toFloat().reduced(.5f), Palette::controlRadius, 1);
}
void RecordView::resized()
{
    auto a = getLocalBounds(); auto top = a.removeFromTop(46).reduced(12, 8);
    projectButton.setBounds(top.removeFromLeft(96)); top.removeFromLeft(8);
    exportButton.setBounds(top.removeFromRight(82)); top.removeFromRight(8);
    settingsButton.setBounds(top.removeFromRight(64)); top.removeFromRight(8);
    importButton.setBounds(top.removeFromRight(148)); top.removeFromRight(14);
    timelineTab.setBounds(top.removeFromRight(94)); recordTab.setBounds(top.removeFromRight(66));
    top.removeFromRight(12); projectName.setBounds(top.reduced(6, 0));
    statusLabel.setBounds(a.removeFromTop(26).reduced(12, 0));
    errorLabel.setBounds(a.removeFromTop(errorLabel.getText().isEmpty() ? 0 : 26).reduced(12, 0));
    a = a.reduced(12, 0); a.removeFromTop(8);
    const int cameraHeight = timeline ? juce::jlimit(120, 236, a.getHeight() / 3)
        : juce::jlimit(140, (a.getWidth() - 16) * 9 / 32 + 44, a.getHeight() - 228);
    auto cameraArea = a.removeFromTop(cameraHeight); auto l = cameraArea.removeFromLeft((cameraArea.getWidth() - 12) / 2); cameraArea.removeFromLeft(12); cameras[0].setBounds(l); cameras[1].setBounds(cameraArea);
    a.removeFromTop(8); auto controls = a.removeFromTop(38);
    startButton.setBounds(controls.removeFromLeft(128).reduced(2)); stopButton.setBounds(controls.removeFromLeft(76).reduced(2)); markerButton.setBounds(controls.removeFromLeft(120).reduced(2));
    a.removeFromTop(8); lowerBounds = a.withTrimmedBottom(10);
    noMicrophones.setBounds(a.removeFromBottom(25)); microphoneViewport.setBounds(a);
    strips.setSize(juce::jmax(a.getWidth() - 2, int(stripCount) * 176), 132);
    for (unsigned i = 0; i < 8; ++i) microphones[i].setBounds(int(i) * 176, 0, 172, 132);
}
}
