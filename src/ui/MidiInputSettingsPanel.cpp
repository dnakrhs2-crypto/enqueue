#include "ui/MidiInputSettingsPanel.h"
#include "app/ShortcutDisplay.h"
#include "ui/UiUtils.h"

namespace gocue
{
namespace
{
class DeviceRow : public juce::Component
{
public:
    DeviceRow() { addAndMakeVisible (selected); addAndMakeVisible (state); addAndMakeVisible (reconnect); state.setFont (Palette::font()); }
    void resized() override
    {
        auto area = getLocalBounds();
        reconnect.setBounds (area.removeFromRight (86).reduced (2));
        state.setBounds (area.removeFromRight (84));
        selected.setBounds (area);
    }
    juce::ToggleButton selected;
    juce::Label state;
    juce::TextButton reconnect { juce::String::fromUTF8 ("다시 연결") };
};
}
std::vector<MidiInputService::Device> MidiInputSettingsPanel::snapshot (const ShortcutService& service, const MidiInputService* input)
{
    auto result = input != nullptr ? input->devices() : std::vector<MidiInputService::Device>();
    const auto add = [&] (const juce::String& id)
    {
        if (id != "any" && std::none_of (result.begin(), result.end(), [&] (const auto& d) { return d.identifier == id; }))
            result.push_back ({ id, ShortcutDisplay::deviceName (service, id), MidiInputService::Status::disconnected, 0, 0 });
    };
    for (const auto& p : service.getMidiInputSettings().selected) add (p.first);
    for (const auto& p : service.getMidiProfile().overrides) for (const auto& trigger : p.second) add (trigger.source);
    return result;
}
MidiInputSettingsPanel::MidiInputSettingsPanel (ShortcutService& s, MidiInputService* i) : service (s), input (i), list ({}, this)
{
    for (auto* c : std::initializer_list<juce::Component*> { &fold, &refreshButton, &automatic, &background, &hint, &list }) addAndMakeVisible (c);
    fold.setWantsKeyboardFocus (false);
    fold.onClick = [this] { expanded = ! expanded; refresh(); resized(); if (onHeightChanged) onHeightChanged(); };
    refreshButton.setButtonText (ko ("새로고침"));
    refreshButton.onClick = [this] { if (input != nullptr) input->refresh(); refresh(); };
    automatic.setButtonText (ko ("모든 MIDI 입력 자동 사용"));
    automatic.setTooltip (ko ("켜면 나중에 연결한 입력도 자동으로 사용합니다. 기본값은 꺼짐입니다."));
    background.setButtonText (ko ("백그라운드에서 재생 조작 허용"));
    background.setTooltip (ko ("기본값은 켜짐. 선택된 입력의 패닉은 이 옵션과 무관하게 동작합니다."));
    automatic.onClick = [this] { auto next = service.getMidiInputSettings(); next.autoUseAll = automatic.getToggleState(); report (service.setMidiInputSettings (next)); };
    background.onClick = [this] { auto next = service.getMidiInputSettings(); next.allowBackgroundPlayback = background.getToggleState(); report (service.setMidiInputSettings (next)); };
    hint.setFont (Palette::font (Palette::fileSize));
    hint.setMinimumHorizontalScale (1.0f);
    list.setRowHeight (28);
    list.setColour (juce::ListBox::backgroundColourId, Palette::panel2);
    list.setOutlineThickness (1);
    refresh();
    startTimerHz (4);
}
MidiInputSettingsPanel::~MidiInputSettingsPanel() { stopTimer(); }
void MidiInputSettingsPanel::report (const ShortcutOperationResult& result)
{
    error = result.failed() ? ko ("저장하지 못했습니다: ") + result.getErrorMessage() : juce::String();
    refresh();
}
void MidiInputSettingsPanel::refresh()
{
    devices = snapshot (service, input);
    fold.setButtonText (ko (expanded ? "− " : "+ ") + ko ("MIDI 입력 — 이 PC")
        + (input != nullptr ? ko ("  ·  ") + ShortcutDisplay::midiSummary (*input, &service) : juce::String()));
    automatic.setToggleState (service.getMidiInputSettings().autoUseAll, juce::dontSendNotification);
    background.setToggleState (service.getMidiInputSettings().allowBackgroundPlayback, juce::dontSendNotification);
    automatic.setEnabled (! service.isEditingLocked()); background.setEnabled (! service.isEditingLocked());
    const auto message = service.isEditingLocked() ? ko ("쇼 모드: 입력 선택·재지정 잠김. 자동 재연결과 상태 표시는 계속됩니다.")
        : service.getMidiInputSettings().autoUseAll ? ko ("자동 사용 켜짐: 나중에 연결한 입력도 대상입니다.")
        : ko ("사용할 입력을 체크하세요. CC 첫 값은 기준값이며 실행되지 않습니다. 페달·버튼은 노트 권장.");
    hint.setText (error.isNotEmpty() ? error : message, juce::dontSendNotification); hint.setTooltip (hint.getText());
    for (auto* c : std::initializer_list<juce::Component*> { &automatic, &background, &hint, &list }) c->setVisible (expanded);
    list.updateContent();
}
juce::Component* MidiInputSettingsPanel::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    auto* view = dynamic_cast<DeviceRow*> (existing);
    if (view == nullptr) view = new DeviceRow();
    if (! juce::isPositiveAndBelow (row, getNumRows())) return view;
    const auto device = devices[static_cast<size_t> (row)];
    view->selected.setButtonText (device.name);
    view->selected.setTooltip (device.name + "\n" + device.identifier);
    view->selected.setToggleState (service.getMidiInputSettings().autoUseAll || service.getMidiInputSettings().selected.count (device.identifier) != 0, juce::dontSendNotification);
    view->selected.setEnabled (! service.isEditingLocked() && ! service.getMidiInputSettings().autoUseAll);
    view->state.setText (ShortcutDisplay::deviceStatus (device.status), juce::dontSendNotification);
    view->state.setTooltip (device.status == MidiInputService::Status::waiting ? ko ("입력 손실 뒤 다음 MIDI 메시지를 기다리는 중입니다.") : view->state.getText());
    if (device.overloaded) view->state.setTooltip (ShortcutDisplay::deviceStatus (device.status) + ko (" · 이 연결의 과부하 기록 있음. 푸터에서 누계를 확인하세요."));
    view->reconnect.setEnabled (! service.isEditingLocked() && device.status == MidiInputService::Status::disconnected);
    const juce::Component::SafePointer<MidiInputSettingsPanel> safe (this);
    view->selected.onClick = [safe, device]
    {
        if (safe == nullptr || safe->service.isEditingLocked()) return;
        auto next = safe->service.getMidiInputSettings();
        if (next.selected.erase (device.identifier) == 0) next.selected[device.identifier] = device.name;
        safe->report (safe->service.setMidiInputSettings (next));
    };
    view->reconnect.onClick = [safe, id = device.identifier] { if (safe != nullptr) safe->reconnect (id); };
    return view;
}
void MidiInputSettingsPanel::reconnect (const juce::String& previous)
{
    if (service.isEditingLocked()) return;
    juce::PopupMenu menu;
    std::vector<std::pair<juce::String, juce::String>> available;
    for (const auto& p : service.getAvailableMidiDevices()) { available.push_back (p); menu.addItem (static_cast<int> (available.size()), p.second + ko (" · ") + p.first); }
    if (available.empty()) { error = ko ("연결 가능한 입력이 없습니다. 장치를 연결하고 새로고침하세요."); refresh(); return; }
    const juce::Component::SafePointer<MidiInputSettingsPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&list), [safe, previous, available] (int result)
    {
        if (safe == nullptr || ! juce::isPositiveAndBelow (result - 1, static_cast<int> (available.size()))) return;
        const auto& selected = available[static_cast<size_t> (result - 1)];
        safe->report (safe->service.reconnectMidiInput (previous, selected.first, selected.second));
    });
}
void MidiInputSettingsPanel::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop (28);
    refreshButton.setBounds (top.removeFromRight (90).reduced (2)); fold.setBounds (top.reduced (0, 2));
    if (! expanded) return;
    area.removeFromTop (2); list.setBounds (area.removeFromTop (78));
    automatic.setBounds (area.removeFromTop (24)); background.setBounds (area.removeFromTop (24));
    hint.setBounds (area);
}
}
