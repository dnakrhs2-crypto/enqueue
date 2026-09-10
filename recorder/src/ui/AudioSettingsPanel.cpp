#include "AudioSettingsPanel.h"

namespace gocue::recorder
{
namespace { void label(juce::Component& parent, juce::Label& l, const juce::String& text) { l.setText(text, juce::dontSendNotification); l.setFont(juce::Font(juce::FontOptions(17))); parent.addAndMakeVisible(l); } }
AudioSettingsPanel::AudioSettingsPanel(const UserSettings& s, const RecorderProject& p, const RecorderAudioEngine::DeviceInfo& info)
    : initial(s), fixed(!p.media->assets.empty()), projectFs(p.Fs)
{
    label(*this, deviceLabel, ko("오디오 장치")); label(*this, rateLabel, ko("샘플레이트"));
    label(*this, actualLabel, {}); label(*this, bufferLabel, ko("버퍼")); label(*this, latencyLabel, {});
    label(*this, leftLabel, ko("재생 출력 왼쪽")); label(*this, rightLabel, ko("재생 출력 오른쪽"));
    for (auto* box : {&devices, &rate, &buffer, &left, &right}) { addAndMakeVisible(box); box->setTextWhenNothingSelected(ko("선택 안 함")); }
    int id = 1; for (const auto& name : RecorderAudioEngine::deviceNames()) { devices.addItem(name, id); if (name == s.asioDeviceId) devices.setSelectedId(id, juce::dontSendNotification); ++id; }
    if (s.asioDeviceId.isNotEmpty() && !devices.getSelectedId()) devices.setText(s.asioDeviceId + ko(" · 연결 안 됨"), juce::dontSendNotification);
    if (s.asioDeviceId.isEmpty() && devices.getNumItems() > 0) devices.setSelectedId(1, juce::dontSendNotification); // the session picks the first driver too
    for (auto Fs : {44100, 48000, 88200, 96000, 192000}) rate.addItem(juce::String(Fs) + " Hz", Fs);
    rate.setSelectedId(int(fixed ? p.Fs : s.preferredSampleRate), juce::dontSendNotification); rate.setEnabled(!fixed);
    buffer.setEditableText(true); buffer.setText(juce::String(s.bufferSize), juce::dontSendNotification);
    addAndMakeVisible(controlPanel); addAndMakeVisible(mono); mono.setToggleState(s.output.mono, juce::dontSendNotification);
    // Every edit applies at once: the session (re)opens the device, there is no connect step.
    const auto changed = [this]
    {
        auto s = read(initial);
        if (s.asioDeviceId != actual.name) { s.physicalInputs.clear(); s.output = {}; } // a new device gets its first-run defaults
        actualLabel.setText(ko("장치를 연결하는 중입니다."), juce::dontSendNotification);
        if (onChanged) onChanged(s);
    };
    mono.onClick = [this, changed] { right.setEnabled(!mono.getToggleState()); leftLabel.setText(mono.getToggleState() ? ko("재생 출력 모노 채널") : ko("재생 출력 왼쪽"), juce::dontSendNotification); changed(); };
    for (auto* box : {&devices, &rate, &buffer, &left, &right}) box->onChange = changed;
    for (auto& box : inputs) box.onChange = changed;
    for (unsigned i = 0; i < 8; ++i) { label(*this, inputLabels[i], ko("마이크 ") + juce::String(i + 1) + ko(" · 물리 입력")); addAndMakeVisible(inputs[i]); }
    controlPanel.onClick = [this] { if (onControlPanel) onControlPanel(); };
    setDeviceInfo(info);
}
UserSettings AudioSettingsPanel::read(UserSettings s) const
{
    if (devices.getSelectedId()) s.asioDeviceId = devices.getText();
    s.preferredSampleRate = fixed ? projectFs : unsigned(rate.getSelectedId()); s.bufferSize = buffer.getText().getIntValue();
    s.output.mono = mono.getToggleState(); s.output.left = left.getSelectedId() - 2; s.output.right = right.getSelectedId() - 2;
    s.output.monoChannel = s.output.left; s.physicalInputs.clear();
    if (s.output.mono) { s.output.left = -1; s.output.right = -1; } else s.output.monoChannel = -1;
    for (const auto& box : inputs) s.physicalInputs.push_back(box.getSelectedId() - 2);
    return s;
}
void AudioSettingsPanel::setDeviceInfo(const RecorderAudioEngine::DeviceInfo& info)
{
    actual = info; const auto s = initial;
    actualLabel.setText(info.sampleRate ? ko("실제 ") + juce::String(info.sampleRate) + " Hz" + (fixed ? ko(" · 프로젝트 고정") : juce::String()) : ko("장치를 연결하면 실제 샘플레이트가 표시됩니다."), juce::dontSendNotification);
    latencyLabel.setText(info.sampleRate ? ko("입력 지연 ") + juce::String(info.inputLatency) + ko(" 샘플 / ") + juce::String(1000.0 * info.inputLatency / info.sampleRate, 2)
        + ko("ms   ·   출력 지연 ") + juce::String(info.outputLatency) + ko(" 샘플 / ") + juce::String(1000.0 * info.outputLatency / info.sampleRate, 2) + "ms" : ko("입출력 지연 확인 전"), juce::dontSendNotification);
    buffer.clear(juce::dontSendNotification); for (auto n : info.availableBuffers) buffer.addItem(juce::String(n), n);
    buffer.setText(juce::String(info.bufferFrames ? int(info.bufferFrames) : s.bufferSize), juce::dontSendNotification);
    for (auto* box : {&left, &right})
    {
        box->clear(juce::dontSendNotification); box->addItem(ko("선택 안 함"), 1);
        for (int i = 0; i < info.physicalOutputs; ++i) box->addItem(juce::String(i + 1) + " · " + info.outputNames[i], i + 2);
    }
    left.setSelectedId((s.output.mono ? s.output.monoChannel : s.output.left) + 2, juce::dontSendNotification);
    right.setSelectedId(s.output.right + 2, juce::dontSendNotification); right.setEnabled(!mono.getToggleState());
    for (unsigned m = 0; m < 8; ++m)
    {
        auto& box = inputs[m]; box.clear(juce::dontSendNotification); box.addItem(ko("사용 안 함"), 1);
        for (int i = 0; i < info.physicalInputs; ++i) box.addItem(juce::String(i + 1) + " · " + info.inputNames[i], i + 2);
        box.setSelectedId(m < s.physicalInputs.size() ? s.physicalInputs[m] + 2 : 1, juce::dontSendNotification);
    }
    controlPanel.setEnabled(info.sampleRate != 0);
}
void AudioSettingsPanel::resized()
{
    auto a = getLocalBounds().reduced(16); auto row = a.removeFromTop(38);
    deviceLabel.setBounds(row.removeFromLeft(112)); devices.setBounds(row.reduced(2));
    row = a.removeFromTop(38); rateLabel.setBounds(row.removeFromLeft(112)); rate.setBounds(row.removeFromLeft(155).reduced(2)); bufferLabel.setBounds(row.removeFromLeft(66)); buffer.setBounds(row.removeFromLeft(100).reduced(2)); controlPanel.setBounds(row.reduced(2));
    actualLabel.setBounds(a.removeFromTop(30)); latencyLabel.setBounds(a.removeFromTop(34));
    row = a.removeFromTop(32); mono.setBounds(row.removeFromLeft(145)); a.removeFromTop(4);
    row = a.removeFromTop(38); leftLabel.setBounds(row.removeFromLeft(178)); left.setBounds(row.reduced(2));
    row = a.removeFromTop(38); rightLabel.setBounds(row.removeFromLeft(178)); right.setBounds(row.reduced(2)); a.removeFromTop(12);
    for (unsigned i = 0; i < 8; ++i) { row = a.removeFromTop(34); inputLabels[i].setBounds(row.removeFromLeft(196)); inputs[i].setBounds(row.reduced(2)); }
}
}
