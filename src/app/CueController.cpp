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
            if (! targetCue.armed)
            {
                status (ko ("비활성 큐: 시작하지 않음: ") + targetLabel);
                break;
            }

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
            cues.update (target, [arm] (Cue& c) { c.armed = arm; if (! arm) c.skipIfDisarmed = true; });   // older readers skip it too
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

        // a child's own follow (an observer) counts: the next child it leads to has not started yet, so the group is not over
        if (engine.isPlaying (c.id) || fadeRunner.isRunning (c.id) || hasPendingFor (c.id, true) || playlists.count (c.id) != 0)
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
                    continue;   // 비활성화

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
            {
                const auto lastPlayed = run.current;   // the child that really played last (a disarmed one at the end of the order never did)

                for (int i = (int) run.order.size() - 1; i > 0; --i)
                    std::swap (run.order[(size_t) i], run.order[(size_t) randomChoice (i + 1)]);

                // a 랜덤 반복 jukebox: the new round must not open with the song that just ended - among the children
                // that can play (a disarmed or deleted one at the front is skipped below, so the first playable counts)
                std::vector<size_t> playable;

                for (size_t k = 0; k < run.order.size(); ++k)
                    if (const auto* c = document.findCueAnywhere (run.order[k]); c != nullptr && c->armed)
                        playable.push_back (k);

                if (playable.size() > 1 && run.order[playable[0]] == lastPlayed)
                    std::swap (run.order[playable[0]], run.order[playable[(size_t) juce::jlimit (1, (int) playable.size() - 1, 1 + randomChoice ((int) playable.size() - 1))]]);
            }
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

        const bool started = scheduleStart (childId, startAt, audition) != GoResult::failed;

        // scheduleStart runs a zero-pre-wait child at once, and that child may be a control cue that stops (and so
        // erases) this very playlist. From here 'run' and 'it' may be dangling: re-find the entry before any more use.
        it = playlists.find (groupId);

        if (it == playlists.end())
            return;

        if (! started)
        {
            // could not start (file missing ...): on to the next, but a list where nothing starts must not spin forever
            if (++it->second.failures >= (int) it->second.order.size())
                break;

            ++it->second.position;
            continue;
        }

        it->second.failures = 0;

        // a zero-pre-wait child can re-enter this playlist (a control cue that starts / advances G): the re-entrant
        // call already installed the follow watch for whatever is playing now. If this run has moved off the child we
        // dispatched, do not install a second watch (it would start the next child while the current one plays).
        if (it->second.current != childId)
            return;

        // the pre-wait of the child after this one: a crossfade must start that much earlier to land on time
        double nextPreWait = 0.0;

        {
            auto& r = it->second;
            const int nextPos = r.position + 1 < (int) r.order.size() ? r.position + 1 : (loop && ! r.order.empty() ? 0 : -1);

            if (nextPos >= 0)
                if (const auto* nextChild = document.findCueAnywhere (r.order[(size_t) nextPos]))
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
    cancelPendingFor (groupId, true);   // the playlist's watch on the current child - not the group's own follow / duck restore, which outlive the skip
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

void CueController::track (int schedulerId, const juce::Uuid& owner, bool observer)
{
    // forget entries the scheduler has already run
    pending.erase (std::remove_if (pending.begin(), pending.end(), [this] (const Pending& p) { return ! scheduler.isPending (p.id); }), pending.end());
    pending.push_back ({ schedulerId, owner, observer });
}

int CueController::getNumPending() const
{
    int n = 0;

    for (const auto& p : pending)
        if (scheduler.isPending (p.id))
            ++n;

    return n;
}

bool CueController::hasPendingFor (const juce::Uuid& cueId, bool includeObservers) const
{
    for (const auto& p : pending)
        if (p.owner == cueId && (includeObservers || ! p.observer) && scheduler.isPending (p.id))
            return true;

    return false;
}

void CueController::cancelPending()
{
    for (const auto& p : pending)
        scheduler.cancel (p.id);

    pending.clear();
}

void CueController::cancelPendingFor (const juce::Uuid& cueId, bool keepObservers)
{
    auto matches = [&] (const Pending& p) { return p.owner == cueId && ! (keepObservers && p.observer); };

    for (const auto& p : pending)
        if (matches (p))
            scheduler.cancel (p.id);

    pending.erase (std::remove_if (pending.begin(), pending.end(), matches), pending.end());

    if (keepObservers)
        return;   // the run goes on (a playlist on its next child): its duck stays too

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
    options.explicitStart = explicitStartForNextPlay;

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

    if (isPanicLatched())   // every start passes here: GO, hotkey, cart button, preview, wall clock, follow, control cue
    {
        status (ko ("전체 정지 진행 중: 시작하지 않음: ") + cue.name);
        return GoResult::failed;
    }

    if (! cue.armed)   // 비활성화: not from a cart button, a preview, a scheduled start or a control cue either
    {
        status (ko ("비활성 큐: 시작하지 않음: ") + cue.name);
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
        if (auditionNow)
        {
            // a fade acts on the live instance of its target: an audition would alter (or stop) the real show
            status (ko ("페이드 큐는 미리듣기할 수 없습니다: ") + cueLabel (index, cue), true);
            return GoResult::failed;
        }

        if (const auto* fadeTarget = document.findCueAnywhere (cue.fade.targetId); fadeTarget != nullptr && engine.isAuditioning (fadeTarget->id))
        {
            status (ko ("대상이 미리듣기 중이라 페이드하지 않습니다: ") + cueLabel (index, cue), true);
            return GoResult::failed;
        }

        juce::String error;
        bool targetAutoStarted = false;

        if (cue.fade.mode == FadeMode::fadeIn)
        {
            const auto* target = document.findCueAnywhere (cue.fade.targetId);

            if (target != nullptr && ! target->makesSound())
            {
                status (ko ("페이드 인 대상은 소리 큐여야 합니다: ") + cueLabel (index, cue), true);
                return GoResult::failed;
            }

            if (target != nullptr)
            {
                const double floorDb = document.settings.minLevelDb;

                if (engine.isPaused (target->id))
                {
                    // paused: it comes back at the floor and the fade lifts it
                    engine.setLiveGainDb (target->id, floorDb);
                    engine.resume (target->id);
                }
                else if (! engine.isPlaying (target->id) || engine.isStopping (target->id))
                {
                    // not playing (or on its way out): a fresh instance starts at the floor; the request is keyed to this
                    // target so that nothing a control cue starts in between can take it
                    startNextAtLevelFor = target->id;
                    startNextLevelDb = floorDb;
                    const auto started = startById (target->id, false);
                    startNextAtLevelFor = juce::Uuid::null();

                    if (started != GoResult::started)
                    {
                        status (ko ("페이드 인 대상을 시작하지 못함: ") + cueLabel (index, cue), true);
                        return GoResult::failed;
                    }

                    targetAutoStarted = true;
                }
                // already running: the fade lifts it from where it is
            }
        }

        // a running fade fired again restarts from where its target is now
        if (! fadeRunner.start (cue, &error, targetAutoStarted))
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

    // a normal GO on a cue that is auditioning restarts it with the real output (QLab); a fade-in's floor request
    // means a fresh instance whatever the second-trigger rule says
    if (engine.isPlaying (cue.id) && startNextAtLevelFor != cue.id && ! (engine.isAuditioning (cue.id) && ! auditionNow))
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

    auto options = playOptions (audition);

    if (startNextAtLevelFor == cue.id)   // a fade-in cue starts this target at the fade floor
    {
        options.hasStartGain = true;
        options.startGainDb = startNextLevelDb;
        startNextAtLevelFor = juce::Uuid::null();
    }

    if (! engine.play (cue, options, &error))
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

    // a run in scope must not carry on behind the fade: a playlist would start its next child, a follow its next cue,
    // a pre-wait its cue, a wait cue its continuation
    auto stopsToo = [&] (const juce::Uuid& id) { return spare.count (id) == 0 && id != ownTarget && inScope (id); };
    std::vector<juce::Uuid> owners;

    for (const auto& p : pending)
        if (stopsToo (p.owner))
            owners.push_back (p.owner);

    for (const auto& owner : owners)
        cancelPendingFor (owner);

    for (auto it = playlists.begin(); it != playlists.end();)
        it = stopsToo (it->first) ? playlists.erase (it) : std::next (it);

    for (auto it = waits.begin(); it != waits.end();)
        it = stopsToo (it->first) ? waits.erase (it) : std::next (it);
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

    // this cue's contribution ends when it is over (or stopped); the others keep theirs. An observer: a group's own
    // restore watch must not count as the group still running, or it would wait for itself
    track (scheduler.watch ([this, id] { return ! isCueActive (id); },
                            [this, id, ramp]
                            {
                                for (auto& target : ducks)
                                    target.second.erase (id);

                                refreshDucks (ramp);
                            }), id, true);
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

    // a group's own fade-stop-others / duck must not hit the children it just started - nor may a control cue's hit
    // the group it just started (its children are what the operator hears of it)
    auto spare = familyOf (copy);

    if (copy.isControl() && copy.control.needsTarget())
        if (const auto* target = document.findCueAnywhere (copy.control.targetId))
            for (const auto& id2 : familyOf (*target))
                spare.insert (id2);

    applyFadeStopOthers (copy, spare);
    applyDuck (copy, spare);
    return result;
}

CueController::GoResult CueController::fire (const juce::Uuid& cueId, bool audition)
{
    return startById (cueId, audition);
}

CueController::GoResult CueController::scheduleStart (const juce::Uuid& id, double atSeconds, bool audition)
{
    if (atSeconds <= clock())
        return startById (id, audition);

    track (scheduler.schedule (atSeconds, [this, id, audition] { startById (id, audition); }), id);
    return GoResult::started;
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

    if (isPanicLatched())
    {
        status (ko ("전체 정지 진행 중: 시작하지 않음"));   // GO / hotkey / wall clock: nothing starts until the panic is over
        return index;
    }

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

        if (! cue.armed)   // 비활성화: the playhead passes over it
        {
            i = next;
            continue;
        }

        const double startAt = t + cue.preWaitSeconds;
        lastGroupEnterIndex = -1;
        lastGroupEnterList = nullptr;

        // The cue this sequence starts with is fired again while its pre-wait still runs (or while it plays and has a
        // pre-wait): its second-trigger rule applies now, not after another pre-wait, and a pending start is never
        // doubled - a hotkey pressed twice is one start
        if (i == index && cue.preWaitSeconds > 0.0 && (hasPendingFor (cue.id) || engine.isPlaying (cue.id)))
        {
            const bool playing = engine.isPlaying (cue.id);
            // a normal GO on a cue that is auditioning restarts it for real whatever its rule says (as go() / trigger() do)
            const bool auditionRestart = playing && engine.isAuditioning (cue.id) && ! isAuditionRequested (audition);
            const auto rule = auditionRestart ? SecondTriggerAction::hardStopRestart : cue.secondTrigger;

            if (rule == SecondTriggerAction::nothing)
                return next;   // the run (or its pre-wait) goes on untouched

            if (rule == SecondTriggerAction::hardStopRestart)
            {
                // the pre-wait starts over, and so does the sequence: the starts the earlier walk scheduled for the
                // cues behind this one (auto-continue) go too, or they would come up twice. The same walk as below -
                // a disarmed cue is stepped over, the chain ends at the first cue that does not auto-continue - and a
                // later cue that is already playing keeps its run (its follow, its duck): only its start was doubled
                for (int k = index; k >= 0 && k < bound; k = cues.subtreeEnd (k))
                {
                    const auto& c = cues.get (k);

                    if (! c.armed)
                        continue;

                    if (k == index || ! engine.isPlaying (c.id))
                        cancelPendingFor (c.id);

                    if (c.continueMode != ContinueMode::autoContinue)
                        break;
                }
            }
            else
            {
                if (playing)
                    trigger (cue, audition);   // stop / fade / devamp the running instance now (a devamp keeps its follow, as always)
                else
                    cancelPendingFor (cue.id);   // a start still in its pre-wait: dropped

                return next;   // the re-trigger is answered: nothing new starts behind it
            }
        }

        const auto result = scheduleStart (cue.id, startAt, audition);

        if (result == GoResult::ignored)
            return next;   // the second-trigger rule acted on (or kept) the running instance: its own sequence stands, no new one is put behind it

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
        // The next cue is remembered by id: rows may be inserted / deleted / moved meanwhile. An observer of the run
        // (see track): a group's follow must not count as the group still running, or it would wait for itself.
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
                                    }), id, true);

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

    if (isPanicLatched())
    {
        status (ko ("전체 정지 진행 중: 시작하지 않음"));   // neither a start nor a resume during the panic
        return GoResult::failed;
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

void CueController::fireSequenceById (const juce::Uuid& id, bool audition)
{
    int index = -1;
    auto* list = document.listContaining (id, &index);

    if (list == nullptr)
        return;

    if (! list->get (index).armed)
    {
        // looked at once more right before the start: a trigger fired earlier in the same second may have disarmed it,
        // and a key aimed at a disarmed cue starts nothing (fireSequence would pass on to the next cue)
        status (ko ("비활성 큐, 트리거 무시: ") + cueLabel (index, list->get (index)));
        return;
    }

    fireSequence (*list, index, audition);
}

bool CueController::handleHotkey (const juce::KeyPress& key)
{
    const auto description = key.getTextDescription();

    if (description.isEmpty())
        return false;

    // the match is found first and fired after the walk: the start may switch lists (a goto in the sequence), which
    // would pull the list - and the cue reference - out from under the walk
    juce::Uuid matchId = juce::Uuid::null();
    juce::String label;
    bool armed = true;

    document.forEachList ([&] (CueList& cues)   // hotkeys reach into every list and cart
    {
        if (! matchId.isNull())
            return;   // one key fires one cue (the loader clears duplicates; this guards a live edit race)

        for (int i = 0; i < cues.size(); ++i)
        {
            const auto& cue = cues.get (i);

            if (cue.hotkey.isNotEmpty() && juce::KeyPress::createFromDescription (cue.hotkey) == key)
            {
                matchId = cue.id;
                label = cueLabel (i, cue);
                armed = cue.armed;
                return;
            }
        }
    });

    if (matchId.isNull())
        return false;

    if (! armed)
    {
        status (ko ("비활성 큐, 핫키 무시: ") + label);   // GO passes over a disarmed cue; a key aimed at it starts nothing (not the next cue either)
        return true;
    }

    status (ko ("핫키: ") + label);   // before the start: what goes wrong in it (a missing file, a panic) overwrites this
    fireSequenceById (matchId, false);   // hotkeys do not move the playhead
    return true;
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

    // every second since the last check is examined (a busy message thread - a long file open, a plugin scan - must
    // not skip 12:00:00); after a longer gap (sleep, a clock change) only the current second counts
    const juce::int64 catchUp = (juce::int64) wallClockCatchUpSeconds;
    juce::int64 from = lastWallClockSecond < 0 || second - lastWallClockSecond > catchUp || second < lastWallClockSecond ? second : lastWallClockSecond + 1;
    lastWallClockSecond = second;

    // the matches are collected first and fired after the walk: a start may switch lists (a goto), which would pull
    // the list out from under the walk
    struct Due { juce::Uuid id; juce::String label; bool armed; };
    std::vector<Due> due;

    for (juce::int64 s = from; s <= second; ++s)
    {
        const juce::Time t (s * 1000);
        const int hour = t.getHours(), minute = t.getMinutes(), sec = t.getSeconds();
        const int dayBit = 1 << t.getDayOfWeek();   // 0 = Sunday

        document.forEachList ([&] (CueList& cues)   // every list and cart
        {
            for (int i = 0; i < cues.size(); ++i)
            {
                const auto& cue = cues.get (i);
                const auto& wc = cue.wallClock;

                if (wc.enabled && wc.hour == hour && wc.minute == minute && wc.second == sec && (wc.daysMask & dayBit) != 0)
                    due.push_back ({ cue.id, cueLabel (i, cue), cue.armed });
            }
        });
    }

    for (const auto& d : due)
    {
        if (! d.armed)
        {
            status (ko ("비활성 큐, 시간 트리거 무시: ") + d.label);   // a disarmed cue starts nothing - not the next cue either
            continue;
        }

        status (ko ("시간 트리거: ") + d.label);
        fireSequenceById (d.id, false);
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
    explicitStartForNextPlay = true;
    const auto result = preview (false);
    startOffsetForNextPlay = 0.0;
    explicitStartForNextPlay = false;
    return result;
}

bool CueController::togglePause()
{
    const auto id = resolveTarget (false);

    if (id.isNull())
        return false;

    const int index = document.cues.indexOf (id);
    const juce::String label = index >= 0 ? cueLabel (index, document.cues.get (index)) : juce::String();

    if (isPanicLatched())
    {
        status (ko ("전체 정지 진행 중: 재개하지 않음"));
        return false;
    }

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

bool CueController::resumeCue (const juce::Uuid& id)
{
    if (id.isNull())
        return false;

    if (isPanicLatched())
    {
        status (ko ("전체 정지 진행 중: 재개하지 않음"));
        return false;
    }

    if (! engine.isPaused (id))
        return false;

    engine.resume (id);
    return true;
}

void CueController::pauseCue (const juce::Uuid& id)
{
    if (! id.isNull())
        engine.pause (id);
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
        panicLatchUntil = now + 0.1;   // the 5 ms gate close, with margin: nothing starts on top of the stop
        status (ko ("전체 즉시 정지"));
    }
    else
    {
        const double seconds = document.settings.panicSeconds;
        engine.fadeOutAndStopAll ((int) std::lround (seconds * 1000.0));
        panicLatchUntil = now + seconds + 0.25;   // the fade, then the gate's 200 ms close: nothing starts until both are over
        status (ko ("전체 페이드 정지 (") + juce::String (seconds, 1) + ko ("초)"));
    }

    lastPanicTime = now;
}

bool CueController::isPanicLatched() const
{
    return clock() < panicLatchUntil;
}

void CueController::hardStopAll()
{
    fadeRunner.stopAll();
    cancelPending();
    playlists.clear();
    waits.clear();
    engine.stopAll();
    panicLatchUntil = -1.0e9;   // a reset (new / open project), not a panic: the next start is welcome
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
    lastWallClockSecond = -1;   // the new project's clocks start from the current second: nothing from before it, nothing skipped
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
