#include "AudioSettingsPanel.h"
#include "app/RecorderSession.h"

namespace gocue::recorder
{
namespace { void label(juce::Component& parent, juce::Label& l, const juce::String& text) { l.setText(text, juce::dontSendNotification); l.setFont(juce::Font(juce::FontOptions(17))); parent.addAndMakeVisible(l); } }
AudioSettingsPanel::AudioSettingsPanel(const UserSettings& s, const RecorderProject& p, const RecorderAudioEngine::DeviceInfo& info)
    : initial(s), fixed(!p.media->assets.empty()), projectFs(p.Fs)
{
    label(*this, deviceLabel, ko("오디오 장치")); label(*this, rateLabel, ko("샘플레이트"));
    label(*this, actualLabel, {}); label(*this, bufferLabel, ko("버퍼")); label(*this, latencyLabel, {});
    label(*this, leftLabel, ko("재생 출력 왼쪽")); label(*this, rightLabel, ko("재생 출력 오른쪽"));
    devices.setComponentID("audioDevice");
    for (auto* box : {&devices, &rate, &buffer, &left, &right}) { addAndMakeVisible(box); box->setTextWhenNothingSelected(ko("선택 안 함")); }
    int id = 1; for (const auto& name : RecorderAudioEngine::deviceNames()) { devices.addItem(name, id); if (name == s.asioDeviceId) devices.setSelectedId(id, juce::dontSendNotification); ++id; }
    if (s.asioDeviceId.isNotEmpty() && !devices.getSelectedId()) devices.setText(s.asioDeviceId + ko(" · 연결 안 됨"), juce::dontSendNotification);
    if (s.asioDeviceId.isEmpty() && devices.getNumItems() > 0) devices.setSelectedId(1, juce::dontSendNotification); // the session picks the first driver too
    for (auto Fs : {44100, 48000, 88200, 96000, 192000}) rate.addItem(juce::String(Fs) + " Hz", Fs);
    selectRate(fixed ? p.Fs : s.preferredSampleRate); rate.setEnabled(!fixed);
    buffer.setEditableText(true); buffer.setText(juce::String(s.bufferSize), juce::dontSendNotification);
    addAndMakeVisible(controlPanel); addAndMakeVisible(mono); mono.setToggleState(s.output.mono, juce::dontSendNotification);
    // Every edit applies at once: the session (re)opens the device, there is no connect step.
    const auto changed = [this]
    {
        refreshInputChoices();
        auto s = read(initial);
        if (s.asioDeviceId != actual.name) { s.physicalInputs.clear(); s.stereoSlots.fill(false); s.output = {}; s.audioDefaultsApplied = false; } // a new device gets its first-run defaults
        actualLabel.setText(ko("장치를 연결하는 중입니다."), juce::dontSendNotification);
        if (onChanged) onChanged(s);
    };
    mono.onClick = [this, changed] { right.setEnabled(!mono.getToggleState()); leftLabel.setText(mono.getToggleState() ? ko("재생 출력 모노 채널") : ko("재생 출력 왼쪽"), juce::dontSendNotification); changed(); };
    for (auto* box : {&devices, &rate, &buffer, &left, &right}) box->onChange = changed;
    for (unsigned i = 0; i < inputs.size(); ++i)
    {
        const auto number = juce::String(i + 1), name = ko("마이크 ") + number;
        label(*this, inputLabels[i], {}); inputLabels[i].setFont(juce::Font(juce::FontOptions(15)));
        inputLabels[i].setComponentID("microphoneSummary" + number);
        label(*this, inputHints[i], {}); inputHints[i].setFont(juce::Font(juce::FontOptions(14)));
        inputHints[i].setComponentID("microphoneHint" + number);
        inputs[i].setComponentID("microphoneInput" + number); inputs[i].setTitle(name + ko(" 물리 입력 (스테레오 왼쪽)"));
        addAndMakeVisible(inputs[i]);
        inputs[i].onChange = [this, i, changed]
        {
            const bool active = inputs[i].getSelectedId() > 1;
            inputMono[i].setToggleState(active && !inputStereo[i].getToggleState(), juce::dontSendNotification);
            if (!active) inputStereo[i].setToggleState(false, juce::dontSendNotification);
            changed();
        };
        for (auto* button : {&inputMono[i], &inputStereo[i]})
        {
            button->setClickingTogglesState(true); button->setRadioGroupId(int(i) + 100);
            addAndMakeVisible(button);
        }
        inputMono[i].setButtonText(ko("모노")); inputMono[i].setComponentID("microphoneMono" + number);
        inputStereo[i].setButtonText(ko("스테레오")); inputStereo[i].setComponentID("microphoneStereo" + number);
        inputMono[i].setTitle(name + ko(" 모노")); inputStereo[i].setTitle(name + ko(" 스테레오"));
        inputMono[i].setConnectedEdges(juce::Button::ConnectedOnRight);
        inputStereo[i].setConnectedEdges(juce::Button::ConnectedOnLeft);
        inputMono[i].onClick = [this, i, changed] { if (inputMono[i].getToggleState()) changed(); };
        inputStereo[i].onClick = [this, i, changed] { if (inputStereo[i].getToggleState()) changed(); };
    }
    for (unsigned page = 0; page < inputPages.size(); ++page)
    {
        auto& button = inputPages[page]; addAndMakeVisible(button);
        button.setComponentID("microphonePage" + juce::String(page + 1));
        button.onClick = [this, page] { inputPage = page; resized(); };
    }
    for (unsigned i = 0; i < s.physicalInputs.size(); ++i)
        if (s.physicalInputs[i] >= 0) { inputPage = i / 4; break; }
    controlPanel.onClick = [this] { if (onControlPanel) onControlPanel(); };
    setDeviceInfo(info);
}
juce::Result AudioSettingsPanel::configure(RecorderSession& session, UserSettings requested, const UserSettings& applied)
{
    // Keep the reason visible beside the restored slot, including a queued edit
    // rejected against a more recently applied configuration.
    std::array<juce::String, 8> conflicts;
    for (unsigned i = 0; i < inputs.size(); ++i)
    {
        const int next = i < requested.physicalInputs.size() ? requested.physicalInputs[i] : -1;
        const int previous = i < applied.physicalInputs.size() ? applied.physicalInputs[i] : -1;
        if (next != previous || requested.stereoSlots[i] != applied.stereoSlots[i])
            conflicts[i] = inputConflict(i, next, requested.stereoSlots[i], requested);
    }
    const auto result = session.configure(std::move(requested));
    if (result.failed())
    {
        setSettings(applied); setDeviceInfo(session.audioEngine().deviceInfo());
        for (unsigned i = 0; i < inputs.size(); ++i) if (conflicts[i].isNotEmpty())
        {
            inputHints[i].setText(ko("변경 취소 · ") + conflicts[i], juce::dontSendNotification);
            inputHints[i].setColour(juce::Label::textColourId, Palette::danger);
            inputHints[i].setTooltip(inputHints[i].getText());
        }
    }
    return result;
}
void AudioSettingsPanel::setSettings(const UserSettings& s)
{
    initial = s;
    for (int i = 0; i < devices.getNumItems(); ++i) if (devices.getItemText(i) == s.asioDeviceId) devices.setSelectedId(devices.getItemId(i), juce::dontSendNotification);
    selectRate(fixed ? projectFs : s.preferredSampleRate);
    buffer.setText(juce::String(s.bufferSize), juce::dontSendNotification);
    mono.setToggleState(s.output.mono, juce::dontSendNotification); right.setEnabled(!s.output.mono);
    leftLabel.setText(s.output.mono ? ko("재생 출력 모노 채널") : ko("재생 출력 왼쪽"), juce::dontSendNotification);
    setDeviceInfo(actual);
}
void AudioSettingsPanel::selectRate(unsigned fs)
{
    if (fs && rate.indexOfItemId(int(fs)) < 0) rate.addItem(juce::String(int(fs)) + " Hz", int(fs)); // driver-reported rate outside the preset list
    rate.setSelectedId(int(fs), juce::dontSendNotification);
}
void AudioSettingsPanel::setBusy(bool configuring) { busy = configuring; controlPanel.setEnabled(!busy && actual.sampleRate != 0); }
UserSettings AudioSettingsPanel::read(UserSettings s) const
{
    if (devices.getSelectedId()) s.asioDeviceId = devices.getText();
    s.preferredSampleRate = fixed ? projectFs : unsigned(rate.getSelectedId()); s.bufferSize = buffer.getText().getIntValue();
    s.output.mono = mono.getToggleState(); s.output.left = left.getSelectedId() - 2; s.output.right = right.getSelectedId() - 2;
    s.output.monoChannel = s.output.left; s.physicalInputs.clear();
    if (s.output.mono) { s.output.left = -1; s.output.right = -1; } else s.output.monoChannel = -1;
    for (unsigned i = 0; i < inputs.size(); ++i)
    {
        const int id = inputs[i].getSelectedId();
        s.physicalInputs.push_back(id > 1 ? id - 2 : -1);
        s.stereoSlots[i] = id > 1 && inputStereo[i].getToggleState();
    }
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
        for (int i = 0; i < info.physicalInputs; ++i) box.addItem(inputName(i), i + 2);
        box.setSelectedId(m < s.physicalInputs.size() && s.physicalInputs[m] >= 0 ? s.physicalInputs[m] + 2 : 1, juce::dontSendNotification);
        const bool stereo = box.getSelectedId() > 1 && s.stereoSlots[m];
        inputMono[m].setToggleState(box.getSelectedId() > 1 && !stereo, juce::dontSendNotification);
        inputStereo[m].setToggleState(stereo, juce::dontSendNotification);
    }
    refreshInputChoices();
    controlPanel.setEnabled(!busy && info.sampleRate != 0);
}
juce::String AudioSettingsPanel::inputName(int physicalInput) const
{
    auto text = juce::String(physicalInput + 1);
    if (physicalInput >= 0 && physicalInput < actual.inputNames.size() && actual.inputNames[physicalInput].isNotEmpty())
        text += ko(" · ") + actual.inputNames[physicalInput];
    return text;
}
juce::String AudioSettingsPanel::inputConflict(unsigned slot, int leftInput, bool stereo, const UserSettings& s) const
{
    if (leftInput < 0) return {};
    juce::StringArray conflicts;
    for (unsigned other = 0; other < inputs.size() && other < s.physicalInputs.size(); ++other)
    {
        const int otherLeft = s.physicalInputs[other];
        if (other == slot || otherLeft < 0) continue;
        const int common = juce::jmax(leftInput, otherLeft);
        if (common <= juce::jmin(leftInput + int(stereo), otherLeft + int(s.stereoSlots[other])))
            conflicts.add(ko("입력 ") + juce::String(common + 1) + ko(": 마이크 ") + juce::String(other + 1) + ko("와 겹침"));
    }
    return conflicts.joinIntoString(ko(" / "));
}
void AudioSettingsPanel::refreshInputChoices()
{
    const auto s = read(initial);
    for (unsigned i = 0; i < inputs.size(); ++i)
    {
        auto& box = inputs[i]; const int selected = box.getSelectedId(), leftInput = s.physicalInputs[i];
        const bool stereo = s.stereoSlots[i], active = leftInput >= 0;
        for (int physical = 0; physical < actual.physicalInputs; ++physical)
        {
            auto reason = inputConflict(i, physical, stereo, s);
            if (stereo && physical + 1 >= actual.physicalInputs) reason = ko("오른쪽 입력 없음 · 모노로 먼저 변경");
            box.changeItemText(physical + 2, inputName(physical) + (reason.isEmpty() ? juce::String() : ko(" · ") + reason));
            box.setItemEnabled(physical + 2, reason.isEmpty());
        }
        box.setSelectedId(selected, juce::dontSendNotification); // refresh any changed menu annotation, without another edit
        auto summary = ko("마이크 ") + juce::String(i + 1) + ko(" · ");
        summary += active ? ko("입력 ") + juce::String(leftInput + 1)
            + (stereo ? "+" + juce::String(leftInput + 2) + ko(" (스테레오)") : ko(" (모노)")) : ko("사용 안 함");
        inputLabels[i].setText(summary, juce::dontSendNotification);
        inputLabels[i].setColour(juce::Label::textColourId, active ? Palette::text : Palette::dimText);
        auto stereoReason = !active ? ko("물리 입력을 먼저 선택하세요.")
            : leftInput + 1 >= actual.physicalInputs ? ko("마지막 입력에는 오른쪽 채널이 없습니다.")
            : inputConflict(i, leftInput, true, s);
        inputMono[i].setEnabled(active);
        inputStereo[i].setEnabled(active && stereoReason.isEmpty());
        inputStereo[i].setTooltip(stereoReason.isEmpty() ? ko("스테레오 · 오른쪽 = ") + inputName(leftInput + 1) : stereoReason);
        inputMono[i].setTooltip(ko("선택한 물리 입력 하나를 녹음합니다."));
        auto hint = inputConflict(i, leftInput, stereo, s);
        const bool conflict = hint.isNotEmpty();
        if (hint.isEmpty())
            hint = !active ? ko("물리 입력을 선택하면 이 마이크를 사용할 수 있습니다.")
                : stereoReason.isNotEmpty() ? ko("스테레오 불가 · ") + stereoReason
                : stereo ? ko("스테레오 · 오른쪽 = ") + inputName(leftInput + 1) + ko(" (자동)")
                : ko("모노 · 입력 1개 / 스테레오는 다음 입력을 오른쪽으로 사용");
        inputHints[i].setText(hint, juce::dontSendNotification); inputHints[i].setTooltip(hint);
        inputHints[i].setColour(juce::Label::textColourId, conflict ? Palette::danger : Palette::dimText);
        box.setTooltip(active ? inputName(leftInput) + (conflict ? ko(" · ") + hint : juce::String()) : ko("사용 안 함"));
    }
    for (unsigned page = 0; page < inputPages.size(); ++page)
    {
        unsigned count = 0;
        for (unsigned i = page * 4; i < page * 4 + 4; ++i) if (s.physicalInputs[i] >= 0) ++count;
        inputPages[page].setButtonText(ko("마이크 ") + juce::String(page * 4 + 1) + "–" + juce::String(page * 4 + 4)
            + ko(" · ") + juce::String(count) + ko("개 사용"));
    }
}
void AudioSettingsPanel::resized()
{
    auto a = getLocalBounds().reduced(16); auto row = a.removeFromTop(38);
    deviceLabel.setBounds(row.removeFromLeft(112)); devices.setBounds(row.reduced(2));
    row = a.removeFromTop(38); rateLabel.setBounds(row.removeFromLeft(112)); rate.setBounds(row.removeFromLeft(155).reduced(2)); bufferLabel.setBounds(row.removeFromLeft(66)); buffer.setBounds(row.removeFromLeft(100).reduced(2)); controlPanel.setBounds(row.reduced(2));
    actualLabel.setBounds(a.removeFromTop(24)); latencyLabel.setBounds(a.removeFromTop(26));
    row = a.removeFromTop(30); mono.setBounds(row.removeFromLeft(145)); a.removeFromTop(4);
    row = a.removeFromTop(34); leftLabel.setBounds(row.removeFromLeft(178)); left.setBounds(row.reduced(2));
    row = a.removeFromTop(34); rightLabel.setBounds(row.removeFromLeft(178)); right.setBounds(row.reduced(2)); a.removeFromTop(8);
    // SettingsForm fixes the panel at 588px: four rows per page keep all eight slots accessible.
    row = a.removeFromTop(32); const int pageWidth = row.getWidth() / 2;
    for (unsigned page = 0; page < inputPages.size(); ++page)
    {
        inputPages[page].setBounds(row.removeFromLeft(pageWidth).reduced(2));
        inputPages[page].setToggleState(page == inputPage, juce::dontSendNotification);
    }
    for (unsigned i = 0; i < inputs.size(); ++i)
    {
        const bool visible = i / 4 == inputPage;
        for (auto* component : std::array<juce::Component*, 5>{&inputLabels[i], &inputHints[i], &inputs[i], &inputMono[i], &inputStereo[i]})
            component->setVisible(visible);
        if (!visible) continue;
        inputLabels[i].setBounds(a.removeFromTop(20)); row = a.removeFromTop(30);
        inputStereo[i].setBounds(row.removeFromRight(100).reduced(0, 1));
        inputMono[i].setBounds(row.removeFromRight(70).reduced(0, 1)); row.removeFromRight(8);
        inputs[i].setBounds(row.reduced(2, 1));
        inputHints[i].setBounds(a.removeFromTop(22));
    }
}
}
