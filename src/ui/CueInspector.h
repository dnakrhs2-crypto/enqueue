#pragma once

#include "app/AppSettings.h"
#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include "model/CueList.h"
#include "ui/CurveEditor.h"
#include "ui/LevelMatrixComponent.h"
#include "ui/PluginWindows.h"
#include "ui/TimeLoopsPanel.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace gocue
{

/** Bottom panel with tabs for the selected cue: 기본 (number, name, colour, file, waits, continue mode,
    hotkey, flags, stop fade, gain, notes) / 시간·루프 (waveform, trim, loops, envelope) /
    레벨 (patch, level matrix) / 트림 (fixed offsets) /
    트리거 (second trigger, wall clock, fade-stop-others, duck) / 이펙트 (VST3 insert chain).
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
    /** Show mode: everything read-only. */
    void setEditable (bool editable);
    /** Jumps to the 기본 tab and focuses the notes field. */
    void showNotes();
    /** Jumps to the 시간·루프 tab (or the 페이드 tab of a fade cue). */
    void showTimeTab();
    /** Fade cue: copies the target's current levels into the fade's goals (Ctrl+Shift+T). */
    void fetchFadeLevelsFromTarget();

    std::function<void()> onOpenPluginManager;
    /** Esc inside a text field: the edit is cancelled and this fires (wired to "stop all"). */
    std::function<void()> onPanic;
    std::function<void()> onPreview;
    std::function<void()> onResetCue;

    void resized() override;
    void paint (juce::Graphics& g) override;

    class BasicsPanel;
    class LevelsPanel;
    class TrimPanel;
    class TriggersPanel;
    class EffectsPanel;
    class FadePanel;
    class CurvePanel;
    class FadeParamsPanel;

private:
    void refresh();
    /** Installs the tab set for the selected cue's type (audio / fade). */
    void rebuildTabs (bool forFade);

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
    std::unique_ptr<BasicsPanel> basicsPanel;
    std::unique_ptr<TimeLoopsPanel> timeLoopsPanel;
    std::unique_ptr<LevelsPanel> levelsPanel;
    std::unique_ptr<TrimPanel> trimPanel;
    std::unique_ptr<TriggersPanel> triggersPanel;
    std::unique_ptr<EffectsPanel> effectsPanel;
    std::unique_ptr<FadePanel> fadePanel;
    std::unique_ptr<CurvePanel> curvePanel;
    std::unique_ptr<FadeParamsPanel> fadeParamsPanel;
    BasicsPanel* basics = nullptr;       // aliases of the panels above
    TimeLoopsPanel* timeLoops = nullptr;
    LevelsPanel* levels = nullptr;
    TrimPanel* trim = nullptr;
    TriggersPanel* triggers = nullptr;
    EffectsPanel* effects = nullptr;
    int tabSet = -1;                     // 0 = audio, 1 = fade
    bool editable = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueInspector)
};

} // namespace gocue
