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
            if (p.id == selected->id && ! p.loaded && ! (ignoreFadingOut && p.fadingOut))
                return p.id;

    return engine.getMostRecentlyStartedCue (ignoreFadingOut);
}

bool CueController::isGoLocked() const
{
    const double window = document.settings.doubleGoSeconds;
    return window > 0.0 && clock() - lastGoTime < window;
}

void CueController::track (int schedulerId, const juce::Uuid& owner)
{
    // forget entries the scheduler has already run
    pending.erase (std::remove_if (pending.begin(), pending.end(), [this] (const Pending& p) { return ! scheduler.isPending (p.id); }), pending.end());
    pending.push_back ({ schedulerId, owner });
}

int CueController::getNumPending() const
{
    int n = 0;

    for (const auto& p : pending)
        if (scheduler.isPending (p.id))
            ++n;

    return n;
}

bool CueController::hasPendingFor (const juce::Uuid& cueId) const
{
    for (const auto& p : pending)
        if (p.owner == cueId && scheduler.isPending (p.id))
            return true;

    return false;
}

void CueController::cancelPending()
{
    for (const auto& p : pending)
        scheduler.cancel (p.id);

    pending.clear();
}

void CueController::cancelPendingFor (const juce::Uuid& cueId)
{
    for (const auto& p : pending)
        if (p.owner == cueId)
            scheduler.cancel (p.id);

    pending.erase (std::remove_if (pending.begin(), pending.end(), [&] (const Pending& p) { return p.owner == cueId; }), pending.end());
}

//==============================================================================
bool CueController::isAuditionRequested (bool requested) const noexcept
{
    return requested || document.settings.alwaysAudition;
}

AudioEngine::PlayOptions CueController::playOptions (bool audition) const
{
    AudioEngine::PlayOptions options;

    if (! isAuditionRequested (audition))
        return options;

    options.audition = true;

    switch (document.settings.audition)
    {
        case WorkspaceSettings::Audition::unchanged:      break;
        case WorkspaceSettings::Audition::none:           options.silent = true; break;
        case WorkspaceSettings::Audition::alternatePatch: options.patchOverride = document.settings.auditionPatchId; break;
    }

    return options;
}

