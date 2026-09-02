#pragma once

#include "app/ProjectDocument.h"
#include "app/Scheduler.h"
#include "audio/AudioEngine.h"

#include <functional>
#include <map>
#include <set>
#include <vector>

namespace gocue
{

/** Show-control logic on top of the engine: GO / pause / fades / panic, cue sequences (pre-wait,
    post-wait, auto-continue, auto-follow), arming, fade-stop-others, ducking, applying the workspace
    settings (double-GO protection, key-up, panic time) and each cue's second-trigger rule.
    Message thread only. The UI is a thin layer over this so the rules are unit-testable. */
class CueController
{
public:
    CueController (AudioEngine& engine, ProjectDocument& document, Scheduler& scheduler);

    enum class GoResult
    {
        started,          // a cue (or sequence) was fired
        resumed,          // paused cues were resumed instead
        ignored,          // the cue's second-trigger rule swallowed the GO (or acted on the running instance)
        rejectedDoubleGo, // within the minimum time between GOs
        rejectedKeyUp,    // the GO key has not been released yet
        failed,           // the cue could not be played (missing file ...)
        nothingSelected
    };

    /** Space: resumes paused cues if there are any; otherwise fires the playhead cue (and the sequence it
        heads) and moves the playhead past the sequence. */
    GoResult go();
    /** The GO key was released (for "require key up before the next GO"). */
    void goKeyReleased();
    /** P: pauses the target cue (the standby cue if it is playing, else the most recently started one);
        resumes it when it is already paused. Returns false when nothing is playing. */
    bool togglePause();
    /** F: fades the target cue out over its own stop fade. */
    bool fadeOutTarget();
    /** Esc: fades everything out over the panic time and cancels pending waits; a second Esc within
        doubleEscSeconds stops at once. */
    void panicAll();
    void hardStopAll();
    /** V: fires the selected cue alone (no pre-wait, no sequence) without moving the playhead. */
    GoResult preview();
    /** Stops the selected cue. */
    void resetSelected();
    /** Hard-stops everything, cancels pending waits and puts the playhead on the first cue. */
    void resetAll();
    /** Fires one cue now, applying its second-trigger rule when it is already running.
        Does not apply pre-waits or continue modes. */
    GoResult trigger (const Cue& cue);

    /** Fires the cue at 'index' with its pre-wait and the sequence it heads (auto-continue / auto-follow).
        Returns the index of the first cue after the sequence (clamped to the list). */
    int fireSequence (int index);
    /** Index of the first cue after the sequence that starts at 'index'. */
    int sequenceEnd (int index) const;
    /** Cancels scheduled starts, follows and duck restores. */
    void cancelPending();
    /** Cancels the pending starts / follows / duck restores that belong to one cue's run. */
    void cancelPendingFor (const juce::Uuid& cueId);
    /** Number of scheduled starts / follows still pending (tests). */
    int getNumPending() const;
    bool hasPendingFor (const juce::Uuid& cueId) const;
    /** Cues that have been started at least once since the last reset (drives the "second colour"). */
    bool hasPlayed (const juce::Uuid& cueId) const { return played.count (cueId) != 0; }
    void clearPlayed() { played.clear(); }

    /** A key that is not a command shortcut: fires the cues whose hotkey matches (with their sequences,
        without moving the playhead). Returns true when a cue took it. */
    bool handleHotkey (const juce::KeyPress& key);
    /** An auto-repeat of a held hotkey: swallowed when the key is a cue hotkey (true), else passed on (false). */
    bool handleHotkeyRepeat (const juce::KeyPress& key) const;
    /** Fires the cues whose wall-clock trigger matches 'now' (once per matching second). Call ~30x per second. */
    void checkWallClock (juce::Time now);
    /** L: pre-loads the selected cue so GO starts it with no disk latency. */
    bool loadSelected (double startSeconds = 0.0);

    /** True while double-GO protection would refuse a GO (drives the red border on the GO button). */
    bool isGoLocked() const;

    std::function<void (const juce::String& message, bool isError)> onStatus;
    std::function<void()> onGoRejected;
    /** Seconds clock; tests inject a fake one (also used by the scheduler entries this makes). */
    std::function<double()> clock;

    static constexpr double doubleEscSeconds = 0.5;

private:
    juce::Uuid resolveTarget (bool ignoreFadingOut) const;
    void status (const juce::String& message, bool isError = false);
    static juce::String cueLabel (int index, const Cue& cue);
    /** Fires a cue by id at once (it may have been edited since it was scheduled). */
    void startById (const juce::Uuid& id);
    void scheduleStart (const juce::Uuid& id, double atSeconds);
    void applyFadeStopOthers (const Cue& cue);
    void applyDuck (const Cue& cue);
    /** Recomputes and applies the ducks of every target after a contribution changed. */
    void refreshDucks (double rampSeconds);
    void track (int schedulerId, const juce::Uuid& owner);

    AudioEngine& engine;
    ProjectDocument& document;
    Scheduler& scheduler;
    struct Pending { int id; juce::Uuid owner; };
    std::vector<Pending> pending;
    std::map<juce::Uuid, std::map<juce::Uuid, double>> ducks;   // target -> (ducking cue -> dB)
    std::set<juce::Uuid> played;
    juce::int64 lastWallClockSecond = -1;
    double lastGoTime = -1.0e9;
    double lastPanicTime = -1.0e9;
    bool goKeyDown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueController)
};

} // namespace gocue
