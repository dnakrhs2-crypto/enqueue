#pragma once
#include "ui/KeyCapture.h"
#include "app/ProjectDocument.h"
#include "ui/MidiInputSettingsPanel.h"
#include "app/MidiTriggerRouter.h"

namespace gocue
{
/** Pure presentation calculations, shared by the table and import preview. */
namespace ShortcutSettingsModel
{
enum class Category { all, playback, cue, edit, view, file, settings };
enum class Status { all, changed, unassigned, conflict, limited, disconnected, waiting, unavailable, overloaded };
struct Row
{
    juce::String id, name, description, scope;
    Category category = Category::all;
    ShortcutKeys keys;
    bool changed = false;
    juce::StringArray conflicts, limitations;
    MidiTriggers midi;
    juce::String midiText;
    juce::StringArray midiStates;
};
struct Issues { juce::StringArray conflicts, limitations; juce::String commandOwner; };
using Cues = std::vector<ShortcutKeyContext::CueHotkey>;
Issues inspect (const ShortcutService&, const juce::String& action, const juce::KeyPress&, const Cues&);
bool moveRemovesLastPanic (const ShortcutService&, const juce::String& destination, const ShortcutKeys& movingKeys);
std::vector<Row> rows (const ShortcutService&, const Cues&, const std::vector<MidiInputService::Device>& = {}, const std::vector<MidiBinding>& = {}, const MidiTriggerRouter* = nullptr);
std::vector<Row> filter (std::vector<Row>, const juce::String& search, Category, Status);
juce::String compactKeys (const ShortcutKeys&, int width, const std::function<int (const juce::String&)>& measure);
juce::String compactMidi (const ShortcutService&, const MidiTriggers&, int width, const std::function<int (const juce::String&)>& measure);
struct ImportPreview
{
    bool applicable = false, removesLastPanic = false, replacesMidi = false, formatKnown = false;
    juce::String error;
    juce::StringArray changes, unassigned, conflicts, limitations;
    juce::String text() const;
};
ImportPreview previewImport (const ShortcutService&, const juce::String& xml, const Cues&, const std::vector<MidiBinding>& = {});
}

class ShortcutSettingsTab : public juce::Component,
                            private juce::ListBoxModel,
                            private ShortcutService::Listener,
                            private ProjectDocument::Listener, private juce::Timer
{
public:
    ShortcutSettingsTab (ShortcutService&, ProjectDocument&, MidiInputService* = nullptr, MidiTriggerRouter* = nullptr);
    ~ShortcutSettingsTab() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void cancelCapture();
    void visibilityChanged() override;

private:
    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override;
    juce::Component* refreshComponentForRow (int, bool, juce::Component*) override;
    void listBoxItemClicked (int, const juce::MouseEvent&) override;
    void selectedRowsChanged (int) override;
    juce::String getTooltipForRow (int) override;
    void shortcutsChanged() override;
    void shortcutEditingLockChanged() override;
    void documentStateChanged() override;
    void containersChanged() override { documentStateChanged(); }
    ShortcutSettingsModel::Cues cues() const;
    void refresh();
    void showSelection();
    void learn (bool replace);
    void registerKey (const juce::KeyPress&, KeyCapture::Completion);
    void registerMidi (const MidiTrigger&, KeyCapture::Completion);
    void removeKey();
    void restoreSelected();
    void importFile();
    void exportFile (bool keyboardOnly = false);
    void report (const ShortcutOperationResult&);
    void confirm (const juce::String&, const juce::String&, const juce::String&, std::function<void()>, bool editsMapping = true, std::function<void()> cancelled = {});
    void confirmPanicRemoval (std::function<void()>);
    void setDetail (const juce::String&);
    void timerCallback() override;
    std::vector<MidiBinding> midiCues() const;

    ShortcutService& service;
    ProjectDocument& document;
    MidiInputService* midiInput;
    MidiTriggerRouter* midiRouter;
    MidiInputSettingsPanel inputPanel;
    juce::Label notice, header, detail;
    juce::TextEditor search;
    juce::ComboBox category, status, selectedKey;
    juce::ListBox table;
    juce::Viewport detailView;
    KeyCapture capture;
    juce::TextButton replaceButton, deleteButton, restoreButton, importButton, exportButton, resetButton;
    std::vector<ShortcutSettingsModel::Row> visibleRows;
    juce::String selectedID, captureID;
    int replaceIndex = -1;
    juce::KeyPress replacedKey;
    std::optional<MidiTrigger> replacedMidi;
    bool committingCapture = false;
    juce::String deviceSignature;
    uint64_t operation = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<juce::Component::SafePointer<juce::AlertWindow>> dialogs;
};
}
