#pragma once
#include "RecorderLookAndFeel.h"
#include "ShortcutKeys.h"
#include "app/RecorderSettings.h"

namespace gocue::recorder
{
class ShortcutSettingsPanel : public juce::Component
{
public:
    explicit ShortcutSettingsPanel(const UserSettings&);
    UserSettings read(UserSettings s) const { s.shortcuts = bindings; return s; }
    juce::Result validate() const { return bindings.validate(); }
    void resized() override;
private:
    class Capture : public juce::TextButton
    {
    public:
        std::function<bool(const juce::KeyPress&)> onKey;
        bool keyPressed(const juce::KeyPress& key) override
        { return onKey && onKey(key) ? true : juce::TextButton::keyPressed(key); }
    };
    void update();
    RecorderShortcuts bindings;
    std::array<juce::Label, RecorderShortcuts::count> labels;
    std::array<Capture, RecorderShortcuts::count> captures;
    juce::Label hint, error;
    juce::TextButton reset {ko("기본값으로 초기화")};
    int capturing = -1;
};
}
