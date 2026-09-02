#pragma once

#include "audio/AudioEngine.h"
#include "model/CueList.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace gocue
{

/** The cue list table (QLab-style columns): status icons, number, name, file, pre-wait, duration,
    post-wait, continue mode. Multi-selection, a separate playhead marker, quick-edit keys
    (N Q E W C O), inline cell editing, row drag reordering, file drops, a context menu.
    Running cues are tinted (green / yellow paused / orange fading) with a progress bar. */
class CueTable : public juce::Component,
                 private juce::TableListBoxModel,
                 private juce::FileDragAndDropTarget,
                 private juce::DragAndDropTarget,
                 private CueList::Listener
{
public:
    enum ColumnId
    {
        colStatus = 1,
        colNumber,
        colName,
        colFile,
        colPreWait,
        colDuration,
        colPostWait,
        colContinue
    };

    CueTable (CueList& cues, juce::AudioFormatManager& formats, juce::ApplicationCommandManager& commands);
    ~CueTable() override;

    /** Called from a UI timer with the engine's current playback state. */
    void setPlayingCues (std::vector<AudioEngine::PlayingCue> playing);
    /** Show mode: no inline edits, drags or property changes from the table. */
    void setEditable (bool shouldBeEditable);
    bool isEditable() const noexcept { return editable; }
    /** Small / medium / large rows (0..2). */
    void setRowSize (int size);

    /** Invoked with the dropped audio files and the row index they should be inserted at. */
    std::function<void (const juce::StringArray& files, int insertIndex)> onFilesDropped;
    /** Rows dragged inside the table: move them so the block starts at insertIndex (index before removal). */
    std::function<void (const std::vector<int>& rows, int insertIndex)> onMoveRows;
    /** An undoable edit of one or more cues. */
    std::function<void (const std::vector<int>& rows, const juce::String& editName, const std::function<void (Cue&)>& mutator)> onEditCues;
    /** 'O': edit the notes of a row (in the inspector). */
    std::function<void (int row)> onEditNotes;
    /** 'D': the duration lives in the inspector's time tab. */
    std::function<void (int row)> onEditDuration;

    /** Opens the inline editor for a cell (number / name / pre-wait / post-wait). */
    void beginCellEdit (int row, ColumnId column);
    void focusTable();
    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;

private:
    class CellEditor;

    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int rowNumber, int width, int height, bool rowIsSelected) override;
    void paintCell (juce::Graphics&, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
    void cellClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
    void cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void deleteKeyPressed (int lastRowSelected) override;
    void backgroundClicked (const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rowsToDescribe) override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // DragAndDropTarget (rows moved inside the table)
    bool isInterestedInDragSource (const SourceDetails& details) override;
    void itemDragEnter (const SourceDetails& details) override;
    void itemDragMove (const SourceDetails& details) override;
    void itemDragExit (const SourceDetails& details) override;
    void itemDropped (const SourceDetails& details) override;

    // CueList::Listener
    void cueListStructureChanged() override;
    void cueChanged (int index) override;
    void cueSelectionChanged (int index) override;
    void playheadChanged (int index) override;

    const AudioEngine::PlayingCue* findPlaying (const juce::Uuid& id) const;
    void syncSelectionFromModel();
    bool handleQuickEditKey (const juce::KeyPress& key);
    void showContextMenu (int row, juce::Point<int> screenPosition);
    void cycleContinueMode (int row);
    int insertionIndexForY (int y) const;
    void commitCellEdit (int row, ColumnId column, const juce::String& text);
    std::vector<int> rowsForEdit (int row) const;

    /** juce::ListBox eats Up/Down regardless of modifiers; let Ctrl/Alt combinations reach the command shortcuts,
        and route the quick-edit letters to the owner. */
    struct TableBox : public juce::TableListBox
    {
        std::function<bool (const juce::KeyPress&)> onKey;
        bool keyPressed (const juce::KeyPress& key) override;
    };

    CueList& cues;
    juce::AudioFormatManager& formats;
    juce::ApplicationCommandManager& commands;
    TableBox table;
    std::vector<AudioEngine::PlayingCue> playing;
    std::unique_ptr<CellEditor> cellEditor;
    bool dragOver = false;
    bool rowDragOver = false;
    int rowDropIndex = -1;
    bool syncingSelection = false;
    bool editable = true;
};

} // namespace gocue
