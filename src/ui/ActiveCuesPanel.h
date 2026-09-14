#pragma once

#include "app/WaitProgress.h"
#include "audio/AudioEngine.h"
#include "model/CueList.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace gocue
{

/** Right sidebar (QLab "Active Cues"): one card per running cue with pause / resume, number and name,
    elapsed / remaining time, a scrubbable progress bar and a panic button - and one card per cue whose pre-wait
    (or wait cue's wait, or post-wait after its sound) counts down, with the time left, a bar and the panic button.
    A running cue's own post-wait (or the pre-wait of its restart) is a countdown pill on its card. */
class ActiveCuesPanel : public juce::Component
{
public:
    ActiveCuesPanel (AudioEngine& engine, CueList& cues);
    ~ActiveCuesPanel() override;

    /** Fed from the UI timer: the engine's instances and the waits running now ('nowSeconds' = the controller's clock
        the waits were read at). */
    void setPlayingCues (const std::vector<AudioEngine::PlayingCue>& playing, const std::vector<WaitProgress>& waits = {},
                         double nowSeconds = 0.0);
    void setPlayingCount (int numPlaying, int numPaused, int numWaiting = 0);
    /** The row's stop button: the owner stops the cue wherever it runs (fade cues and waits live outside the engine). */
    std::function<void (const juce::Uuid& cueId)> onStopRequested;
    /** The row's pause button: resume = true asks for a resume (the owner applies the panic latch). */
    std::function<void (const juce::Uuid& cueId, bool resume)> onPauseRequested;
    void setNewestFirst (bool newestFirst);
    /** Off in show mode: clicking a progress bar must not seek a running cue. */
    void setScrubEnabled (bool enabled) noexcept { scrubEnabled = enabled; }
    bool isScrubEnabled() const noexcept { return scrubEnabled; }

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class Row;

    void layoutRows();

    AudioEngine& engine;
    CueList& cues;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label title, playingLabel, emptyLabel;
    bool newestFirst = false;
    bool scrubEnabled = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ActiveCuesPanel)
};

} // namespace gocue
