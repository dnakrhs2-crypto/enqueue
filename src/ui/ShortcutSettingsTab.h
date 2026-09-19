#pragma once
#include "ui/KeyCapture.h"
#include "app/ProjectDocument.h"

namespace gocue
{
/** Pure presentation calculations, shared by the table and import preview. */
namespace ShortcutSettingsModel
{
enum class Category { all, playback, cue, edit, view, file, settings };
enum class Status { all, changed, unassigned, conflict, limited };
struct Row
{
    juce::String id, name, description, scope;
    Category category = Category::all;
    ShortcutKeys keys;
    bool changed = false;
    juce::StringArray conflicts, limitations;
};
struct Issues { juce::StringArray conflicts, limitations; juce::String commandOwner; };
using Cues = std::vector<ShortcutKeyContext::CueHotkey>;
Issues inspect (const ShortcutService&, const juce::String& action, const juce::KeyPress&, const Cues&);
bool moveRemovesLastPanic (const ShortcutService&, const juce::String& destination, const ShortcutKeys& movingKeys);
std::vector<Row> rows (const ShortcutService&, const Cues&);
std::vector<Row> filter (std::vector<Row>, const juce::String& search, Category, Status);
juce::String compactKeys (const ShortcutKeys&, int width, const std::function<int (const juce::String&)>& measure);
struct ImportPreview
{
    bool applicable = false, removesLastPanic = false;
    juce::String error;
    juce::StringArray changes, unassigned, conflicts, limitations;
    juce::String text() const;
};
ImportPreview previewImport (const ShortcutService&, const juce::String& xml, const Cues&);
}

class ShortcutSettingsTab : public juce::Component,
                            private juce::ListBoxModel,
                            private ShortcutService::Listener,
                            private ProjectDocument::Listener
{
public:
    ShortcutSettingsTab (ShortcutService&, ProjectDocument&);
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
    void registerKey (const juce::KeyPress&);
    void removeKey();
    void restoreSelected();
    void importFile();
    void exportFile();
    void report (const ShortcutOperationResult&);
    void confirm (const juce::String&, const juce::String&, const juce::String&, std::function<void()>, bool editsMapping = true);
    void confirmPanicRemoval (std::function<void()>);
    void setDetail (const juce::String&);

    ShortcutService& service;
    ProjectDocument& document;
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
    uint64_t operation = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<juce::Component::SafePointer<juce::AlertWindow>> dialogs;
};
}
