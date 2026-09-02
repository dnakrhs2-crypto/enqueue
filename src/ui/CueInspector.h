#pragma once

#include "app/AppSettings.h"
#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include "model/CueList.h"
#include "ui/PluginWindows.h"
#include "ui/TimeLoopsPanel.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace gocue
{

/** Bottom panel with tabs for the standby cue: 기본 (name, file, stop fade, gain) /
    시간·루프 (waveform, trim, loops, envelope) / 이펙트 (VST3 insert chain).
    Every edit goes through ProjectDocument::perform so it is undoable. */
class CueInspector : public juce::Component,
                     private CueList::Listener
{
public:
    CueInspector (ProjectDocument& document, AudioEngine& engine, AppSettings& settings, PluginWindowManager& windows);
    ~CueInspector() override;

    /** Rebuilds the plugin strip (after a project load / duplicate / undo). */
    void refreshPlugins();
    /** Forwarded chain-change notifications (keeps the strip in sync with edits made elsewhere). */
    void pluginChainChanged (PluginChain* chain);
    /** Live playback state from the UI timer (drives the waveform playhead). */
    void setPlayback (const std::vector<AudioEngine::PlayingCue>& playing);

    std::function<void()> onOpenPluginManager;
    /** Esc inside a text field: the edit is cancelled and this fires (wired to "stop all"). */
    std::function<void()> onPanic;
    std::function<void()> onPreview;
    std::function<void()> onResetCue;

    void resized() override;
    void paint (juce::Graphics& g) override;

    class BasicsPanel;
    class TriggersPanel;
    class EffectsPanel;

private:
    void refresh();

    void cueSelectionChanged (int) override { refresh(); }
    void cueChanged (int index) override;
    void cueListStructureChanged() override { refresh(); }

    ProjectDocument& document;
    CueList& cues;
    AudioEngine& engine;
    AppSettings& settings;
    juce::AudioThumbnailCache thumbnailCache { 64 };

    juce::Label title;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    BasicsPanel* basics = nullptr;       // owned by 'tabs'
    TimeLoopsPanel* timeLoops = nullptr; // owned by 'tabs'
    TriggersPanel* triggers = nullptr;   // owned by 'tabs'
    EffectsPanel* effects = nullptr;     // owned by 'tabs'

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueInspector)
};

} // namespace gocue
