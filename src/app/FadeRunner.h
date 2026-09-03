#pragma once

#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

namespace gocue
{

/** Runs fade cues: moves the target's *running instance* (main level, level matrix, playback rate, VST3
    parameters) from where it is when the fade starts to the fade cue's goals over the fade's duration,
    along its curve. Only the fade's active cells are touched, so fades on different cells of the same
    target run side by side. The pre-fade state is remembered for "revert".
    Ticks at 100 Hz from its own timer in the app; tests inject a clock and call tick(). Message thread. */
class FadeRunner : private juce::Timer
{
public:
    FadeRunner (AudioEngine& engine, ProjectDocument& document);
    ~FadeRunner() override;

    /** Seconds clock (defaults to the wall clock). */
    std::function<double()> clock;

    struct Info
    {
        juce::Uuid fadeId = juce::Uuid::null(), targetId = juce::Uuid::null();
        double elapsedSeconds = 0.0, durationSeconds = 0.0;
    };

    /** Starts (or restarts) 'fadeCue' on its target. False with a message when there is no target or the
        target is not running. */
    bool start (const Cue& fadeCue, juce::String* error = nullptr, bool targetAutoStarted = false);
    /** Advances every running fade; finished fades are removed (stopping their target when asked). */
    void tick();
    /** Ends the fade where it is. */
    void stop (const juce::Uuid& fadeCueId);
    void stopAll();
    bool isRunning (const juce::Uuid& fadeCueId) const;
    int getNumRunning() const noexcept { return (int) fades.size(); }
    std::vector<Info> getRunning() const;

    /** Puts the most recently faded target back to its pre-fade state (if that instance is still running):
        only the lanes that fade owned (its active cells, rate, parameters) are restored. */
    bool revertLast();
    bool canRevert() const noexcept { return ! revertStack.empty(); }
    /** Project switch: drops every running fade and the revert history (the cue ids may recur in the next file). */
    void resetSession();

    void startTicking (int intervalMs = 10) { startTimer (intervalMs); }
    void stopTicking() { stopTimer(); }

    static constexpr int maxRevertStates = 20;

private:
    struct State
    {
        double mainDb = 0.0;
        LevelMatrix levels;
        TrimLevels trim;
        double rate = 1.0;
        std::vector<float> params;   // parallel to FadeCueData::params
    };

    struct Active
    {
        juce::Uuid fadeId = juce::Uuid::null(), targetId = juce::Uuid::null();
        juce::int64 instance = -1;   // the target's start order when the fade began: a restart ends the fade
        double startTime = 0.0, duration = 0.0;
        FadeCueData data;
        State from, to;
    };

    struct RevertEntry
    {
        juce::Uuid targetId = juce::Uuid::null();
        juce::int64 instance = -1;
        bool stopOnRevert = false;   // the fade-in started this instance itself: reverting stops it again
        FadeCueData data;   // which lanes the fade owned
        State from;
    };

    bool readState (const juce::Uuid& targetId, const FadeCueData& data, State& out) const;
    void writeState (const juce::Uuid& targetId, const FadeCueData& data, const State& state, bool levels, bool rate, bool params);
    void apply (Active& a, double completionTime);
    /** Takes the lanes 'winner' drives away from 'other' (a newer fade on the same cells owns them). True when
        'other' has nothing left to do. */
    static bool takeLanes (const FadeCueData& winner, FadeCueData& other);
    static bool hasAnyLane (const FadeCueData& data);
    void timerCallback() override { tick(); }

    AudioEngine& engine;
    ProjectDocument& document;
    std::vector<Active> fades;
    std::vector<RevertEntry> revertStack;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FadeRunner)
};

} // namespace gocue
