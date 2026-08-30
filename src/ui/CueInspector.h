#pragma once

#include "app/AppSettings.h"
#include "model/CueList.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace gocue
{

/** Bottom panel: edits the standby cue (name, file, fades, gain; plugin slots later). */
class CueInspector : public juce::Component,
                     private CueList::Listener
{
public:
    CueInspector (CueList& cues, juce::AudioFormatManager& formats, AppSettings& settings);
    ~CueInspector() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void refresh();
    void commitName();
    void commitFade (juce::TextEditor& editor, bool isFadeIn);
    void commitGain();
    void chooseFile();
    void setupNumberEditor (juce::TextEditor& editor);

    void cueSelectionChanged (int) override { refresh(); }
    void cueChanged (int index) override;
    void cueListStructureChanged() override { refresh(); }

    CueList& cues;
    juce::AudioFormatManager& formats;
    AppSettings& settings;

    juce::Label title, nameLabel, fileLabel, fadeInLabel, fadeOutLabel, gainLabel, pluginsLabel, pluginsPlaceholder;
    juce::TextEditor nameEditor, fadeInEditor, fadeOutEditor;
    juce::Label filePathLabel;
    juce::TextButton browseButton;
    juce::Slider gainSlider;
    std::unique_ptr<juce::FileChooser> chooser;
    bool refreshing = false;
};

} // namespace gocue
