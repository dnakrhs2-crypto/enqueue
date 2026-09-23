#pragma once

#include "app/FadeRunner.h"
#include "app/ProjectDocument.h"
#include "app/Scheduler.h"
#include "app/WaitProgress.h"
#include "app/InputActivationTracker.h"
#include "audio/AudioEngine.h"

#include <functional>
#include <limits>
#include <map>
#include <memory>
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
    GoResult go (bool audition = false, double observedSeconds = -1.0);
    /** The GO key was released (for "require key up before the next GO"). */
    void goKeyReleased();
    /** P: pauses the target cue (the standby cue if it is playing, else the most recently started one);
        resumes it when it is already paused. Returns false when nothing is playing. */
    bool togglePause();
    /** The active-cues panel's pause / resume: a resume goes through the panic latch. */
    bool resumeCue (const juce::Uuid& id);
    void pauseCue (const juce::Uuid& id);
    /** F: fades the target cue out over its own stop fade. */
    bool fadeOutTarget();
    /** Esc: fades everything out over the panic time and cancels pending waits; a second Esc within
        doubleEscSeconds stops at once. */
    void panicAll();
    /** A keyboard gesture already classified from its event timestamp. */
    void panicAll (bool hardStop);
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
    /** Cancels the cue's scheduled starts (a pre-wait, a pending restart) and the auto-continue starts put on behind
        them; a running instance of the cue is left alone. */
    void cancelScheduledStart (const juce::Uuid& cueId);
    /** Cancels one scheduled start by its scheduler id ('WaitProgress::startId') with the follow put on for it and the
        auto-continue chain behind it - another start of the same cue, put on by another run, stays. */
    void cancelStart (int startId);
    /** The active cues panel's × on a waiting card, by what the card showed: a post-wait = that one scheduled start of the
        next cue goes (cancelStart), a run of that cue started on its own stays; a wait cue's wait is stopped (stopCue) - as
        the current child of a running playlist it takes the playlist with it (the list cannot go on without it); the pre-wait
        of the start a playlist put on for its current child does the same, any other pre-wait is that one start (cancelStart). */
    void cancelWait (const juce::Uuid& cueId, WaitProgress::Kind kind, int startId);
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
    /** What cancelPendingFor() takes of a cue's run: everything; all but its observers (a playlist moving to its next child
        ends the child's watch only, the group's own follow stays); or only its scheduled starts (a doubled start goes, the
        run - its playlist steps, its follow - stays). A duck restore is not pending here: it runs when the cue is over. */
    enum class Cancel { all, keepObservers, startsOnly };
    /** Cancels the pending starts / steps / follows that belong to one cue's run. Entries tagged with 'keepRunId' (the
        scheduler id of the start they were put on for: that run's follow) are left alone; 0 = keep nothing. */
    void cancelPendingFor (const juce::Uuid& cueId, Cancel scope = Cancel::all, int keepRunId = 0);
    /** Number of scheduled starts / follows still pending (tests). */
    int getNumPending() const;
    /** A scheduled start or a playlist step of this cue's run is still pending. The observer watches (an auto-follow
        waiting for the cue to end, a duck restore) count only when asked ('includeObservers'): a group's own
        observers would otherwise keep the group "active" forever and could never fire - but a child's pending follow
        does mean the group is still going. */
    bool hasPendingFor (const juce::Uuid& cueId, bool includeObservers = false) const;
    /** Scheduled starts / follows, running wait cues or playlist groups: something would still start. */
    bool hasPendingStarts() const noexcept { return ! pending.empty() || ! waits.empty() || ! playlists.empty(); }
    /** The waits running right now, for the UI: pre-waits counting down to a scheduled start, an auto-continue cue's
        post-wait, a wait cue's wait. A start put on behind an auto-continue cue shows as that cue's post-wait until the
        post-wait is over, then as its own pre-wait (when it has one). In the order they were put on. */
    std::vector<WaitProgress> getRunningWaits() const;
    /** Cues that have been started at least once since the last reset (drives the "second colour"). */
    bool hasPlayed (const juce::Uuid& cueId) const { return played.count (cueId) != 0; }
    void clearPlayed() { played.clear(); }

    /** A key that is not a command shortcut: fires the cues whose hotkey matches (with their sequences,
        without moving the playhead). Returns true when a cue took it. */
    bool handleHotkey (const juce::KeyPress& key);
    bool triggerCueById (const juce::Uuid&, const InputInvocation&);
    /** An auto-repeat of a held hotkey: swallowed when the key is a cue hotkey (true), else passed on (false). */
    bool handleHotkeyRepeat (const juce::KeyPress& key) const;
    /** Fires the cues whose wall-clock trigger matches 'now' (once per matching second). Call ~30x per second. */
    void checkWallClock (juce::Time now);
    /** fireSequence() for a cue wherever it lives (hotkeys, wall clocks): the list and index are looked up now. */
    void fireSequenceById (const juce::Uuid& id, bool audition = false);
    /** A check that comes this late still examines every second since the previous one (a busy message thread must
        not skip 12:00:00); a longer gap (sleep, a clock change) examines only the current second. */
    static constexpr double wallClockCatchUpSeconds = 60.0;
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
        fadeMs: 0 = at once, < 0 = each child's own stop fade, > 0 = that fade. 'previousRunOnly' (a restart): the
        group's own entries that belong to the run starting right now - its follow - stay (see cancelPreviousRun). */
    void stopGroup (const juce::Uuid& groupId, int fadeMs, bool previousRunOnly = false);
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
    /** A start-first-enter group's destination, from its actual child walk or a prediction before its pre-wait. */
    int groupEnterDestination (const CueList& list, int index, int childAfter = -1) const;
    GoResult trigger (const Cue& cue, bool audition, int* groupEnterIndex);
    juce::Uuid resolveTarget (bool ignoreFadingOut) const;
    void status (const juce::String& message, bool isError = false);
    static juce::String cueLabel (int index, const Cue& cue);
    /** Fires a cue by id at once (it may have been edited since it was scheduled). */
    GoResult startById (const juce::Uuid& id, bool audition, int* groupEnterIndex = nullptr);
    /** The moments a scheduled start's waits began, for getRunningWaits(): 'preWaitFrom' = when the cue's own pre-wait
        began (the GO, the group start, the end of the previous post-wait); < 0 = at the start itself (no pre-wait shown).
        'postWaitOwner' / 'postWaitFrom' = the auto-continue cue whose post-wait runs before this start, and when it started. */
    struct StartTiming
    {
        double preWaitFrom = -1.0;
        juce::Uuid postWaitOwner = juce::Uuid::null();
        double postWaitFrom = -1.0;
        int afterStartId = 0;   // the scheduler id of that cue's own scheduled start (0 = it started at once): the chain behind a cancelled start goes with it
    };
    /** The result of an immediate start (atSeconds is now or past); a scheduled one is 'started'. 'scheduledId' receives the
        scheduler id of the start (0 when it ran at once): what a walk puts on for that run is tagged with it. */
    GoResult scheduleStart (const juce::Uuid& id, double atSeconds, bool audition, int* scheduledId = nullptr, StartTiming timing = {},
                            int* groupEnterIndex = nullptr);
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
    /** What a pending entry is to its owner's run: a scheduled start, a playlist step (the watch that moves the list on,
        the crossfade's fade-out) or an observer (a follow waiting for the run to end): see hasPendingFor(). */
    enum class PendingKind { start, step, observer };
    /** Remembers a scheduler entry as part of 'owner's run (cancelled with it). 'runId' = the scheduler id of the scheduled
        start the entry was put on for (a walk's follow behind its schedule), 0 = none: see cancelPreviousRun(). */
    void track (int schedulerId, const juce::Uuid& owner, PendingKind kind = PendingKind::start, int runId = 0);
    /** The GO window applied to a hotkey / cart click: true (and reported) when the same cue was fired inside it. */
    bool refusesDoubleFire (const juce::Uuid& cueId, const juce::String& label, double observedSeconds = -1.0);
    /** A restart (second-trigger) drops the previous run's pending entries - not those of the run that is starting right now
        (the follow a sequence walk put on right behind the scheduled start that is firing). */
    void cancelPreviousRun (const juce::Uuid& cueId);
    /** A successful fade start replaces only continuations of runs that have already started, including their
        remaining post-waits. Future starts and their continuations, and the start firing now, stay intact. */
    void cancelPreviousFadeRuns (const juce::Uuid& cueId);
    /** Cancels the auto-continue starts a sequence walk put on behind 'owner's start 'startId' (0 = an immediate start),
        and the starts behind those in turn: scheduled relative to that start, they go with it (each with its own run). */
    void cancelChainBehind (const juce::Uuid& owner, int startId);
    /** The same for every start of 'owner' at once (the cue's whole run goes) - all but the chain behind 'keepStartId'
        (a restart keeps the run that is starting now; -1 = keep none). A fired start's own entry may have been swept
        already, so the chains are found by their owner, not through that entry. */
    void cancelChainsOf (const juce::Uuid& owner, int keepStartId = -1);
    /** The observer / step entries the walk put on for 'owner's scheduled start 'startId' (its follow): a cancelled
        start takes them with it, another run's follow (a different id) stays. Nothing for 0 (no run tag). */
    void cancelRunOf (const juce::Uuid& owner, int startId);
    /** The contributions (duck cue -> dB) the running duck cues put on a cue that starts now. */
    std::map<juce::Uuid, double> ducksFor (const juce::Uuid& cueId) const;
    /** Takes a duck cue's contributions off everyone (over its duck time) and forgets it. */
    void releaseDuck (const juce::Uuid& id);
    void clearDucks();
    void playlistStep (const juce::Uuid& groupId);
    double remainingSecondsOf (const juce::Uuid& id) const;

    AudioEngine& engine;
    ProjectDocument& document;
    Scheduler& scheduler;
    FadeRunner fadeRunner;
    GoResult triggerImpl (const Cue& cue, bool audition, int* groupEnterIndex);
    GoResult firstTriggerResult = GoResult::started;
    bool firstTriggerSeen = true;
    struct Pending
    {
        int id; juce::Uuid owner; PendingKind kind = PendingKind::start; int runId = 0;
        // a start's timing for getRunningWaits(): when it fires, when its cue's pre-wait began, the post-wait it waits out
        double at = 0.0, preWaitFrom = 0.0, postWaitFrom = -1.0;
        juce::Uuid postWaitOwner = juce::Uuid::null();
        int afterStartId = 0;   // the post-wait owner's scheduled start this start was put on behind (0 = an immediate start): see cancelChainBehind()
        bool audition = false;
    };
    std::vector<Pending> pending;
    GoResult triggerControl (const Cue& cue, int index, bool audition);
    bool recording = false;
    double recordingStart = 0.0;
    std::vector<RecordedStart> recorded;
    struct WaitRun { double startedAt = 0.0, endsAt = 0.0; };
    std::map<juce::Uuid, WaitRun> waits;                           // wait cues: id -> the running wait
    struct PlaylistRun
    {
        std::vector<juce::Uuid> order; int position = 0; bool audition = false; juce::Uuid current = juce::Uuid::null(); int failures = 0;
        int currentStartId = 0;   // the scheduled start of 'current' this run put on (0 = it started at once): cancelling that one start stops the list
    };
    std::map<juce::Uuid, PlaylistRun> playlists;                  // running playlist groups
    std::map<juce::Uuid, std::set<juce::Uuid>> randomUsed;        // random groups: children played this round
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
    /** A duck cue that runs now: what it puts on whoever plays meanwhile, whom it spares, and the watch that releases it. */
    struct ActiveDuck { double levelDb = 0.0; double seconds = 0.0; std::set<juce::Uuid> spare; int watchId = -1; };
    std::map<juce::Uuid, ActiveDuck> activeDucks;
    std::map<juce::Uuid, double> lastFireTimes;                  // hotkey / cart: cue -> when it last fired (the GO window on them)
    int firingStartId = std::numeric_limits<int>::max();         // the scheduled start being carried out right now (its scheduler id) ...
    juce::Uuid firingStartCue = juce::Uuid::null();              // ... and its cue: see cancelPreviousRun()
    std::map<juce::Uuid, juce::int64> wallClockFired;            // cue -> the epoch second its clock last fired (a clock set back must not fire it again)
    std::set<juce::Uuid> played;
    juce::int64 lastWallClockSecond = -1;
    double lastGoTime = -1.0e9;
    double lastPanicTime = -1.0e9;
    double panicLatchUntil = -1.0e9;   // until then every start (GO, hotkey, wall clock, auto-continue) is refused
    juce::Uuid startNextAtLevelFor = juce::Uuid::null();   // a fade-in cue starts this target at startNextLevelDb (nothing else)
    double startNextLevelDb = 0.0;
    bool goKeyDown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueController)
};

} // namespace gocue
