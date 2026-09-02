#pragma once

#include "audio/AudioEngine.h"
#include "model/CueList.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace gocue
{

/** A cart: the active list's cues laid out as a grid of buttons (slot k = cue k). A click fires the cue
    (its second-trigger rule applies while it runs), a running button is tinted with a progress bar, and audio
    files dropped on a slot become cues there. Carts have no playhead and no sequences (QLab cue carts). */
class CueCartView : public juce::Component,
                    private juce::FileDragAndDropTarget,
                    private CueList::Listener
{
public:
    CueCartView (CueList& cues, juce::AudioFormatManager& formats);
    ~CueCartView() override;

    void setGrid (int rows, int cols);
    void setPlayingCues (std::vector<AudioEngine::PlayingCue> playing);
    void setEditable (bool shouldBeEditable);

    /** A button was clicked: fire the cue. */
    std::function<void (const Cue& cue)> onTrigger;
    /** Files dropped on slot 'slotIndex' (the list index they should be inserted at). */
    std::function<void (const juce::StringArray& files, int slotIndex)> onFilesDropped;
    /** Right-click on a running cue: stop it. */
    std::function<void (const juce::Uuid& id)> onStop;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    void cueListStructureChanged() override { repaint(); }
    void cueChanged (int) override { repaint(); }
    void cueSelectionChanged (int) override { repaint(); }

    juce::Rectangle<int> cellBounds (int slot) const;
    int slotAt (juce::Point<int> p) const;
    const AudioEngine::PlayingCue* findPlaying (const juce::Uuid& id) const;

    CueList& cues;
    juce::AudioFormatManager& formats;
    std::vector<AudioEngine::PlayingCue> playing;
    int rows = 4, cols = 4;
    int pressedSlot = -1;
    int dropSlot = -1;
    bool editable = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueCartView)
};

} // namespace gocue
