#pragma once

#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include "ui/WaveformView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{

/** Inspector tab "재생": trim, play count / loop, rate, integrated fade envelope, the waveform.
    Writes edits to the document (undoable) and pushes trim / rate to a running player. */
class TimeLoopsPanel : public juce::Component
{
public:
    TimeLoopsPanel (ProjectDocument& document, AudioEngine& engine, juce::AudioThumbnailCache& cache);
    ~TimeLoopsPanel() override;

    /** Reloads the controls from the standby cue. */
    void refresh();
    /** Playback state of the standby cue (null when it is not playing). */
    void setPlayback (const AudioEngine::PlayingCue* playing);

    std::function<void()> onPreview;
    /** A click on the waveform: play the cue from that file position (a running one jumps there). */
    std::function<void (double fileSeconds)> onSeekPlay;
    std::function<void()> onReset;
    /** Esc inside a text field: cancel the edit and fire this (wired to "stop all"). */
    std::function<void()> onPanic;
    std::function<void (const juce::String& message, bool isError)> onStatus;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    const Cue* selected() const { return document.cues.getSelected(); }
    /** What a running instance is told after the edit: region (trim), rate, both, or nothing (next start only). */
    enum class LiveApply { none, region, rate, regionAndRate, envelope };
    void updateSelected (const juce::String& name, const std::function<void (Cue&)>& mutator, const juce::String& coalesceKey = {},
                         LiveApply apply = LiveApply::regionAndRate);
    void commitSlices (const std::vector<Slice>& slices, int firstCount, bool finished);
    void importSliceMarkers();
    void pushLiveRegion();
    void commitTrim (double start, double end, bool finished);
    void commitEnvelope (const Envelope& envelope, bool finished);
    void commitStart();
    void commitEnd();
    void commitPlayCount();
    void commitRate();
    void setupEditor (juce::TextEditor& editor, const juce::String& allowed, int maxLength);
    void setupToggle (juce::ToggleButton& toggle, const char* text);
    void cancelEditAndPanic();
    void showContextMenu (juce::Point<int> screenPosition);

    ProjectDocument& document;
    AudioEngine& engine;
    WaveformView waveform;

    juce::Label startLabel, endLabel, lengthLabel, countLabel, rateLabel, envelopeLabel, zoomLabel, sizeLabel;
    juce::TextEditor startEditor, endEditor, countEditor, rateEditor;
    juce::ToggleButton infiniteToggle, envelopeToggle, linearToggle, lockToggle, pitchToggle;
    juce::TextButton resetButton, zoomInButton, zoomOutButton, sizeUpButton, sizeDownButton;
    bool refreshing = false;
    juce::uint32 lastLiveEnvelopePush = 0;   // commitEnvelope throttle
    juce::Uuid shownId = juce::Uuid::null();   // the cue the fields show (focus-lost commits go there)
    bool cancellingEdit = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimeLoopsPanel)
};

} // namespace gocue
