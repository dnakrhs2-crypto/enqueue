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

    int index = -1;
    const auto* list = document.listContaining (id, &index);
    return list != nullptr && list->get (index).isGroup() && isGroupActive (*list, index);
}

CueController::GoResult CueController::triggerControl (const Cue& cue, int index, bool audition)
{
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

    int target = -1;
    CueList* targetList = ctl.targetId.isNull() ? nullptr : document.listContaining (ctl.targetId, &target);

    if (targetList == nullptr)
    {
        status (ko ("제어 큐에 대상이 없습니다: ") + cueLabel (index, cue), true);
        return GoResult::failed;
    }

    auto& cues = *targetList;   // the list the target lives in (not necessarily the active one)
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
                fireSequence (cues, target, audition);   // like a GO on the target, without moving the playhead
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

            if (targetCue.makesSound() && ! engine.load (targetCue, targetCue.isMic() ? 0.0 : ctl.seconds, &error))
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
            // applied once the outermost GO / sequence has finished walking its list: switching lists in the
            // middle of a sequence would pull the rows out from under it
            pendingGoto.set = true;
            pendingGoto.container = document.containerOf (targetCue.id);
            pendingGoto.cueId = targetCue.id;
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
    return isGroupActive (document.cues, index);
}

bool CueController::isGroupActive (const CueList& cues, int index) const
{
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

        if (const auto w = waits.find (c.id); w != waits.end() && clock() < w->second)
            return true;   // a wait cue inside the group is still running
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
    int index = -1;
    auto* list = document.listContaining (groupId, &index);

    if (list == nullptr)
        return;

    for (int i : list->descendantsOf (index))
    {
        const auto id = list->get (i).id;
        cancelPendingFor (id);
        playlists.erase (id);
        waits.erase (id);
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
    return startGroup (document.cues, index, audition);
}

int CueController::startGroup (CueList& cues, int index, bool audition)
{
    lastGroupEnterIndex = -1;
    lastGroupEnterList = &cues;

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
            const int after = fireSequence (cues, children.front(), audition);
            lastGroupEnterIndex = after < end ? after : end;
            break;
        }

        case GroupMode::startFirst:
            fireSequence (cues, children.front(), audition);
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
            fireSequence (cues, chosen, audition);
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

    const auto* group = document.findCueAnywhere (groupId);

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
        const auto* child = document.findCueAnywhere (childId);

        if (child == nullptr || ! child->armed)
        {
            ++run.position;   // deleted / disarmed: skip
            continue;
        }

        run.current = childId;
        const double startAt = clock() + child->preWaitSeconds;
        const bool audition = run.audition;

        if (! scheduleStart (childId, startAt, audition))   // std::map: 'run' stays valid even if a nested playlist registers
        {
            // could not start (file missing ...): on to the next, but a list where nothing starts must not spin forever
            if (++run.failures >= (int) run.order.size())
                break;

            ++run.position;
            continue;
        }

        run.failures = 0;

        // the pre-wait of the child after this one: a crossfade must start that much earlier to land on time
        double nextPreWait = 0.0;

        {
            const int nextPos = run.position + 1 < (int) run.order.size() ? run.position + 1 : (loop && ! run.order.empty() ? 0 : -1);

            if (nextPos >= 0)
                if (const auto* nextChild = document.findCueAnywhere (run.order[(size_t) nextPos]))
                    nextPreWait = juce::jmax (0.0, nextChild->preWaitSeconds);
        }

        // the next child follows when this one is over (or 'xf' seconds before its end for a crossfade); a group,
        // a wait or a fade counts as running as long as the controller says so, not only while the engine plays it
        track (scheduler.watch ([this, childId, startAt, crossfade, xf, nextPreWait]
                                {
                                    if (clock() < startAt)
                                        return false;

                                    if (! isCueActive (childId))
                                        return true;

                                    if (! crossfade || ! engine.isPlaying (childId))
                                        return false;

                                    const double remaining = remainingSecondsOf (childId);
                                    return remaining >= 0.0 && remaining <= xf + nextPreWait;
                                },
                                [this, groupId, childId, crossfade, xf, nextPreWait]
                                {
                                    if (crossfade && engine.isPlaying (childId))
                                    {
                                        const int ms = (int) std::lround (xf * 1000.0);

                                        if (nextPreWait > 0.0)   // fade out when the next one actually starts
                                            track (scheduler.schedule (clock() + nextPreWait, [this, childId, ms] { if (engine.isPlaying (childId)) engine.fadeOutAndStop (childId, ms); }), groupId);
                                        else
                                            engine.fadeOutAndStop (childId, ms);
                                    }

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

    const auto* group = document.findCueAnywhere (groupId);
    cancelPendingFor (groupId);   // the follow watch of the current child
    const auto current = it->second.current;

    if (! current.isNull())
    {
        cancelPendingFor (current);   // still in its pre-wait: it must not come alive later next to the new entry

        if (engine.isPlaying (current))
        {
            if (group != nullptr && group->group.crossfade && group->group.crossfadeSeconds > 0.0)
                engine.fadeOutAndStop (current, (int) std::lround (group->group.crossfadeSeconds * 1000.0));
            else
                engine.fadeOutAndStop (current);
        }
        else
        {
            stopCue (current);   // a nested group / wait / fade
        }
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

    // the duck this cue put on the others is released with its cleanup watch (which was just cancelled)
    bool ducked = false;

    for (auto& target : ducks)
        ducked = target.second.erase (cueId) > 0 || ducked;

    if (ducked)
        refreshDucks (0.2);
}

//==============================================================================
bool CueController::isAuditionRequested (bool requested) const noexcept
{
    return requested || document.settings.alwaysAudition;
}

AudioEngine::PlayOptions CueController::playOptions (bool audition) const
{
    AudioEngine::PlayOptions options;
    options.startSeconds = startOffsetForNextPlay;

    if (! isAuditionRequested (audition))
        return options;

    options.audition = true;

    switch (document.settings.audition)
    {
        case WorkspaceSettings::Audition::unchanged:      break;
        case WorkspaceSettings::Audition::none:           options.silent = true; break;
        case WorkspaceSettings::Audition::alternatePatch:
            options.patchOverride = document.settings.auditionPatchId;

            if (options.patchOverride.isNull() || document.findPatch (options.patchOverride) == nullptr)
                options.silent = true;   // the alternate patch was deleted: audition without output rather than for real
            break;
    }

    return options;
}

CueController::GoResult CueController::trigger (const Cue& cue, bool audition)
{
    // a cue that ends up starting itself (A -> start B -> start A, a group holding its own start cue) is refused
    for (const auto& d : dispatchStack)
        if (d.id == cue.id)
        {
            status (ko ("순환 참조라서 실행하지 않음: ") + cue.name, true);
            return GoResult::failed;
        }

    if (dispatchStack.size() >= 32)
    {
        status (ko ("실행 사슬이 너무 깊어서 멈춤: ") + cue.name, true);
        return GoResult::failed;
    }

    const DepthGuard depth (*this);
    dispatchStack.push_back ({ cue.id, cue.isControl() });
    const auto result = triggerImpl (cue, audition);
    dispatchStack.pop_back();

    if (! firstTriggerSeen)
    {
        firstTriggerSeen = true;
        firstTriggerResult = result;
    }

    if (recording && result == GoResult::started && ! cue.isGroup())   // a group's children record themselves
    {
        bool underControl = false;   // a start control cue records itself, not what it starts (else the replay starts it twice)

        for (const auto& d : dispatchStack)
            underControl = underControl || d.isControl;

        if (! underControl)
            recorded.push_back ({ cue.id, juce::jmax (0.0, clock() - recordingStart) });
    }

    return result;
}

void CueController::applyPendingGoto()
{
    if (! pendingGoto.set)
        return;

    pendingGoto.set = false;

    if (pendingGoto.container >= 0 && pendingGoto.container != document.getActiveContainer())
        document.setActiveContainer (pendingGoto.container);   // a target in another list / cart brings that one to the front

    if (const int index = document.cues.indexOf (pendingGoto.cueId); index >= 0)
    {
        document.cues.setPlayheadIndex (index);
        gotoApplied = true;
    }
}

void CueController::startRecording()
{
    recording = true;
    recordingStart = clock();
    recorded.clear();
    status (ko ("시퀀스 녹음 시작 — 지금부터 시작되는 큐와 시각을 기록합니다"));
}

std::vector<CueController::RecordedStart> CueController::stopRecording()
{
    recording = false;
    auto result = std::move (recorded);
    recorded.clear();
    status (ko ("시퀀스 녹음 정지: ") + juce::String (result.size()) + ko ("개 기록"));
    return result;
}

CueController::GoResult CueController::triggerImpl (const Cue& cue, bool audition)
{
    int index = -1;
    CueList* listPtr = document.listContaining (cue.id, &index);
    CueList& cues = listPtr != nullptr ? *listPtr : document.cues;   // the list the cue lives in (any list / cart)
    const bool auditionNow = isAuditionRequested (audition);

    if (cue.isControl())
        return triggerControl (cue, index, audition);

    if (cue.isGroup())
    {
        if (index < 0)
            return GoResult::failed;

        if (isGroupActive (cues, index))
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
                    for (int i : cues.descendantsOf (index))
                        if (engine.isPlaying (cues.get (i).id))
                            engine.finishCurrentPass (cues.get (i).id);

                    status (ko ("이번 반복까지만: ") + cueLabel (index, cue));
                    return GoResult::ignored;

                case SecondTriggerAction::hardStopRestart:
                    stopGroup (cue.id, 0);
                    break;
            }
        }

        startGroup (cues, index, audition);
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

        played.insert (cue.id);   // the second colour applies to fades too
        return GoResult::started;
    }

    if (cue.isDevamp())
    {
        const auto targetId = cue.devamp.targetId;

        if (targetId.isNull() || document.findCueAnywhere (targetId) == nullptr)
        {
            status (ko ("디밴프 큐에 대상이 없습니다: ") + cueLabel (index, cue), true);
            return GoResult::failed;
        }

        if (! engine.isPlaying (targetId))
        {
            status (ko ("디밴프 대상이 재생 중이 아닙니다: ") + cueLabel (index, cue), true);
            return GoResult::failed;
        }

        const auto instance = engine.getStartOrder (targetId);
        const double secondsToBoundary = engine.finishCurrentPass (targetId, cue.devamp.stopTarget);

        if (secondsToBoundary < 0.0)
        {
            status (ko ("디밴프 대상에 끝낼 반복이 없습니다: ") + cueLabel (index, cue), true);
            return GoResult::failed;
        }

        status (ko ("디밴프: ") + cueLabel (index, cue));

        if (cue.devamp.startNextCue)
        {
            // the cue after this one starts the moment the target reaches its loop point: watched on the target's own
            // timeline (a pause or rate change moves the moment; a stop or restart of the target cancels it)
            const auto nextId = cues.isValidIndex (index + 1) ? cues.get (index + 1).id : juce::Uuid::null();
            const juce::int64 boundary = engine.getVirtualPosition (targetId) < 0 ? -1
                                       : engine.getVirtualPosition (targetId)
                                         + (juce::int64) std::llround (secondsToBoundary * juce::jmax (AudioCueData::minRate, engine.getLiveRate (targetId)) * engine.getFileSampleRate (targetId));
            const double deadline = clock() + secondsToBoundary + 1.0;   // a stalled position (device stopped) must not wait forever

            if (! nextId.isNull() && boundary >= 0)
                track (scheduler.watch ([this, targetId, instance, boundary, deadline]
                                        {
                                            if (engine.getStartOrder (targetId) != instance)
                                                return true;   // gone or restarted: the action checks and does nothing

                                            const auto pos = engine.getVirtualPosition (targetId);
                                            return pos >= boundary - engine.getBlockSize() || clock() >= deadline;
                                        },
                                        [this, targetId, instance, nextId, audition]
                                        {
                                            if (engine.getStartOrder (targetId) != instance)
                                                return;

                                            int nextIndex = -1;

                                            if (auto* nextList = document.listContaining (nextId, &nextIndex))
                                                fireSequence (*nextList, nextIndex, audition);
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
                if (cue.isMic() || engine.finishCurrentPass (cue.id) < 0.0)
                {
                    status (ko ("끝낼 반복이 없습니다: ") + cueLabel (index, cue), true);   // a mic cue, or no loop pass
                    return GoResult::ignored;
                }

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
void CueController::applyFadeStopOthers (const Cue& cue, const std::set<juce::Uuid>& spare)
{
    if (! cue.fadeStopOthers.enabled)
        return;

    const int ms = (int) std::lround (cue.fadeStopOthers.seconds * 1000.0);
    const auto scope = cue.fadeStopOthers.scope;
    const auto ownTarget = cue.targetId();   // a fade / devamp / control cue must not fade its own target out
    const int ownContainer = document.containerOf (cue.id);

    // peers = same parent in the same list; list = the cue's own list / cart; all = every list
    auto inScope = [&] (const juce::Uuid& otherId)
    {
        if (scope == FadeStopScope::all)
            return true;

        if (document.containerOf (otherId) != ownContainer)
            return false;

        if (scope == FadeStopScope::list)
            return true;

        const auto* other = document.findCueAnywhere (otherId);
        return other == nullptr || other->parentId == cue.parentId;
    };

    // running fade cues are "others" too
    for (const auto& f : fadeRunner.getRunning())
        if (spare.count (f.fadeId) == 0 && inScope (f.fadeId))
            fadeRunner.stop (f.fadeId);

    for (const auto& p : engine.getPlayingCues())
    {
        if (spare.count (p.id) != 0 || p.id == ownTarget || p.loaded)
            continue;

        if (inScope (p.id))
            engine.fadeOutAndStop (p.id, ms);
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

void CueController::applyDuck (const Cue& cue, const std::set<juce::Uuid>& spare)
{
    if (! cue.duck.enabled)
        return;

    bool any = false;

    for (const auto& p : engine.getPlayingCues())
    {
        if (spare.count (p.id) != 0 || p.loaded)
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

std::set<juce::Uuid> CueController::familyOf (const Cue& cue) const
{
    std::set<juce::Uuid> family { cue.id };
    int index = -1;

    if (cue.isGroup())
        if (const auto* list = document.listContaining (cue.id, &index))
            for (int i : list->descendantsOf (index))
                family.insert (list->get (i).id);

    return family;
}

CueController::GoResult CueController::startById (const juce::Uuid& id, bool audition)
{
    const auto* cue = document.findCueAnywhere (id);

    if (cue == nullptr)
        return GoResult::failed;   // deleted while it was waiting

    const Cue copy = *cue;
    const auto result = trigger (copy, audition);

    if (result != GoResult::started)
        return result;

    // a group's own fade-stop-others / duck must not hit the children it just started
    const auto spare = familyOf (copy);
    applyFadeStopOthers (copy, spare);
    applyDuck (copy, spare);
    return result;
}

CueController::GoResult CueController::fire (const juce::Uuid& cueId, bool audition)
{
    return startById (cueId, audition);
}

bool CueController::scheduleStart (const juce::Uuid& id, double atSeconds, bool audition)
{
    if (atSeconds <= clock())
        return startById (id, audition) != GoResult::failed;

    track (scheduler.schedule (atSeconds, [this, id, audition] { startById (id, audition); }), id);
    return true;
}

int CueController::sequenceEnd (int index) const
{
    return sequenceEnd (document.cues, index);
}

int CueController::sequenceEnd (const CueList& cues, int index) const
{
    // a sequence runs along siblings: the chain stops at the end of the enclosing group
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
    return fireSequence (document.cues, index, audition);
}

int CueController::fireSequence (CueList& cues, int index, bool audition)
{
    if (! cues.isValidIndex (index))
        return juce::jmin (juce::jmax (index, 0), cues.size());

    const DepthGuard depth (*this);   // a goto inside the sequence applies once this walk is over

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
        lastGroupEnterList = nullptr;

        if (cue.armed)
            scheduleStart (cue.id, startAt, audition);
        else
            status (ko ("비활성 큐 건너뜀: ") + cueLabel (i, cue));

        // a devamp that starts the next cue itself is its own continuation: its continue mode is ignored
        if (cue.continueMode == ContinueMode::none || (cue.isDevamp() && cue.devamp.startNextCue))
            return lastGroupEnterIndex >= 0 && lastGroupEnterList == &cues ? lastGroupEnterIndex : next;   // "start first and enter": the playhead goes inside (this list only)

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
                                        int nextIndex = -1;

                                        if (auto* nextList = document.listContaining (nextId, &nextIndex))
                                            fireSequence (*nextList, nextIndex, audition);
                                    }), id);

        return sequenceEnd (cues, index);
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

    if (document.isActiveCart())
        return GoResult::nothingSelected;   // a cart has no playhead: its buttons fire cues

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
    gotoApplied = false;
    const int after = fireSequence (index, audition);

    if (gotoApplied)
    {
        gotoApplied = false;   // a goto cue in the sequence put the playhead where it wanted (maybe in another list)
        status (ko ("GO: ") + cueLabel (index, copy));
        return GoResult::started;
    }
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

    document.forEachList ([&] (CueList& cues)   // hotkeys reach into every list and cart
    {
        for (int i = 0; i < cues.size(); ++i)
        {
            const auto& cue = cues.get (i);

            if (cue.hotkey.isNotEmpty() && juce::KeyPress::createFromDescription (cue.hotkey) == key)
            {
                fireSequence (cues, i, false);   // hotkeys do not move the playhead
                status (ko ("핫키: ") + cueLabel (i, cue));
                handled = true;
            }
        }
    });

    return handled;
}

bool CueController::handleHotkeyRepeat (const juce::KeyPress& key) const
{
    bool found = false;

    document.forEachList ([&] (CueList& cues)
    {
        for (const auto& cue : cues.getAll())
            if (cue.hotkey.isNotEmpty() && juce::KeyPress::createFromDescription (cue.hotkey) == key)
                found = true;
    });

    return found;
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

    for (juce::int64 s = from; s <= second; ++s)
    {
        const juce::Time t (s * 1000);
        const int hour = t.getHours(), minute = t.getMinutes(), sec = t.getSeconds();
        const int dayBit = 1 << t.getDayOfWeek();   // 0 = Sunday

        document.forEachList ([&] (CueList& cues)   // every list and cart
        {
            for (int i = 0; i < cues.size(); ++i)
            {
                const auto& wc = cues.get (i).wallClock;

                if (wc.enabled && wc.hour == hour && wc.minute == minute && wc.second == sec && (wc.daysMask & dayBit) != 0)
                {
                    status (ko ("시간 트리거: ") + cueLabel (i, cues.get (i)));
                    fireSequence (cues, i, false);
                }
            }
        });
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

CueController::GoResult CueController::previewFrom (double regionSeconds)
{
    startOffsetForNextPlay = juce::jmax (0.0, regionSeconds);
    const auto result = preview (false);
    startOffsetForNextPlay = 0.0;
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

void CueController::stopCue (const juce::Uuid& cueId, bool fade)
{
    cancelPendingFor (cueId);   // its pre-wait / follow must not fire afterwards
    waits.erase (cueId);
    playlists.erase (cueId);
    fadeRunner.stop (cueId);    // a fade cue

    for (const auto& f : fadeRunner.getRunning())   // fades aimed at this cue
        if (f.targetId == cueId)
            fadeRunner.stop (f.fadeId);

    const auto* cue = document.findCueAnywhere (cueId);

    if (cue != nullptr && cue->isGroup())
        stopGroup (cueId, fade ? -1 : 0);
    else if (fade && engine.isPlaying (cueId))
        engine.fadeOutAndStop (cueId);
    else
        engine.stop (cueId);
}

void CueController::resetSelected()
{
    if (const auto* cue = document.cues.getSelected())
    {
        stopCue (cue->id);
        played.erase (cue->id);
        status (ko ("리셋: ") + cueLabel (document.cues.getSelectedIndex(), *cue));
    }
}

void CueController::resetForNewProject()
{
    fadeRunner.resetSession();
    cancelPending();
    playlists.clear();
    randomUsed.clear();
    waits.clear();
    ducks.clear();
    played.clear();
    recorded.clear();
    recording = false;
    lastGroupEnterIndex = -1;
    lastGroupEnterList = nullptr;
    pendingGoto = {};
    gotoApplied = false;
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
