#pragma once

#include "app/WaitProgress.h"
#include "app/VolumeCue.h"
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
                         double nowSeconds = 0.0, const VolumeCue::Badges& badges = {});
    void setPlayingCount (int numPlaying, int numPaused, int numWaiting = 0);
    /** The row's stop button: the owner stops the cue wherever it runs (fade cues and waits live outside the engine). */
    std::function<void (const juce::Uuid& cueId)> onStopRequested;
    /** A waiting card's ×: cancel that wait - for a pre-wait or a wait cue the cue itself, for a post-wait card the next
        cue whose scheduled start the post-wait leads to; 'kind' says which of these the card showed. */
    std::function<void (const juce::Uuid& cueId, WaitProgress::Kind kind, int startId)> onCancelWaitRequested;
    /** Looks a cue up anywhere in the project (the panel's own list is the active one; a cue of another list runs too). */
    std::function<const Cue* (const juce::Uuid& cueId)> findCue;
    /** The row's pause button: resume = true asks for a resume (the owner applies the panic latch). */
    std::function<void (const juce::Uuid& cueId, bool resume)> onPauseRequested;
    void setNewestFirst (bool newestFirst);
    bool isNewestFirst() const noexcept { return newestFirst; }
    std::function<void()> onBigViewRequested;
    void setViewOnly (bool viewOnly);
    bool isViewOnly() const noexcept { return viewOnly; }
    int getCardCount() const noexcept { return (int) rows.size(); }
    /** Off in show mode: clicking a progress bar must not seek a running cue. */
    void setScrubEnabled (bool enabled) noexcept { scrubEnabled = enabled; }
    bool isScrubEnabled() const noexcept { return scrubEnabled && ! viewOnly; }

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
    juce::TextButton bigViewButton;
    bool newestFirst = false;
    bool scrubEnabled = true;
    bool viewOnly = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ActiveCuesPanel)
};

} // namespace gocue
