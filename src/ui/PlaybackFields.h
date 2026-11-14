#pragma once

#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>

namespace gocue
{
/** The basic and play tabs each own these controls; their panels own the layout.
    Input, undo, selection changes and live playback use the same rules on both tabs. */
class PlaybackFields
{
    class CommitEditor final : public juce::TextEditor
    {
    public:
        void syncText (const juce::String& text) { setText (text, false); syncedText = text; }
        bool takePendingEdit()
        {
            const bool changed = getText() != syncedText;
            syncedText = getText();
            return changed;
        }

    private:
        void focusLost (FocusChangeType cause) override
        {
            // Commit before the row click selects another cue. JUCE also queues a callback;
            // syncedText and the rounded display comparisons make that second call a no-op.
            if (onFocusLost) onFocusLost();
            juce::TextEditor::focusLost (cause);
        }
        juce::String syncedText;
    };

public:
    PlaybackFields (ProjectDocument&, AudioEngine&);
    void addTo (juce::Component& parent);
    void refresh (bool editable);
    void commitPending();
    const Cue* shownCue() const;
    std::array<juce::Component*, 13> fields();
    std::function<void()> onCancelEdit;

    /** Also used by the play tab's waveform and envelope controls. */
    enum class LiveApply { none, region, rate, regionAndRate, envelope, loop };
    void updateShown (const juce::String& name, const std::function<void (Cue&)>& mutator,
                      const juce::String& coalesceKey = {}, LiveApply apply = LiveApply::regionAndRate);

    juce::Label startLabel, endLabel, lengthLabel, countLabel, rateLabel, fadeInLabel;
    CommitEditor startEditor, endEditor, countEditor, rateEditor, fadeInEditor;
    juce::ToggleButton infiniteToggle, pitchToggle;

private:
    bool canEdit() const;
    bool takeEdit (CommitEditor&);
    void cancelEdit();
    void commitStart();
    void commitEnd();
    void commitPlayCount();
    void commitRate();
    void commitFadeIn();

    ProjectDocument& document;
    AudioEngine& engine;
    juce::Uuid shownId = juce::Uuid::null();
    bool editable = true;
    bool refreshing = false;
    bool cancellingEdit = false;
    bool committingPending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackFields)
};
}
