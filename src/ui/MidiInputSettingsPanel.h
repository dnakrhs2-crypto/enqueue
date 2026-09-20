#pragma once
#include "app/MidiInputService.h"

namespace gocue
{
/** PC-only device selection; mapping imports never write these controls. */
class MidiInputSettingsPanel : public juce::Component, private juce::ListBoxModel, private juce::Timer
{
public:
    MidiInputSettingsPanel (ShortcutService&, MidiInputService* = nullptr);
    ~MidiInputSettingsPanel() override;
    std::function<void()> onHeightChanged;
    int preferredHeight() const { return expanded ? 188 : 28; }
    void refresh();
    void resized() override;
    static std::vector<MidiInputService::Device> snapshot (const ShortcutService&, const MidiInputService*);
private:
    int getNumRows() override { return static_cast<int> (devices.size()); }
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int, bool, juce::Component*) override;
    void timerCallback() override { refresh(); }
    void reconnect (const juce::String&);
    void report (const ShortcutOperationResult&);
    ShortcutService& service;
    MidiInputService* input;
    juce::TextButton fold, refreshButton;
    juce::ToggleButton automatic, background;
    juce::Label hint;
    juce::ListBox list;
    std::vector<MidiInputService::Device> devices;
    bool expanded = false;
    juce::String error;
};
}