CueController::GoResult CueController::trigger (const Cue& cue, bool audition)
{
    const int index = document.cues.indexOf (cue.id);
    const bool auditionNow = isAuditionRequested (audition);

    // a normal GO on a cue that is auditioning restarts it with the real output (QLab)
    if (engine.isPlaying (cue.id) && ! (engine.isAuditioning (cue.id) && ! auditionNow))
    {
        switch (cue.secondTrigger)
        {
            case SecondTriggerAction::nothing:
                status (ko ("이미 재생 중: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::panic:
                cancelPendingFor (cue.id);
                engine.fadeOutAndStop (cue.id, (int) std::lround (document.settings.panicSeconds * 1000.0));
                status (ko ("페이드 정지: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::stop:
                cancelPendingFor (cue.id);
                engine.fadeOutAndStop (cue.id);
                status (ko ("페이드 정지: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::hardStop:
                cancelPendingFor (cue.id);
                engine.stop (cue.id);
                status (ko ("정지: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::devamp:
                engine.finishCurrentPass (cue.id);
                status (ko ("이번 반복까지만: ") + cueLabel (index, cue));
                return GoResult::ignored;

            case SecondTriggerAction::hardStopRestart:
                cancelPendingFor (cue.id);   // the previous run's follow / duck restore must not fire for the new run
                break;                       // play() restarts the running instance
        }
    }

    juce::String error;

    if (! engine.play (cue, playOptions (audition), &error))
    {
        status (error, true);
        return GoResult::failed;
    }

    played.insert (cue.id);
    return GoResult::started;
}

//==============================================================================
void CueController::applyFadeStopOthers (const Cue& cue)
{
    if (! cue.fadeStopOthers.enabled)
        return;

    const int ms = (int) std::lround (cue.fadeStopOthers.seconds * 1000.0);

    for (const auto& p : engine.getPlayingCues())
        if (p.id != cue.id && ! p.loaded)
            engine.fadeOutAndStop (p.id, ms);   // one list for now: peers / list / all coincide
}

void CueController::refreshDucks (double rampSeconds)
{
    for (auto it = ducks.begin(); it != ducks.end();)
    {
        if (! engine.isPlaying (it->first) || it->second.empty())
        {
            engine.setDuckDb (it->first, 0.0, rampSeconds);
            it = ducks.erase (it);
            continue;
        }

        // contributions add up (two -6 dB ducks = -12 dB), within the gain range
        double total = 0.0;

        for (const auto& c : it->second)
            total += c.second;

        engine.setDuckDb (it->first, juce::jlimit (Cue::minGainDb, Cue::maxGainDb, total), rampSeconds);
        ++it;
    }
}

void CueController::applyDuck (const Cue& cue)
{
    if (! cue.duck.enabled)
        return;

    bool any = false;

    for (const auto& p : engine.getPlayingCues())
    {
        if (p.id == cue.id || p.loaded)
            continue;

        ducks[p.id][cue.id] = cue.duck.levelDb;
        any = true;
    }

    if (! any)
        return;

    refreshDucks (cue.duck.seconds);
    const auto id = cue.id;
    const double ramp = cue.duck.seconds;

    // this cue's contribution ends when it is over (or stopped); the others keep theirs
    track (scheduler.watch ([this, id] { return ! engine.isPlaying (id); },
                            [this, id, ramp]
                            {
                                for (auto& target : ducks)
                                    target.second.erase (id);

                                refreshDucks (ramp);
                            }), id);
}

void CueController::startById (const juce::Uuid& id, bool audition)
{
    const auto* cue = document.cues.findById (id);

    if (cue == nullptr)
        return;   // deleted while it was waiting

    const Cue copy = *cue;

    if (trigger (copy, audition) != GoResult::started)
        return;

    applyFadeStopOthers (copy);
    applyDuck (copy);
}

void CueController::scheduleStart (const juce::Uuid& id, double atSeconds, bool audition)
{
    if (atSeconds <= clock())
    {
        startById (id, audition);
        return;
    }

    track (scheduler.schedule (atSeconds, [this, id, audition] { startById (id, audition); }), id);
}

int CueController::sequenceEnd (int index) const
{
    const auto& cues = document.cues;

    while (cues.isValidIndex (index) && cues.get (index).continueMode != ContinueMode::none)
        ++index;

    return juce::jmin (index + 1, cues.size());
}

int CueController::fireSequence (int index, bool audition)
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
            scheduleStart (cue.id, startAt, audition);
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

        // auto-follow: the rest of the chain starts when this cue has finished (a disarmed cue is over at once).
        // The next cue is remembered by id: rows may be inserted / deleted / moved meanwhile.
        const auto nextId = cues.isValidIndex (i + 1) ? cues.get (i + 1).id : juce::Uuid::null();
        const auto id = cue.id;
        const bool armed = cue.armed;

        if (! nextId.isNull())
            track (scheduler.watch ([this, id, startAt, armed] { return clock() >= startAt && (! armed || ! engine.isPlaying (id)); },
                                    [this, nextId, audition]
                                    {
                                        if (const int nextIndex = document.cues.indexOf (nextId); nextIndex >= 0)
                                            fireSequence (nextIndex, audition);
                                    }), id);

        return sequenceEnd (index);
    }

    return juce::jmin (i, cues.size());
}

//==============================================================================
CueController::GoResult CueController::go (bool audition)
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
    // (unless it is auditioning and this is a normal GO: then it restarts for real)
    if (engine.isPlaying (copy.id) && copy.secondTrigger != SecondTriggerAction::hardStopRestart
        && ! (engine.isAuditioning (copy.id) && ! isAuditionRequested (audition)))
    {
        const auto result = trigger (copy, audition);
        document.cues.advancePlayhead();
        return result;
    }

    const int after = fireSequence (index, audition);
    const bool anyStarted = engine.isPlaying (copy.id) || getNumPending() > 0;

    if (copy.armed && copy.preWaitSeconds <= 0.0 && ! engine.isPlaying (copy.id))
    {
        // the first cue could not be started (missing file ...): trigger() already reported it
        document.cues.setPlayheadIndex (juce::jmin (after, document.cues.size() - 1));
        return GoResult::failed;
    }

    if (anyStarted || ! copy.armed)
        status ((isAuditionRequested (audition) ? ko ("오디션 GO: ") : ko ("GO: ")) + cueLabel (index, copy));

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

bool CueController::handleHotkeyRepeat (const juce::KeyPress& key) const
{
    const auto description = key.getTextDescription();

    for (const auto& cue : document.cues.getAll())
        if (cue.hotkey.isNotEmpty() && juce::KeyPress::createFromDescription (cue.hotkey) == key)
            return true;

    juce::ignoreUnused (description);
    return false;
}

void CueController::checkWallClock (juce::Time now)
{
    const juce::int64 second = now.toMilliseconds() / 1000;

    if (second == lastWallClockSecond)
        return;

    // every second since the last check is examined (a busy message thread must not skip 12:00:00);
    // after a long gap (sleep, clock change) only the current second counts
    juce::int64 from = lastWallClockSecond < 0 || second - lastWallClockSecond > 5 || second < lastWallClockSecond ? second : lastWallClockSecond + 1;
    lastWallClockSecond = second;
    const auto& cues = document.cues;

    for (juce::int64 s = from; s <= second; ++s)
    {
        const juce::Time t (s * 1000);
        const int hour = t.getHours(), minute = t.getMinutes(), sec = t.getSeconds();
        const int dayBit = 1 << t.getDayOfWeek();   // 0 = Sunday

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

CueController::GoResult CueController::preview (bool audition)
{
    const auto* cue = document.cues.getSelected();

    if (cue == nullptr)
        return GoResult::nothingSelected;

    const int index = document.cues.getSelectedIndex();
    const Cue copy = *cue;
    const auto result = trigger (copy, audition);

    if (result == GoResult::started)
        status ((isAuditionRequested (audition) ? ko ("오디션 미리듣기: ") : ko ("미리듣기: ")) + cueLabel (index, copy));

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
        cancelPendingFor (cue->id);   // its pre-wait / follow must not fire after a reset
        engine.stop (cue->id);
        played.erase (cue->id);
        status (ko ("리셋: ") + cueLabel (document.cues.getSelectedIndex(), *cue));
    }
}

void CueController::resetAll()
{
    cancelPending();
    ducks.clear();
    played.clear();
    engine.stopAll();
    document.cues.setPlayheadIndex (document.cues.isEmpty() ? -1 : 0);
    status (ko ("전체 리셋"));
}

} // namespace gocue
