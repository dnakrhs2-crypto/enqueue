#pragma once

#include "app/AppSettings.h"
#include "audio/AudioEngine.h"
#include "model/CueList.h"
#include "ui/PluginChainComponent.h"
#include "ui/PluginWindows.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace gocue
{

/** Bottom panel: edits the standby cue (name, file, fades, gain) and shows its VST3 insert chain. */
class CueInspector : public juce::Component,
                     private CueList::Listener
{
public:
    CueInspector (CueList& cues, AudioEngine& engine, AppSettings& settings, PluginWindowManager& windows);
    ~CueInspector() override;

    /** Rebuilds the plugin strip (after a project load / duplicate). */
    void refreshPlugins();
    /** Forwarded chain-change notifications (keeps the strip in sync with edits made elsewhere). */
    void pluginChainChanged (PluginChain* chain);

    std::function<void()> onOpenPluginManager;
    /** Esc inside a text field: the edit is cancelled and this fires (wired to "stop all"). */
    std::function<void()> onPanic;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void refresh();
    void commitName();
    void commitFade (juce::TextEditor& editor, bool isFadeIn);
    void commitGain();
    void chooseFile();
    void setupNumberEditor (juce::TextEditor& editor);
    void cancelEditAndPanic();

    void cueSelectionChanged (int) override { refresh(); }
    void cueChanged (int index) override;
    void cueListStructureChanged() override { refresh(); }

    CueList& cues;
    AudioEngine& engine;
    AppSettings& settings;

    juce::Label title, nameLabel, fileLabel, fadeInLabel, fadeOutLabel, gainLabel, pluginsLabel;
    juce::TextEditor nameEditor, fadeInEditor, fadeOutEditor;
    juce::Label filePathLabel;
    juce::TextButton browseButton;
    juce::Slider gainSlider;
    PluginChainComponent chainStrip;
    std::unique_ptr<juce::FileChooser> chooser;
    bool refreshing = false;
    bool cancellingEdit = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueInspector)
};

} // namespace gocue
