#pragma once

#include "app/FadeRunner.h"
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
        heads) and moves the playhead past the sequence. 'audition' (Alt+Space) plays the way the workspace
        audition setting says: unchanged / no output / an alternate patch. */
    GoResult go (bool audition = false);
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
    /** V: fires the selected cue alone (no pre-wait, no sequence) without moving the playhead. Alt+V auditions. */
    GoResult preview (bool audition = false);
    /** Preview starting 'regionSeconds' into the selected cue's region (a click on the waveform). */
    GoResult previewFrom (double regionSeconds);
    /** Stops the selected cue. */
    void resetSelected();
    /** Stops one cue wherever it runs: its pending starts / follows, its fade (a fade cue), the fades aimed at it,
        its playlist run / children (a group), its wait, and the engine instance. Every UI stop goes through here.
        'fade' = an audio instance fades out over its stop fade instead of stopping at once. */
    void stopCue (const juce::Uuid& cueId, bool fade = false);
    /** Fires one cue the way a hotkey / cart button does: the cue alone (no pre-wait, no sequence, no playhead move),
        with its fade-stop-others and duck. */
    GoResult fire (const juce::Uuid& cueId, bool audition = false);
    /** Project switch: nothing of the old project's runs may survive (fades, revert history, playlists, waits, ducks, played). */
    void resetForNewProject();
    /** Hard-stops everything, cancels pending waits and puts the playhead on the first cue. */
    void resetAll();
    /** Fires one cue now, applying its second-trigger rule when it is already running.
        Does not apply pre-waits or continue modes. A normal GO on an auditioning cue restarts it normally. */
    GoResult trigger (const Cue& cue, bool audition = false);

    /** Result of the first trigger() since go() started its sequence (a fade / devamp cue leaves nothing playing
        or pending when it fails, so go() cannot tell from the engine alone). */
    GoResult getFirstTriggerResult() const noexcept { return firstTriggerResult; }

    /** Fires the cue at 'index' of the active list with its pre-wait and the sequence it heads (auto-continue /
        auto-follow). Returns the index of the first cue after the sequence (clamped to the list). */
    int fireSequence (int index, bool audition = false);
    /** The same on any list / cart (sequences started by hotkeys, wall clocks, follows and control cues may live
        in an inactive list). */
    int fireSequence (CueList& list, int index, bool audition);
    /** True when GO / preview audition right now (requested, or "항상 오디션" in the settings). */
    bool isAuditionRequested (bool requested) const noexcept;
    /** Index of the first cue after the sequence that starts at 'index'. */
    int sequenceEnd (int index) const;
    int sequenceEnd (const CueList& list, int index) const;
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

    /** Fade cues run here; the app starts its 100 Hz timer, tests call tick(). */
    FadeRunner& getFadeRunner() noexcept { return fadeRunner; }
    /** Running as audio (engine), as a fade, or as a group with something running / pending inside. */
    bool isCueActive (const juce::Uuid& id) const;

    /** Group cues. startGroup() fires the children the way the group mode says and returns the index the
        playhead goes to (after the group; inside it for "start first and enter"). */
    int startGroup (int index, bool audition);
    int startGroup (CueList& list, int index, bool audition);
    bool isGroupActive (int index) const;
    bool isGroupActive (const CueList& list, int index) const;
    /** Stops everything inside a group (pending starts, playlist run, running children).
        fadeMs: 0 = at once, < 0 = each child's own stop fade, > 0 = that fade. */
    void stopGroup (const juce::Uuid& groupId, int fadeMs);
    /** Playlist groups: fades the current child out and starts the next (delta 1) / previous (-1) one. */
    bool playlistSkip (const juce::Uuid& groupId, int delta);
    /** Random groups pick a child with this (0 .. count-1); tests inject a deterministic one. */
    std::function<int (int count)> randomChoice;

    /** Sequence recording: while recording, every cue that starts is remembered with the time since the recording
        began; stopRecording() hands the list over (the app turns it into a timeline group of start cues). */
    struct RecordedStart { juce::Uuid cueId; double seconds; };
    void startRecording();
    std::vector<RecordedStart> stopRecording();
    bool isRecording() const noexcept { return recording; }
    int getNumRecorded() const noexcept { return (int) recorded.size(); }

    std::function<void (const juce::String& message, bool isError)> onStatus;
    std::function<void()> onGoRejected;
    /** Seconds clock; tests inject a fake one (also used by the scheduler entries this makes). */
    std::function<double()> clock;

    static constexpr double doubleEscSeconds = 0.5;
    /** True while a panic fade (or the 0.5 s after a hard stop) runs: nothing may start. */
    bool isPanicLatched() const;

