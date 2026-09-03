#pragma once

#include "audio/AudioEngine.h"
#include "model/CueList.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace gocue
{

/** Right sidebar (QLab "Active Cues"): one row per running cue with pause / resume, number and name,
    elapsed / remaining time, a scrubbable progress bar and a panic button. */
class ActiveCuesPanel : public juce::Component
{
public:
    ActiveCuesPanel (AudioEngine& engine, CueList& cues);
    ~ActiveCuesPanel() override;

    /** Fed from the UI timer. */
    void setPlayingCues (const std::vector<AudioEngine::PlayingCue>& playing);
    /** The row's stop button: the owner stops the cue wherever it runs (fade cues live outside the engine). */
    std::function<void (const juce::Uuid& cueId)> onStopRequested;
    void setNewestFirst (bool newestFirst);
    /** Off in show mode: clicking a progress bar must not seek a running cue. */
    void setScrubEnabled (bool enabled) noexcept { scrubEnabled = enabled; }
    bool isScrubEnabled() const noexcept { return scrubEnabled; }

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class Row;

    AudioEngine& engine;
    CueList& cues;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label title, emptyLabel;
    bool newestFirst = false;
    bool scrubEnabled = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ActiveCuesPanel)
};

} // namespace gocue
