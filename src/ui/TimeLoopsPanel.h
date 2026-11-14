#pragma once

#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include "ui/GainControl.h"
#include "ui/PlaybackFields.h"
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
    /** Commits pending input before show mode locks the panel. */
    void setEditable (bool editable);
    /** Playback state of the standby cue (null when it is not playing). */
    void setPlayback (const AudioEngine::PlayingCue* playing);
    /** The gain's unit (true = %), shared with the 기본 tab: either tab's gain button switches both. */
    void bindGainUnit (juce::Value& percent);

    std::function<void()> onPreview;
    /** A click on the waveform: play the cue from that file position (a running one jumps there). */
    std::function<void (double fileSeconds)> onSeekPlay;
    std::function<void()> onReset;
    /** Return focus after cancelling an edit; panic is handled by the shortcut router/hook. */
    std::function<void()> onCancelEdit;
    std::function<void (const juce::String& message, bool isError)> onStatus;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    const Cue* selected() const { return document.cues.getSelected(); }
    /** What a running instance is told after the edit: region (trim), rate, both, or nothing (next start only). */
    using LiveApply = PlaybackFields::LiveApply;
    void updateSelected (const juce::String& name, const std::function<void (Cue&)>& mutator, const juce::String& coalesceKey = {},
                         LiveApply apply = LiveApply::regionAndRate);
    void commitSlices (const std::vector<Slice>& slices, int firstCount, bool finished);
    void importSliceMarkers();
    void commitTrim (double start, double end, bool finished);
    void commitEnvelope (const Envelope& envelope, bool finished);
    void commitGain();
    void setupToggle (juce::ToggleButton& toggle, const char* text);
    void showContextMenu (juce::Point<int> screenPosition);

    ProjectDocument& document;
    AudioEngine& engine;
    WaveformView waveform;

    PlaybackFields playback;
    juce::Label envelopeLabel, zoomLabel, sizeLabel;
    juce::ToggleButton envelopeToggle, linearToggle, lockToggle;
    juce::TextButton resetButton, zoomInButton, zoomOutButton, sizeUpButton, sizeDownButton;
    /** The cue's gain, the same value as the 기본 tab's slider: moving either moves the other, and this tab's wave
        follows at once (WaveformView draws with the cue's level). In dB or % as the gain button says. */
    GainUnitButton gainUnitButton;
    GainSlider gainSlider;
    bool refreshing = false;
    juce::uint32 lastLiveEnvelopePush = 0;   // commitEnvelope throttle

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimeLoopsPanel)
};

} // namespace gocue