private:
    juce::Uuid resolveTarget (bool ignoreFadingOut) const;
    void status (const juce::String& message, bool isError = false);
    static juce::String cueLabel (int index, const Cue& cue);
    /** Fires a cue by id at once (it may have been edited since it was scheduled). */
    GoResult startById (const juce::Uuid& id, bool audition);
    /** False when an immediate start failed (a scheduled one is true). */
    bool scheduleStart (const juce::Uuid& id, double atSeconds, bool audition);
    AudioEngine::PlayOptions playOptions (bool audition) const;
    double startOffsetForNextPlay = 0.0;   // previewFrom(): seconds into the region the next play begins at
    bool explicitStartForNextPlay = false;
    /** The cue plus, for a group, everything inside it: spared by its own fade-stop-others / duck. */
    std::set<juce::Uuid> familyOf (const Cue& cue) const;
    void applyFadeStopOthers (const Cue& cue, const std::set<juce::Uuid>& spare);
    void applyDuck (const Cue& cue, const std::set<juce::Uuid>& spare);
    void applyPendingGoto();
    /** Recomputes and applies the ducks of every target after a contribution changed. */
    void refreshDucks (double rampSeconds);
    void track (int schedulerId, const juce::Uuid& owner);
    void playlistStep (const juce::Uuid& groupId);
    double remainingSecondsOf (const juce::Uuid& id) const;

    AudioEngine& engine;
    ProjectDocument& document;
    Scheduler& scheduler;
    FadeRunner fadeRunner;
    GoResult triggerImpl (const Cue& cue, bool audition);
    GoResult firstTriggerResult = GoResult::started;
    bool firstTriggerSeen = true;
    struct Pending { int id; juce::Uuid owner; };
    std::vector<Pending> pending;
    GoResult triggerControl (const Cue& cue, int index, bool audition);
    bool recording = false;
    double recordingStart = 0.0;
    std::vector<RecordedStart> recorded;
    std::map<juce::Uuid, double> waits;                            // wait cues: id -> end time
    struct PlaylistRun { std::vector<juce::Uuid> order; int position = 0; bool audition = false; juce::Uuid current = juce::Uuid::null(); int failures = 0; };
    std::map<juce::Uuid, PlaylistRun> playlists;                  // running playlist groups
    std::map<juce::Uuid, std::set<juce::Uuid>> randomUsed;        // random groups: children played this round
    int lastGroupEnterIndex = -1;                                 // set by startGroup() for "start first and enter"
    const CueList* lastGroupEnterList = nullptr;                  // ... and the list that index belongs to
    /** Cues being triggered right now (outermost first): a cue that starts itself, directly or through others, is refused. */
    struct Dispatch { juce::Uuid id; bool isControl; };
    std::vector<Dispatch> dispatchStack;
    int dispatchDepth = 0;                                        // trigger() and fireSequence() frames on the stack
    struct PendingGoto { bool set = false; int container = -1; juce::Uuid cueId = juce::Uuid::null(); } pendingGoto;
    bool gotoApplied = false;                                     // go(): the playhead was placed by a goto, leave it
    struct DepthGuard
    {
        explicit DepthGuard (CueController& c) : owner (c) { ++owner.dispatchDepth; }
        ~DepthGuard() { if (--owner.dispatchDepth == 0) owner.applyPendingGoto(); }
        CueController& owner;
    };
    std::map<juce::Uuid, std::map<juce::Uuid, double>> ducks;   // target -> (ducking cue -> dB)
    std::set<juce::Uuid> played;
    juce::int64 lastWallClockSecond = -1;
    double lastGoTime = -1.0e9;
    double lastPanicTime = -1.0e9;
    double panicLatchUntil = -1.0e9;   // until then every start (GO, hotkey, wall clock, auto-continue) is refused
    bool goKeyDown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueController)
};

} // namespace gocue
