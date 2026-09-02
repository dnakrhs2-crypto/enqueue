#include "app/CueController.h"

#include <algorithm>

namespace gocue
{

namespace
{
    juce::String ko (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

CueController::CueController (AudioEngine& e, ProjectDocument& d, Scheduler& s)
    : engine (e), document (d), scheduler (s)
{
    clock = [this] { return scheduler.now(); };
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

void CueController::track (int schedulerId)
{
    pending.push_back (schedulerId);
}

int CueController::getNumPending() const
{
    return (int) pending.size();
}

void CueController::cancelPending()
{
    for (int id : pending)
        scheduler.cancel (id);

    pending.clear();
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

//==============================================================================
void CueController::applyFadeStopOthers (const Cue& cue)
{
    if (! cue.fadeStopOthers.enabled)
        return;

    const int ms = (int) std::lround (cue.fadeStopOthers.seconds * 1000.0);

    for (const auto& p : engine.getPlayingCues())
        if (p.id != cue.id)
            engine.fadeOutAndStop (p.id, ms);   // one list for now: peers / list / all coincide
}

void CueController::applyDuck (const Cue& cue)
{
    if (! cue.duck.enabled)
        return;

    std::vector<juce::Uuid> ducked;

    for (const auto& p : engine.getPlayingCues())
    {
        if (p.id == cue.id)
            continue;

        engine.setDuckDb (p.id, cue.duck.levelDb, cue.duck.seconds);
        ducked.push_back (p.id);
    }

    if (ducked.empty())
        return;

    const auto id = cue.id;
    const double ramp = cue.duck.seconds;

    // restore the others when this cue is over (or stopped)
    track (scheduler.watch ([this, id] { return ! engine.isPlaying (id); },
                            [this, ducked, ramp]
                            {
                                for (const auto& other : ducked)
                                    engine.setDuckDb (other, 0.0, ramp);
                            }));
}

void CueController::startById (const juce::Uuid& id)
{
    const auto* cue = document.cues.findById (id);

    if (cue == nullptr)
        return;   // deleted while it was waiting

    const Cue copy = *cue;

    if (trigger (copy) != GoResult::started)
        return;

    applyFadeStopOthers (copy);
    applyDuck (copy);
}

void CueController::scheduleStart (const juce::Uuid& id, double atSeconds)
{
    if (atSeconds <= clock())
    {
        startById (id);
        return;
    }

    track (scheduler.schedule (atSeconds, [this, id] { startById (id); }));
}

int CueController::sequenceEnd (int index) const
{
    const auto& cues = document.cues;

    while (cues.isValidIndex (index) && cues.get (index).continueMode != ContinueMode::none)
        ++index;

    return juce::jmin (index + 1, cues.size());
}

int CueController::fireSequence (int index)
{
    auto& cues = document.cues;
    double t = clock();
    int i = index;

    while (cues.isValidIndex (i))
    {
        const Cue cue = cues.get (i);   // copy: starting a cue may not change the list, but be safe

        if (! cue.armed && cue.skipIfDisarmed)
        {
            ++i;
            continue;
        }

        const double startAt = t + cue.preWaitSeconds;

        if (cue.armed)
            scheduleStart (cue.id, startAt);
        else
            status (ko ("비활성 큐 건너뜀: ") + cueLabel (i, cue));

        if (cue.continueMode == ContinueMode::none)
            return i + 1;

        if (cue.continueMode == ContinueMode::autoContinue)
        {
            t = startAt + cue.postWaitSeconds;
            ++i;
            continue;
        }

        // auto-follow: the rest of the chain starts when this cue has finished (a disarmed cue is over at once)
        const int next = i + 1;
        const auto id = cue.id;
        const bool armed = cue.armed;

        track (scheduler.watch ([this, id, startAt, armed] { return clock() >= startAt && (! armed || ! engine.isPlaying (id)); },
                                [this, next] { fireSequence (next); }));

        return sequenceEnd (index);
    }

    return juce::jmin (i, cues.size());
}

//==============================================================================
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
    const Cue copy = *cue;

    // a running cue that is fired again follows its second-trigger rule instead of starting a sequence
    if (engine.isPlaying (copy.id) && copy.secondTrigger != SecondTriggerAction::hardStopRestart)
    {
        const auto result = trigger (copy);
        document.cues.advancePlayhead();
        return result;
    }

    const int after = fireSequence (index);
    const bool anyStarted = engine.isPlaying (copy.id) || getNumPending() > 0;

    if (copy.armed && copy.preWaitSeconds <= 0.0 && ! engine.isPlaying (copy.id))
    {
        // the first cue could not be started (missing file ...): trigger() already reported it
        document.cues.setPlayheadIndex (juce::jmin (after, document.cues.size() - 1));
        return GoResult::failed;
    }

    if (anyStarted || ! copy.armed)
        status (ko ("GO: ") + cueLabel (index, copy));

    document.cues.setPlayheadIndex (juce::jmin (after, document.cues.size() - 1));
    return GoResult::started;
}

void CueController::goKeyReleased()
{
    goKeyDown = false;
}

bool CueController::handleHotkey (const juce::KeyPress& key)
{
    const auto description = key.getTextDescription();

    if (description.isEmpty())
        return false;

    bool handled = false;
    const auto& cues = document.cues;

    for (int i = 0; i < cues.size(); ++i)
    {
        const auto& cue = cues.get (i);

        if (cue.hotkey.isNotEmpty() && juce::KeyPress::createFromDescription (cue.hotkey) == key)
        {
            fireSequence (i);   // hotkeys do not move the playhead
            status (ko ("핫키: ") + cueLabel (i, cue));
            handled = true;
        }
    }

    return handled;
}

void CueController::checkWallClock (juce::Time now)
{
    const juce::int64 second = now.toMilliseconds() / 1000;

    if (second == lastWallClockSecond)
        return;

    lastWallClockSecond = second;
    const int hour = now.getHours(), minute = now.getMinutes(), sec = now.getSeconds();
    const int dayBit = 1 << now.getDayOfWeek();   // 0 = Sunday
    const auto& cues = document.cues;

    for (int i = 0; i < cues.size(); ++i)
    {
        const auto& wc = cues.get (i).wallClock;

        if (wc.enabled && wc.hour == hour && wc.minute == minute && wc.second == sec && (wc.daysMask & dayBit) != 0)
        {
            status (ko ("시간 트리거: ") + cueLabel (i, cues.get (i)));
            fireSequence (i);
        }
    }
}

bool CueController::loadSelected (double startSeconds)
{
    const auto* cue = document.cues.getSelected();

    if (cue == nullptr)
        return false;

    juce::String error;

    if (! engine.load (*cue, startSeconds, &error))
    {
        status (error, true);
        return false;
    }

    status (ko ("로드: ") + cueLabel (document.cues.getSelectedIndex(), *cue)
            + (startSeconds > 0.0 ? " @ " + juce::String (startSeconds, 2) + "s" : juce::String()));
    return true;
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
    cancelPending();

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
    cancelPending();
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
    cancelPending();
    engine.stopAll();
    document.cues.setPlayheadIndex (document.cues.isEmpty() ? -1 : 0);
    status (ko ("전체 리셋"));
}

} // namespace gocue
