#include "ui/CueMidiPanel.h"
#include "ui/MidiInputSettingsPanel.h"
#include "app/ShortcutDisplay.h"
#include "ui/UiUtils.h"
#include "ui/MidiModalScope.h"

namespace gocue
{
CueMidiPanel::CueMidiPanel (ProjectDocument& d, ShortcutService& s, MidiInputService* i, MidiTriggerRouter* r)
    : document (d), service (s), input (i), router (r)
{
    for (auto* component : std::initializer_list<juce::Component*> { &list, &status, &add, &change, &erase }) addAndMakeVisible (component);
    status.setFont (Palette::font()); status.setMinimumHorizontalScale (1.0f);
    status.setColour (juce::Label::textColourId, Palette::warn);
    list.setTextWhenNothingSelected (ko ("MIDI: 없음"));
    list.onChange = [this]
    {
        change.cancelCapture();
        editingIndex = list.getSelectedId() - 1;
        const auto values = triggers();
        original = juce::isPositiveAndBelow (editingIndex, static_cast<int> (values.size())) ? std::optional<MidiTrigger> (values[static_cast<size_t> (editingIndex)]) : std::nullopt;
        change.editMidi = original;
        refresh();
    };
    add.setButtonText (ko ("입력 추가")); change.setButtonText (ko ("변경")); erase.setButtonText (ko ("삭제"));
    for (auto* button : { &add, &change })
    {
        button->setService (service);
        button->validate = [this] (const juce::KeyPress& key) { return validateKey ? validateKey (key) : ko ("큐 핫키 대상이 없습니다."); };
        button->validateMidi = [this] (const MidiTrigger& trigger) { return validate (trigger); };
        button->onHotkeyChanged = [this] (const juce::String& key) { if (onHotkeyChanged) onHotkeyChanged (key); };
    }
    add.onMidiChanged = [this] (const MidiTrigger& trigger) { return apply (-1, trigger); };
    change.onMidiChanged = [this] (const MidiTrigger& trigger)
    {
        const auto values = triggers();
        if (! original || ! juce::isPositiveAndBelow (editingIndex, static_cast<int> (values.size())) || values[static_cast<size_t> (editingIndex)] != *original)
            return juce::Result::fail (ko ("MIDI 목록이 바뀌었습니다. 다시 선택하세요."));
        return apply (editingIndex, trigger);
    };
    erase.onClick = [this]
    {
        cancelCapture();
        const auto result = remove (list.getSelectedId() - 1);
        if (result.failed()) status.setText (result.getErrorMessage(), juce::dontSendNotification);
    };
    service.addListener (this); document.addListener (this); startTimerHz (4);
    refresh();
}
CueMidiPanel::~CueMidiPanel() { stopTimer(); cancelCapture(); document.removeListener (this); service.removeListener (this); }
const Cue* CueMidiPanel::selectedCue() const
{
    const auto* cue = document.cues.getSelected();
    return cue != nullptr && cue->id == cueID ? cue : nullptr;
}
void CueMidiPanel::setCue (const juce::Uuid& id)
{
    if (cueID != id) { cancelCapture(); cueID = id; list.setSelectedId (0, juce::dontSendNotification); original.reset(); editingIndex = -1; }
    refresh();
}
void CueMidiPanel::cancelCapture() { add.cancelCapture(); change.cancelCapture(); }
MidiTriggers CueMidiPanel::triggers() const { const auto* cue = selectedCue(); return cue != nullptr ? cue->midiTriggers : MidiTriggers(); }
KeyCapture::Decision CueMidiPanel::validate (const MidiTrigger& trigger) const
{
    if (selectedCue() == nullptr || ! isEnabled() || service.isEditingLocked()) return { false, ko ("큐 입력 편집이 잠겼습니다.") };
    const auto existing = triggers();
    for (const auto& other : document.getMidiTriggers())
        if (other.id != cueID && MidiTriggerRules::intersects (trigger, other.trigger)
            && std::find (existing.begin(), existing.end(), trigger) == existing.end()) return { false, ko ("다른 큐 MIDI와 충돌합니다: ") + other.id.toString() };
    return { true, statusFor (trigger) };
}
juce::Result CueMidiPanel::apply (int index, MidiTrigger trigger)
{
    trigger.source = "any";
    const auto checked = validate (trigger);
    if (! checked.allowed) return juce::Result::fail (checked.message);
    auto values = triggers();
    if (index < 0) values.push_back (trigger);
    else if (juce::isPositiveAndBelow (index, static_cast<int> (values.size()))) values[static_cast<size_t> (index)] = trigger;
    else return juce::Result::fail ("Unknown MIDI binding");
    return document.setMidiTriggers (cueID, std::move (values));
}
juce::Result CueMidiPanel::remove (int index)
{
    if (selectedCue() == nullptr || service.isEditingLocked() || ! isEnabled()) return juce::Result::fail ("Cue editing is locked");
    auto values = triggers();
    if (! juce::isPositiveAndBelow (index, static_cast<int> (values.size()))) return juce::Result::fail ("Unknown MIDI binding");
    values.erase (values.begin() + index);
    return document.setMidiTriggers (cueID, std::move (values));
}
juce::String CueMidiPanel::statusFor (const MidiTrigger& trigger) const
{
    for (const auto& command : service.midiCommandBindings())
        if (MidiTriggerRules::intersects (trigger, command.trigger))
            return ko ("이 PC의 ") + ShortcutCatalog::get().find (command.id)->name + ko (" MIDI와 충돌하여 비활성");
    for (const auto& other : document.getMidiTriggers())
        if (other.id != cueID && MidiTriggerRules::intersects (trigger, other.trigger)) return ko ("다른 큐 MIDI와 충돌하여 비활성");
    if (const auto* cue = selectedCue(); cue != nullptr && ! cue->armed) return ko ("큐 비활성");
    if (service.isCapturing()) return ko ("학습 중 — MIDI 실행 억제");
    if (MidiModalScope::active()) return ko ("모달 창 — MIDI 실행 억제");
    if (! service.getMidiInputSettings().allowBackgroundPlayback && ! juce::Process::isForegroundProcess())
        return ko ("백그라운드 재생 조작 꺼짐 — 실행 억제");
    const auto connection = ShortcutDisplay::midiState (service, trigger, MidiInputSettingsPanel::snapshot (service, input));
    if (connection != ko ("연결") && connection != ko ("준비 대기")) return ko ("장치 ") + connection;
    return router != nullptr ? router->bindingStatus ({ cueID.toString(), trigger, 0, true }) : connection;
}
void CueMidiPanel::refresh()
{
    const int selected = list.getSelectedId();
    list.clear (juce::dontSendNotification);
    const auto values = triggers();
    juce::StringArray all;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const auto text = "MIDI: " + ShortcutDisplay::midi (service, { values[i] });
        list.addItem (text, static_cast<int> (i) + 1); all.add (ShortcutDisplay::midiDetails (service, values[i]) + " — " + statusFor (values[i]));
    }
    list.setSelectedId (values.empty() ? 0 : juce::jlimit (1, static_cast<int> (values.size()), selected), juce::dontSendNotification);
    list.setTooltip (all.joinIntoString ("\n"));
    if (! values.empty())
    { editingIndex = list.getSelectedId() - 1; original = values[static_cast<size_t> (editingIndex)]; change.editMidi = original; }
    const bool enabled = selectedCue() != nullptr && isEnabled() && ! service.isEditingLocked();
    add.setEnabled (enabled); change.setEnabled (enabled && ! values.empty()); erase.setEnabled (enabled && ! values.empty());
    timerCallback();
}
void CueMidiPanel::timerCallback()
{
    const auto values = triggers();
    const int index = list.getSelectedId() - 1;
    const auto text = juce::isPositiveAndBelow (index, static_cast<int> (values.size())) ? statusFor (values[static_cast<size_t> (index)])
        : ko ("큐 MIDI는 프로젝트에 저장됩니다. 허용 장치 모두 사용.");
    status.setText (text, juce::dontSendNotification); status.setTooltip (text);
}
void CueMidiPanel::resized()
{
    auto area = getLocalBounds();
    auto row = area.removeFromTop (26);
    erase.setBounds (row.removeFromRight (56)); row.removeFromRight (4);
    change.setBounds (row.removeFromRight (56)); row.removeFromRight (4);
    add.setBounds (row.removeFromRight (92)); row.removeFromRight (6);
    list.setBounds (row);
    status.setBounds (area.removeFromTop (22));
}
}
