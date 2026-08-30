#pragma once

#include "audio/AudioEngine.h"
#include "model/CueList.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace gocue
{

/** The cue list table: number, name, file, fade in, fade out, duration.
    Running cues are tinted (green / orange while fading out) with a progress bar;
    the standby cue carries a blue outline. Audio files can be dropped onto it. */
class CueTable : public juce::Component,
                 private juce::TableListBoxModel,
                 private juce::FileDragAndDropTarget,
                 private CueList::Listener
{
public:
    CueTable (CueList& cues, juce::AudioFormatManager& formats, juce::ApplicationCommandManager& commands);
    ~CueTable() override;

    /** Called from a UI timer with the engine's current playback state. */
    void setPlayingCues (std::vector<AudioEngine::PlayingCue> playing);

    /** Invoked with the dropped audio files and the row index they should be inserted at. */
    std::function<void (const juce::StringArray& files, int insertIndex)> onFilesDropped;

    void focusTable();
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    enum ColumnId
    {
        colNumber = 1,
        colName,
        colFile,
        colFadeIn,
        colFadeOut,
        colDuration
    };

    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int rowNumber, int width, int height, bool rowIsSelected) override;
    void paintCell (juce::Graphics&, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void deleteKeyPressed (int lastRowSelected) override;
    void backgroundClicked (const juce::MouseEvent&) override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // CueList::Listener
    void cueListStructureChanged() override;
    void cueChanged (int index) override;
    void cueSelectionChanged (int index) override;

    const AudioEngine::PlayingCue* findPlaying (const juce::Uuid& id) const;
    void syncSelectionFromModel();

    CueList& cues;
    juce::AudioFormatManager& formats;
    juce::ApplicationCommandManager& commands;
    juce::TableListBox table;
    std::vector<AudioEngine::PlayingCue> playing;
    bool dragOver = false;
    bool syncingSelection = false;
};

} // namespace gocue
