#include "app/CueController.h"

#include <algorithm>

namespace gocue
{

namespace
{
    juce::String ko (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

CueController::CueController (AudioEngine& e, ProjectDocument& d, Scheduler& s)
    : engine (e), document (d), scheduler (s), fadeRunner (e, d)
{
    clock = [this] { return scheduler.now(); };
    fadeRunner.clock = [this] { return clock(); };
    randomChoice = [] (int count) { return juce::Random::getSystemRandom().nextInt (juce::jmax (1, count)); };
}

bool CueController::isCueActive (const juce::Uuid& id) const
{
    if (engine.isPlaying (id) || fadeRunner.isRunning (id))
        return true;

    if (const auto w = waits.find (id); w != waits.end() && clock() < w->second)
        return true;

    const int index = document.cues.indexOf (id);
    return index >= 0 && document.cues.get (index).isGroup() && isGroupActive (index);
}

CueController::GoResult CueController::triggerControl (const Cue& cue, int index, bool audition)
{
    auto& cues = document.cues;
    const auto& ctl = cue.control;

    if (ctl.kind == ControlKind::wait)
    {
        waits[cue.id] = clock() + ctl.seconds;
        status (ko ("대기 ") + juce::String (ctl.seconds, 2) + ko ("초: ") + cueLabel (index, cue));
        played.insert (cue.id);
        return GoResult::started;
    }

    if (ctl.kind == ControlKind::memo)
    {
        played.insert (cue.id);
        return GoResult::started;
    }

    const int target = ctl.targetId.isNull() ? -1 : cues.indexOf (ctl.targetId);

    if (target < 0)
    {
        status (ko ("제어 큐에 대상이 없습니다: ") + cueLabel (index, cue), true);
        return GoResult::failed;
    }

    const Cue targetCue = cues.get (target);
    const auto targetLabel = cueLabel (target, targetCue);

    switch (ctl.kind)
    {
        case ControlKind::start:
            if (engine.isPaused (targetCue.id))
            {
                engine.resume (targetCue.id);
                status (ko ("재개: ") + targetLabel);
            }
            else
            {
                fireSequence (target, audition);   // like a GO on the target, without moving the playhead
                status (ko ("시작: ") + targetLabel);
            }
            break;

        case ControlKind::stop:
            cancelPendingFor (targetCue.id);
            waits.erase (targetCue.id);

            if (targetCue.isGroup())
                stopGroup (targetCue.id, 0);
            else
            {
                fadeRunner.stop (targetCue.id);
                engine.stop (targetCue.id);
            }

            status (ko ("정지: ") + targetLabel);
            break;

        case ControlKind::pause:
            if (targetCue.isGroup())
            {
                for (int i : cues.descendantsOf (target))
                    if (engine.isPlaying (cues.get (i).id) && ! engine.isPaused (cues.get (i).id))
                        engine.pause (cues.get (i).id);
            }
            else if (engine.isPlaying (targetCue.id) && ! engine.isPaused (targetCue.id))
            {
                engine.pause (targetCue.id);
            }

            status (ko ("일시정지: ") + targetLabel);
            break;

        case ControlKind::load:
        {
            juce::String error;

            if (targetCue.isAudio() && ! engine.load (targetCue, ctl.seconds, &error))
            {
                status (error, true);
                return GoResult::failed;
            }

            status (ko ("로드: ") + targetLabel);
            break;
        }

        case ControlKind::reset:
            cancelPendingFor (targetCue.id);
            waits.erase (targetCue.id);
            fadeRunner.stop (targetCue.id);

            if (targetCue.isGroup())
                stopGroup (targetCue.id, 0);
            else
                engine.stop (targetCue.id);

            played.erase (targetCue.id);
            status (ko ("리셋: ") + targetLabel);
            break;

        case ControlKind::gotoCue:
            lastGroupEnterIndex = target;   // go() puts the playhead here instead of past this cue
            cues.setPlayheadIndex (target);
            status (ko ("이동: ") + targetLabel);
            break;

        case ControlKind::arm:
        case ControlKind::disarm:
        {
            const bool arm = ctl.kind == ControlKind::arm;
            cues.update (target, [arm] (Cue& c) { c.armed = arm; });
            status ((arm ? ko ("활성화: ") : ko ("비활성화: ")) + targetLabel);
            break;
        }

        case ControlKind::target:
        {
            if (! targetCue.hasTarget())
            {
                status (ko ("대상 큐가 대상을 가질 수 없는 종류입니다: ") + targetLabel, true);
                return GoResult::failed;
            }

            const auto newTarget = ctl.secondTargetId;
            cues.update (target, [newTarget] (Cue& c) { c.setTargetId (newTarget); });
            status (ko ("대상 변경: ") + targetLabel);
            break;
        }

        case ControlKind::wait:
        case ControlKind::memo:
            break;
    }

    played.insert (cue.id);
    return GoResult::started;
}

bool CueController::isGroupActive (int index) const
{
    const auto& cues = document.cues;

    if (! cues.isValidIndex (index))
        return false;

    const auto id = cues.get (index).id;

    if (playlists.count (id) != 0 || hasPendingFor (id))
        return true;

    for (int i : cues.descendantsOf (index))
    {
        const auto& c = cues.get (i);

        if (engine.isPlaying (c.id) || fadeRunner.isRunning (c.id) || hasPendingFor (c.id) || playlists.count (c.id) != 0)
            return true;
    }

    return false;
}

double CueController::remainingSecondsOf (const juce::Uuid& id) const
{
    for (const auto& p : engine.getPlayingCues())
        if (p.id == id && ! p.loaded)
            return p.remainingSeconds;

    return -1.0;
}

void CueController::stopGroup (const juce::Uuid& groupId, int fadeMs)
{
    cancelPendingFor (groupId);
    playlists.erase (groupId);
    const int index = document.cues.indexOf (groupId);

    if (index < 0)
        return;

    for (int i : document.cues.descendantsOf (index))
    {
        const auto id = document.cues.get (i).id;
        cancelPendingFor (id);
        playlists.erase (id);
        fadeRunner.stop (id);

        if (! engine.isPlaying (id))
            continue;

        if (fadeMs == 0)
            engine.stop (id);
        else if (fadeMs < 0)
            engine.fadeOutAndStop (id);
        else
            engine.fadeOutAndStop (id, fadeMs);
    }
}

int CueController::startGroup (int index, bool audition)
{
    auto& cues = document.cues;
    lastGroupEnterIndex = -1;

    if (! cues.isValidIndex (index) || ! cues.get (index).isGroup())
        return juce::jmin (index + 1, cues.size());

    const Cue group = cues.get (index);
    const auto children = cues.childrenOf (index);
    const int end = cues.subtreeEnd (index);

    if (children.empty())
    {
        status (ko ("빈 그룹: ") + cueLabel (index, group));
        return end;
    }

    switch (group.group.mode)
    {
        case GroupMode::timeline:
        {
            // every child starts now (after its own pre-wait); the children's continue modes do not apply
            const double t = clock();

            for (int child : children)
            {
                const Cue c = cues.get (child);

                if (! c.armed)
                {
                    if (! c.skipIfDisarmed)
                        status (ko ("비활성 큐 건너뜀: ") + cueLabel (child, c));

                    continue;
                }

                scheduleStart (c.id, t + c.preWaitSeconds, audition);
            }

            break;
        }

        case GroupMode::playlist:
        {
            PlaylistRun run;
            run.audition = audition;

            for (int child : children)
                run.order.push_back (cues.get (child).id);

            if (group.group.shuffle)
                for (int i = (int) run.order.size() - 1; i > 0; --i)
                    std::swap (run.order[(size_t) i], run.order[(size_t) randomChoice (i + 1)]);

            playlists[group.id] = std::move (run);
            playlistStep (group.id);
            break;
        }

        case GroupMode::startFirstEnter:
        {
            const int after = fireSequence (children.front(), audition);
            lastGroupEnterIndex = after < end ? after : end;
            break;
        }

        case GroupMode::startFirst:
            fireSequence (children.front(), audition);
            break;

        case GroupMode::random:
        {
            auto& used = randomUsed[group.id];
            std::vector<int> candidates;

            for (int child : children)
                if (used.count (cues.get (child).id) == 0 && cues.get (child).armed)
                    candidates.push_back (child);

            if (candidates.empty())
            {
                // everyone has had a turn: start a new round
                used.clear();

                for (int child : children)
                    if (cues.get (child).armed)
                        candidates.push_back (child);
            }

            if (candidates.empty())
            {
                status (ko ("그룹에 활성 큐가 없습니다: ") + cueLabel (index, group));
                break;
            }

            const int chosen = candidates[(size_t) juce::jlimit (0, (int) candidates.size() - 1, randomChoice ((int) candidates.size()))];
            used.insert (cues.get (chosen).id);
            fireSequence (chosen, audition);
            break;
        }
    }

    return end;
}

void CueController::playlistStep (const juce::Uuid& groupId)
{
    auto it = playlists.find (groupId);

    if (it == playlists.end())
        return;

    const auto* group = document.cues.findById (groupId);

    if (group == nullptr || ! group->isGroup())
    {
        playlists.erase (it);
        return;
    }

    auto& run = it->second;
    const bool loop = group->group.loop;
    const bool shuffle = group->group.shuffle;
    const bool crossfade = group->group.crossfade && group->group.crossfadeSeconds > 0.0;
    const double xf = group->group.crossfadeSeconds;

    for (int guard = 0; guard <= (int) run.order.size(); ++guard)
    {
        if (run.position >= (int) run.order.size())
        {
            if (! loop || run.order.empty())
                break;

            run.position = 0;

            if (shuffle)
                for (int i = (int) run.order.size() - 1; i > 0; --i)
                    std::swap (run.order[(size_t) i], run.order[(size_t) randomChoice (i + 1)]);
        }

        const auto childId = run.order[(size_t) run.position];
        const auto* child = document.cues.findById (childId);

        if (child == nullptr || ! child->armed)
        {
            ++run.position;   // deleted / disarmed: skip
            continue;
        }

        run.current = childId;
        const double startAt = clock() + child->preWaitSeconds;
        const bool audition = run.audition;
        scheduleStart (childId, startAt, audition);   // std::map: 'run' stays valid even if a nested playlist registers

        // the next child follows when this one is over (or 'xf' seconds before its end for a crossfade)
        track (scheduler.watch ([this, childId, startAt, crossfade, xf]
                                {
                                    if (clock() < startAt)
                                        return false;

                                    if (! engine.isPlaying (childId))
                                        return true;

                                    if (! crossfade)
                                        return false;

                                    const double remaining = remainingSecondsOf (childId);
                                    return remaining >= 0.0 && remaining <= xf;
                                },
                                [this, groupId, childId, crossfade, xf]
                                {
                                    if (crossfade && engine.isPlaying (childId))
                                        engine.fadeOutAndStop (childId, (int) std::lround (xf * 1000.0));

                                    if (auto next = playlists.find (groupId); next != playlists.end())
                                    {
                                        ++next->second.position;
                                        playlistStep (groupId);
                                    }
                                }), groupId);
        return;
    }

    playlists.erase (groupId);   // the end of the list (or nothing playable)
}

bool CueController::playlistSkip (const juce::Uuid& groupId, int delta)
{
    auto it = playlists.find (groupId);

    if (it == playlists.end())
        return false;

    const auto* group = document.cues.findById (groupId);
    cancelPendingFor (groupId);   // the follow watch of the current child
    const auto current = it->second.current;

    if (! current.isNull() && engine.isPlaying (current))
    {
        cancelPendingFor (current);

        if (group != nullptr && group->group.crossfade && group->group.crossfadeSeconds > 0.0)
            engine.fadeOutAndStop (current, (int) std::lround (group->group.crossfadeSeconds * 1000.0));
        else
            engine.fadeOutAndStop (current);
    }

    it->second.position = juce::jmax (0, it->second.position + delta);
    playlistStep (groupId);
    return true;
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
    const auto result = triggerImpl (cue, audition);

    if (! firstTriggerSeen)
    {
        firstTriggerSeen = true;
        firstTriggerResult = result;
    }

    return result;
}

CueController::GoResult CueController::triggerImpl (const Cue& cue, bool audition)
{
    const int index = document.cues.indexOf (cue.id);
    const bool auditionNow = isAuditionRequested (audition);

    if (cue.isControl())
        return triggerControl (cue, index, audition);

    if (cue.isGroup())
    {
        if (index < 0)
            return GoResult::failed;

        if (isGroupActive (index))
        {
            if (cue.group.mode == GroupMode::playlist && playlists.count (cue.id) != 0)
            {
                playlistSkip (cue.id, 1);
                status (ko ("플레이리스트 다음: ") + cueLabel (index, cue));
                return GoResult::ignored;
            }

            switch (cue.secondTrigger)
            {
                case SecondTriggerAction::nothing:
                    status (ko ("이미 재생 중: ") + cueLabel (index, cue));
                    return GoResult::ignored;

                case SecondTriggerAction::panic:
                    stopGroup (cue.id, (int) std::lround (document.settings.panicSeconds * 1000.0));
                    status (ko ("페이드 정지: ") + cueLabel (index, cue));
                    return GoResult::ignored;

                case SecondTriggerAction::stop:
                    stopGroup (cue.id, -1);
                    status (ko ("페이드 정지: ") + cueLabel (index, cue));
                    return GoResult::ignored;

                case SecondTriggerAction::hardStop:
                    stopGroup (cue.id, 0);
                    status (ko ("정지: ") + cueLabel (index, cue));
                    return GoResult::ignored;

                case SecondTriggerAction::devamp:
                    for (int i : document.cues.descendantsOf (index))
                        if (engine.isPlaying (document.cues.get (i).id))
                            engine.finishCurrentPass (document.cues.get (i).id);

                    status (ko ("이번 반복까지만: ") + cueLabel (index, cue));
                    return GoResult::ignored;

                case SecondTriggerAction::hardStopRestart:
                    stopGroup (cue.id, 0);
                    break;
            }
        }

        startGroup (index, audition);
        played.insert (cue.id);
        return GoResult::started;
    }

    if (cue.isFade())
    {
        // a running fade fired again restarts from where its target is now
        juce::String error;

        if (! fadeRunner.start (cue, &error))
        {
            status (error, true);
            return GoResult::failed;
        }

        return GoResult::started;
    }

    if (cue.isDevamp())
    {
        const auto targetId = cue.devamp.targetId;

        if (targetId.isNull() || document.cues.findById (targetId) == nullptr)
        {
            status (ko ("디밴프 큐에 대상이 없습니다: ") + cueLabel (index, cue), true);
            return GoResult::failed;
        }

        if (! engine.isPlaying (targetId))
        {
            status (ko ("디밴프 대상이 재생 중이 아닙니다: ") + cueLabel (index, cue), true);
            return GoResult::failed;
        }

        engine.finishCurrentPass (targetId, cue.devamp.stopTarget);
        status (ko ("디밴프: ") + cueLabel (index, cue));

        if (cue.devamp.startNextCue)
        {
            // the cue after this one starts the moment the target reaches its loop point
            const auto nextId = document.cues.isValidIndex (index + 1) ? document.cues.get (index + 1).id : juce::Uuid::null();
            const double at = clock() + juce::jmax (0.0, engine.getSecondsToPassEnd (targetId));

            if (! nextId.isNull())
                track (scheduler.schedule (at, [this, nextId, audition]
                                          {
                                              if (const int nextIndex = document.cues.indexOf (nextId); nextIndex >= 0)
                                                  fireSequence (nextIndex, audition);
                                          }), cue.id);
        }

        return GoResult::started;
    }

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
    const bool peersOnly = cue.fadeStopOthers.scope == FadeStopScope::peers;

    for (const auto& p : engine.getPlayingCues())
    {
        if (p.id == cue.id || p.loaded)
            continue;

        if (peersOnly)
        {
            // peers = the cues with the same parent (inside the same group / at the top level)
            const auto* other = document.cues.findById (p.id);

            if (other != nullptr && other->parentId != cue.parentId)
                continue;
        }

        engine.fadeOutAndStop (p.id, ms);   // list / all coincide while there is one cue list
    }
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
    track (scheduler.watch ([this, id] { return ! isCueActive (id); },
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
    // a sequence runs along siblings: the chain stops at the end of the enclosing group
    const auto& cues = document.cues;

    if (! cues.isValidIndex (index))
        return juce::jmin (juce::jmax (index, 0), cues.size());

    const int parent = cues.parentIndexOf (index);
    const int bound = parent >= 0 ? cues.subtreeEnd (parent) : cues.size();
    int i = index;

    while (cues.get (i).continueMode != ContinueMode::none)
    {
        const int next = cues.subtreeEnd (i);

        if (next >= bound)
            break;

        i = next;
    }

    return juce::jmin (cues.subtreeEnd (i), cues.size());
}

int CueController::fireSequence (int index, bool audition)
{
    auto& cues = document.cues;

    if (! cues.isValidIndex (index))
        return juce::jmin (juce::jmax (index, 0), cues.size());

    // the sequence walks the siblings of 'index' (a group counts as one cue; its children are its own business)
    const int parent = cues.parentIndexOf (index);
    const int bound = parent >= 0 ? cues.subtreeEnd (parent) : cues.size();
    double t = clock();
    int i = index;

    while (i >= 0 && i < bound)
    {
        const Cue cue = cues.get (i);   // copy: starting a cue may not change the list, but be safe
        const int next = cues.subtreeEnd (i);

        if (! cue.armed && cue.skipIfDisarmed)
        {
            i = next;
            continue;
        }

        const double startAt = t + cue.preWaitSeconds;
        lastGroupEnterIndex = -1;

        if (cue.armed)
            scheduleStart (cue.id, startAt, audition);
        else
            status (ko ("비활성 큐 건너뜀: ") + cueLabel (i, cue));

        if (cue.continueMode == ContinueMode::none)
            return lastGroupEnterIndex >= 0 ? lastGroupEnterIndex : next;   // "start first and enter": the playhead goes inside

        if (cue.continueMode == ContinueMode::autoContinue)
        {
            t = startAt + cue.postWaitSeconds;
            i = next;
            continue;
        }

        // auto-follow: the rest of the chain starts when this cue has finished (a disarmed cue is over at once).
        // The next cue is remembered by id: rows may be inserted / deleted / moved meanwhile.
        const auto nextId = next < bound ? cues.get (next).id : juce::Uuid::null();
        const auto id = cue.id;
        const bool armed = cue.armed;

        if (! nextId.isNull())
            track (scheduler.watch ([this, id, startAt, armed] { return clock() >= startAt && (! armed || ! isCueActive (id)); },
                                    [this, nextId, audition]
                                    {
                                        if (const int nextIndex = document.cues.indexOf (nextId); nextIndex >= 0)
                                            fireSequence (nextIndex, audition);
                                    }), id);

        return sequenceEnd (index);
    }

    return juce::jmin (juce::jmax (i, 0), cues.size());
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

    firstTriggerSeen = false;
    firstTriggerResult = GoResult::started;
    const int after = fireSequence (index, audition);
    const bool targeting = copy.isFade() || copy.isDevamp() || copy.isGroup() || copy.isControl();
    const bool firstFailed = copy.armed && copy.preWaitSeconds <= 0.0
                             && (targeting ? (firstTriggerSeen && firstTriggerResult == GoResult::failed) : ! engine.isPlaying (copy.id));
    const bool anyStarted = isCueActive (copy.id) || getNumPending() > 0 || (targeting && ! firstFailed);

    if (firstFailed)
    {
        // the first cue could not be started (missing file, fade / devamp target not playing ...): trigger() already
        // reported it, and that message must stay visible instead of a "GO:" line
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
    fadeRunner.stopAll();
    const double now = clock();
    cancelPending();
    playlists.clear();
    waits.clear();

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
    fadeRunner.stopAll();
    cancelPending();
    playlists.clear();
    waits.clear();
    engine.stopAll();
    status (ko ("전체 즉시 정지"));
}

void CueController::resetSelected()
{
    if (const auto* cue = document.cues.getSelected())
    {
        cancelPendingFor (cue->id);   // its pre-wait / follow must not fire after a reset

        if (cue->isGroup())
            stopGroup (cue->id, 0);
        else
            engine.stop (cue->id);

        played.erase (cue->id);
        status (ko ("리셋: ") + cueLabel (document.cues.getSelectedIndex(), *cue));
    }
}

void CueController::resetAll()
{
    fadeRunner.stopAll();
    cancelPending();
    playlists.clear();
    randomUsed.clear();
    waits.clear();
    ducks.clear();
    played.clear();
    engine.stopAll();
    document.cues.setPlayheadIndex (document.cues.isEmpty() ? -1 : 0);
    status (ko ("전체 리셋"));
}

} // namespace gocue
