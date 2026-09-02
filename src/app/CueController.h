#pragma once

#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"

#include <functional>

namespace gocue
{

/** Show-control logic on top of the engine: GO / pause / fades / panic, applying the workspace
    settings (double-GO protection, key-up, panic time) and each cue's second-trigger rule.
    Message thread only. The UI is a thin layer over this so the rules are unit-testable. */
class CueController
{
public:
    CueController (AudioEngine& engine, ProjectDocument& document);

    enum class GoResult
    {
        started,          // a cue was fired
        resumed,          // paused cues were resumed instead
        ignored,          // the cue's second-trigger rule swallowed the GO (or acted on the running instance)
        rejectedDoubleGo, // within the minimum time between GOs
        rejectedKeyUp,    // the GO key has not been released yet
        failed,           // the cue could not be played (missing file ...)
        nothingSelected
    };

    /** Space: resumes paused cues if there are any; otherwise fires the standby cue and moves the selection on. */
    GoResult go();
    /** The GO key was released (for "require key up before the next GO"). */
    void goKeyReleased();
    /** P: pauses the target cue (the standby cue if it is playing, else the most recently started one);
        resumes it when it is already paused. Returns false when nothing is playing. */
    bool togglePause();
    /** F: fades the target cue out over its own stop fade. */
    bool fadeOutTarget();
    /** Esc: fades everything out over the panic time; a second Esc within doubleEscSeconds stops at once. */
    void panicAll();
    void hardStopAll();
    /** V: fires the standby cue without moving the selection (second-trigger rule applies). */
    GoResult preview();
    /** Stops the standby cue. */
    void resetSelected();
    /** Hard-stops everything and puts the selection on the first cue. */
    void resetAll();
    /** Fires one cue, applying its second-trigger rule when it is already running. */
    GoResult trigger (const Cue& cue);

    /** True while double-GO protection would refuse a GO (drives the red border on the GO button). */
    bool isGoLocked() const;

    std::function<void (const juce::String& message, bool isError)> onStatus;
    std::function<void()> onGoRejected;
    /** Seconds clock; tests inject a fake one. */
    std::function<double()> clock;

    static constexpr double doubleEscSeconds = 0.5;

private:
    juce::Uuid resolveTarget (bool ignoreFadingOut) const;
    void status (const juce::String& message, bool isError = false);
    static juce::String cueLabel (int index, const Cue& cue);

    AudioEngine& engine;
    ProjectDocument& document;
    double lastGoTime = -1.0e9;
    double lastPanicTime = -1.0e9;
    bool goKeyDown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueController)
};

} // namespace gocue
