#include "app/CueController.h"

namespace gocue
{

namespace
{
    juce::String ko (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

CueController::CueController (AudioEngine& e, ProjectDocument& d)
    : engine (e), document (d)
{
    clock = [] { return juce::Time::getMillisecondCounterHiRes() * 0.001; };
}

void CueController::status (const juce::String& message, bool isError)
{
    if (onStatus)
        onStatus (message, isError);
}

juce::String CueController::cueLabel (int index, const Cue& cue)
{
    return "#" + juce::String (index + 1) + " " + cue.name;
}

juce::Uuid CueController::resolveTarget (bool ignoreFadingOut) const
{
    if (const auto* selected = document.cues.getSelected())
        for (const auto& p : engine.getPlayingCues())
            if (p.id == selected->id && ! (ignoreFadingOut && p.fadingOut))
                return p.id;

    return engine.getMostRecentlyStartedCue (ignoreFadingOut);
}

bool CueController::isGoLocked() const
{
    const double window = document.settings.doubleGoSeconds;
    return window > 0.0 && clock() - lastGoTime < window;
}

//==============================================================================
CueController::GoResult CueController::trigger (const Cue& cue)
{
    const int index = document.cues.indexOf (cue.id);

    if (engine.isPlaying (cue.id))
    {
        switch (cue.secondTrigger)
        {
            case SecondTriggerAction::nothing:
                status (ko ("이미 재생 중: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::panic:
                engine.fadeOutAndStop (cue.id, (int) std::lround (document.settings.panicSeconds * 1000.0));
                status (ko ("페이드 정지: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::stop:
                engine.fadeOutAndStop (cue.id);
                status (ko ("페이드 정지: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::hardStop:
                engine.stop (cue.id);
                status (ko ("정지: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::devamp:
                engine.finishCurrentPass (cue.id);
                status (ko ("이번 반복까지만: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::hardStopRestart:
                break;   // play() restarts the running instance
        }
    }

    juce::String error;

    if (! engine.play (cue, &error))
    {
        status (error, true);
        return GoResult::failed;
    }

    return GoResult::started;
}

CueController::GoResult CueController::go()
{
    const auto& settings = document.settings;
    const double now = clock();

    if (settings.requireKeyUp && goKeyDown)
    {
        if (onGoRejected)
            onGoRejected();

        return GoResult::rejectedKeyUp;
    }

    if (settings.doubleGoSeconds > 0.0 && now - lastGoTime < settings.doubleGoSeconds)
    {
        if (onGoRejected)
            onGoRejected();

        return GoResult::rejectedDoubleGo;
    }

    goKeyDown = true;
    lastGoTime = now;

    if (! engine.getPausedCues().empty())
    {
        engine.resumeAll();
        status (ko ("재개"));
        return GoResult::resumed;
    }

    const auto* cue = document.cues.getPlayhead();

    if (cue == nullptr)
        return GoResult::nothingSelected;

    const int index = document.cues.getPlayheadIndex();
    const Cue copy = *cue;   // advancePlayhead() may invalidate the pointer
    const auto result = trigger (copy);

    if (result == GoResult::started)
        status (ko ("GO: ") + cueLabel (index, copy));

    document.cues.advancePlayhead();
    return result;
}

void CueController::goKeyReleased()
{
    goKeyDown = false;
}

CueController::GoResult CueController::preview()
{
    const auto* cue = document.cues.getSelected();

    if (cue == nullptr)
        return GoResult::nothingSelected;

    const int index = document.cues.getSelectedIndex();
    const Cue copy = *cue;
    const auto result = trigger (copy);

    if (result == GoResult::started)
        status (ko ("미리듣기: ") + cueLabel (index, copy));

    return result;
}

bool CueController::togglePause()
{
    const auto id = resolveTarget (false);

    if (id.isNull())
        return false;

    const int index = document.cues.indexOf (id);
    const juce::String label = index >= 0 ? cueLabel (index, document.cues.get (index)) : juce::String();

    if (engine.isPaused (id))
    {
        engine.resume (id);
        status (ko ("재개: ") + label);
    }
    else
    {
        engine.pause (id);
        status (ko ("일시정지: ") + label);
    }

    return true;
}

bool CueController::fadeOutTarget()
{
    const auto id = resolveTarget (true);

    if (id.isNull())
        return false;

    engine.fadeOutAndStop (id);

    if (const int index = document.cues.indexOf (id); index >= 0)
        status (ko ("페이드아웃: ") + cueLabel (index, document.cues.get (index)));

    return true;
}

void CueController::panicAll()
{
    const double now = clock();

    if (now - lastPanicTime <= doubleEscSeconds)
    {
        engine.stopAll();
        status (ko ("전체 즉시 정지"));
    }
    else
    {
        const double seconds = document.settings.panicSeconds;
        engine.fadeOutAndStopAll ((int) std::lround (seconds * 1000.0));
        status (ko ("전체 페이드 정지 (") + juce::String (seconds, 1) + ko ("초)"));
    }

    lastPanicTime = now;
}

void CueController::hardStopAll()
{
    engine.stopAll();
    status (ko ("전체 즉시 정지"));
}

void CueController::resetSelected()
{
    if (const auto* cue = document.cues.getSelected())
    {
        engine.stop (cue->id);
        status (ko ("리셋: ") + cueLabel (document.cues.getSelectedIndex(), *cue));
    }
}

void CueController::resetAll()
{
    engine.stopAll();
    document.cues.setPlayheadIndex (document.cues.isEmpty() ? -1 : 0);
    status (ko ("전체 리셋"));
}

} // namespace gocue
