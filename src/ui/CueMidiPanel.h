#pragma once
#include "ui/KeyCapture.h"
#include "app/MidiTriggerRouter.h"

namespace gocue
{
/** The inspector's MIDI list and shared input-add entry point. Project edits are undoable. */
class CueMidiPanel : public juce::Component, private ProjectDocument::Listener,
                     private ShortcutService::Listener, private juce::Timer
{
public:
    CueMidiPanel (ProjectDocument&, ShortcutService&, MidiInputService* = nullptr, MidiTriggerRouter* = nullptr);
    ~CueMidiPanel() override;
    void setCue (const juce::Uuid&);
    void cancelCapture();
    void resized() override;
    void enablementChanged() override { if (! isEnabled()) cancelCapture(); refresh(); }
    void visibilityChanged() override { if (! isShowing()) cancelCapture(); }
    juce::Result apply (int index, MidiTrigger); // -1 appends; also shared by inspector tests
    juce::Result remove (int index);
    MidiTriggers triggers() const;
    juce::String statusFor (const MidiTrigger&) const;
    std::function<juce::String (const juce::KeyPress&)> validateKey;
    std::function<void (const juce::String&)> onHotkeyChanged;
private:
    const Cue* selectedCue() const;
    void refresh();
    void documentStateChanged() override { refresh(); }
    void containersChanged() override { refresh(); }
    void shortcutsChanged() override { refresh(); }
    void shortcutEditingLockChanged() override { cancelCapture(); refresh(); }
    void timerCallback() override;
    KeyCapture::Decision validate (const MidiTrigger&) const;
    ProjectDocument& document;
    ShortcutService& service;
    MidiInputService* input;
    MidiTriggerRouter* router;
    juce::Uuid cueID = juce::Uuid::null();
    juce::ComboBox list;
    juce::Label status;
    KeyCaptureButton add, change;
    juce::TextButton erase;
    int editingIndex = -1;
    std::optional<MidiTrigger> original;
};
}
