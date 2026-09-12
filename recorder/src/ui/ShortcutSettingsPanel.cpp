#include "ShortcutSettingsPanel.h"

namespace gocue::recorder
{
ShortcutSettingsPanel::ShortcutSettingsPanel(const UserSettings& s) : bindings(s.shortcuts)
{
    addAndMakeVisible(hint); addAndMakeVisible(error); addAndMakeVisible(reset);
    hint.setText(ko("버튼을 누른 뒤 사용할 키를 입력하세요. Esc: 입력 취소\n텍스트를 입력하는 동안에는 단축키가 실행되지 않습니다.\n녹화 정지와 재생/정지는 같은 키를 쓸 수 있습니다(녹화 중엔 정지)"), juce::dontSendNotification);
    error.setColour(juce::Label::textColourId, Palette::danger);
    reset.onClick = [this] { bindings = {}; capturing = -1; error.setText({}, juce::dontSendNotification); update(); };
    for (std::size_t i = 0; i < captures.size(); ++i)
    {
        addAndMakeVisible(labels[i]); addAndMakeVisible(captures[i]);
        labels[i].setText(RecorderShortcuts::name(RecorderCommand(i)), juce::dontSendNotification);
        captures[i].setTitle(labels[i].getText());
        captures[i].onClick = [this, i] { capturing = int(i); captures[i].grabKeyboardFocus(); update(); };
        captures[i].onKey = [this, i](const juce::KeyPress& key)
        {
            if (key.getKeyCode() == juce::KeyPress::escapeKey) { capturing = -1; update(); return true; }
            if (capturing != int(i))
            { if (key.getKeyCode() == juce::KeyPress::returnKey || key.getKeyCode() == juce::KeyPress::spaceKey) { capturing = int(i); update(); return true; } return false; }
            auto next = bindings; next.keys[i] = juce::KeyPress(key.getKeyCode(), key.getModifiers(), 0).getTextDescription();
            const auto result = next.validate(); error.setText(result.getErrorMessage(), juce::dontSendNotification);
            if (result.wasOk()) { bindings = std::move(next); capturing = -1; }
            update();
            return true;
        };
    }
    update();
}
void ShortcutSettingsPanel::update()
{
    for (std::size_t i = 0; i < captures.size(); ++i)
        captures[i].setButtonText(capturing == int(i) ? ko("키를 누르세요…") : bindings.keys[i]);
}
void ShortcutSettingsPanel::resized()
{
    auto a = getLocalBounds().reduced(20); hint.setBounds(a.removeFromTop(88)); a.removeFromTop(12);
    for (std::size_t i = 0; i < captures.size(); ++i)
    { auto row = a.removeFromTop(48); labels[i].setBounds(row.removeFromLeft(180)); captures[i].setBounds(row.reduced(4)); }
    error.setBounds(a.removeFromTop(58)); reset.setBounds(a.removeFromTop(36).removeFromLeft(190));
}
}
