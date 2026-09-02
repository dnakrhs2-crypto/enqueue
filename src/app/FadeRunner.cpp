#include "app/FadeRunner.h"

#include <algorithm>
#include <cmath>

namespace gocue
{

namespace
{
    juce::String ko (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

FadeRunner::FadeRunner (AudioEngine& e, ProjectDocument& d)
    : engine (e), document (d)
{
    clock = [] { return juce::Time::getMillisecondCounterHiRes() * 0.001; };
}

FadeRunner::~FadeRunner()
{
    stopTimer();
}

bool FadeRunner::readState (const juce::Uuid& targetId, const FadeCueData& data, State& out) const
{
    AudioEngine::LiveState live;

    if (! engine.getLiveState (targetId, live))
        return false;

    out.mainDb = live.gainDb;
    out.levels = live.levels;
    out.trim = live.trim;
    out.rate = live.rate;
    out.params.assign (data.params.size(), 0.0f);

    if (auto* chain = engine.findCueChain (targetId))
    {
        for (size_t i = 0; i < data.params.size(); ++i)
        {
            const auto& p = data.params[i];

            if (p.slot < 0 || p.slot >= chain->getNumSlots())
                continue;

            if (auto* plugin = chain->getSlot (p.slot).plugin.get())
            {
                const auto& parameters = plugin->getParameters();

                if (p.parameter >= 0 && p.parameter < parameters.size())
                    out.params[i] = parameters[p.parameter]->getValue();
            }
        }
    }

    return true;
}

void FadeRunner::writeState (const juce::Uuid& targetId, const FadeCueData& data, const State& state, bool levels, bool rate, bool params)
{
    if (levels)
    {
        engine.setLiveGainDb (targetId, state.mainDb);
        engine.setLiveLevels (targetId, state.levels, state.trim);
    }

    if (rate)
        engine.setLiveRate (targetId, state.rate);

    if (params)
    {
        if (auto* chain = engine.findCueChain (targetId))
        {
            for (size_t i = 0; i < data.params.size() && i < state.params.size(); ++i)
            {
                const auto& p = data.params[i];

                if (! p.active || p.slot < 0 || p.slot >= chain->getNumSlots())
                    continue;

                if (auto* plugin = chain->getSlot (p.slot).plugin.get())
                {
                    const auto& parameters = plugin->getParameters();

                    if (p.parameter >= 0 && p.parameter < parameters.size())
                        parameters[p.parameter]->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, state.params[i]));
                }
            }
        }
    }
}

bool FadeRunner::start (const Cue& fadeCue, juce::String* error)
{
    if (! fadeCue.isFade())
        return false;

    const auto& data = fadeCue.fade;

    if (data.targetId.isNull() || document.cues.findById (data.targetId) == nullptr)
    {
        if (error != nullptr)
            *error = ko ("페이드 큐에 대상이 없습니다: ") + fadeCue.name;

        return false;
    }

    Active a;
    a.fadeId = fadeCue.id;
    a.targetId = data.targetId;
    a.instance = engine.getStartOrder (data.targetId);
    a.startTime = clock();
    a.duration = juce::jmax (0.0, data.durationSeconds);
    a.data = data;

    if (a.instance < 0 || ! readState (a.targetId, a.data, a.from))
    {
        if (error != nullptr)
            *error = ko ("페이드 대상이 재생 중이 아닙니다: ") + fadeCue.name;

        return false;
    }

    // goals: absolute values, or offsets from the current state, for the active cells only
    a.to = a.from;
    const double maxDb = document.settings.maxLevelDb;
    const double minDb = document.settings.minLevelDb;
    auto goal = [&] (double from, double value) -> double
    {
        if (data.relative)
        {
            if (LevelMatrix::isSilent (from))
                return from;   // silence stays silent under a relative fade

            return juce::jlimit (minDb, maxDb, from + value);
        }

        return LevelMatrix::isSilent (value) || value < minDb ? LevelMatrix::silentDb : juce::jmin (maxDb, value);
    };

    if (data.fadeLevels)
    {
        LevelMatrix goals = data.levels;
        goals.resize (a.from.levels.numInputs(), a.from.levels.numOutputs());
        a.data.resizeActive (goals.numInputs(), goals.numOutputs());

        if (data.mainActive)
            a.to.mainDb = data.relative ? juce::jlimit (Cue::minGainDb, Cue::maxGainDb, a.from.mainDb + data.mainDb)
                                        : juce::jlimit (Cue::minGainDb, Cue::maxGainDb, data.mainDb);

        for (int i = 0; i < goals.numInputs(); ++i)
        {
            if (a.data.isInputActive (i))
                a.to.levels.inputDb[(size_t) i] = goal (a.from.levels.inputDb[(size_t) i], goals.inputDb[(size_t) i]);

            for (int o = 0; o < goals.numOutputs(); ++o)
                if (a.data.isCrosspointActive (i, o))
                    a.to.levels.crosspointDb[(size_t) i][(size_t) o] = goal (a.from.levels.crosspointDb[(size_t) i][(size_t) o], goals.crosspointDb[(size_t) i][(size_t) o]);
        }

        for (int o = 0; o < goals.numOutputs(); ++o)
            if (a.data.isOutputActive (o))
                a.to.levels.outputDb[(size_t) o] = goal (a.from.levels.outputDb[(size_t) o], goals.outputDb[(size_t) o]);
    }

    if (data.fadeRate)
        a.to.rate = juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, data.relative ? a.from.rate * data.rate : data.rate);

    for (size_t i = 0; i < data.params.size(); ++i)
        if (data.params[i].active)
            a.to.params[i] = data.relative ? juce::jlimit (0.0f, 1.0f, a.from.params[i] + data.params[i].value) : data.params[i].value;

    // a fade that is already running restarts from where the target is now
    fades.erase (std::remove_if (fades.begin(), fades.end(), [&] (const Active& f) { return f.fadeId == a.fadeId; }), fades.end());

    // the cells this fade drives now belong to it: an older fade on the same target lets go of them (otherwise it
    // would take them back the moment this one ends, with a jump)
    for (auto& other : fades)
        if (other.targetId == a.targetId && takeLanes (a.data, other.data))
            other.duration = -1.0;   // nothing left for it to do

    fades.erase (std::remove_if (fades.begin(), fades.end(), [] (const Active& f) { return f.duration < 0.0; }), fades.end());

    RevertEntry entry;
    entry.targetId = a.targetId;
    entry.instance = a.instance;
    entry.data = a.data;
    entry.from = a.from;
    revertStack.push_back (std::move (entry));

    if ((int) revertStack.size() > maxRevertStates)
        revertStack.erase (revertStack.begin());

    fades.push_back (std::move (a));
    tick();   // apply the first step at once
    return true;
}

bool FadeRunner::hasAnyLane (const FadeCueData& data)
{
    if (data.fadeLevels)
    {
        if (data.mainActive)
            return true;

        for (int i = 0; i < data.levels.numInputs(); ++i)
        {
            if (data.isInputActive (i))
                return true;

            for (int o = 0; o < data.levels.numOutputs(); ++o)
                if (data.isCrosspointActive (i, o))
                    return true;
        }

        for (int o = 0; o < data.levels.numOutputs(); ++o)
            if (data.isOutputActive (o))
                return true;
    }

    if (data.fadeRate)
        return true;

    for (const auto& p : data.params)
        if (p.active)
            return true;

    return false;
}

bool FadeRunner::takeLanes (const FadeCueData& winner, FadeCueData& other)
{
    if (winner.fadeLevels && other.fadeLevels)
    {
        if (winner.mainActive)
            other.mainActive = false;

        const int inputs = juce::jmin (winner.levels.numInputs(), other.levels.numInputs());
        const int outputs = juce::jmin (winner.levels.numOutputs(), other.levels.numOutputs());

        for (int i = 0; i < inputs; ++i)
        {
            if (winner.isInputActive (i))
                other.setInputActive (i, false);

            for (int o = 0; o < outputs; ++o)
                if (winner.isCrosspointActive (i, o))
                    other.setCrosspointActive (i, o, false);
        }

        for (int o = 0; o < outputs; ++o)
            if (winner.isOutputActive (o))
                other.setOutputActive (o, false);
    }

    if (winner.fadeRate && other.fadeRate)
        other.fadeRate = false;

    for (const auto& wp : winner.params)
        if (wp.active)
            for (auto& op : other.params)
                if (op.slot == wp.slot && op.parameter == wp.parameter)
                    op.active = false;

    return ! hasAnyLane (other);
}

void FadeRunner::resetSession()
{
    fades.clear();
    revertStack.clear();
}

void FadeRunner::apply (Active& a, double t)
{
    State cur;

    if (! readState (a.targetId, a.data, cur))
        return;   // the target is gone: nothing to fade

    const double c = a.data.curve.completion (t);
    const double minDb = document.settings.minLevelDb;
    const auto& curveRef = a.data.curve;
    // a level that reaches the floor is silence for the matrix (-inf), not "-60 dB"
    auto curve = [&curveRef, minDb] (double from, double to, double tt) -> double
    {
        const double v = curveRef.interpolateDb (from, to, tt, minDb);
        return v <= minDb ? LevelMatrix::silentDb : v;
    };

    if (a.data.fadeLevels)
    {
        if (a.data.mainActive)
            cur.mainDb = curveRef.interpolateDb (a.from.mainDb, a.to.mainDb, t, Cue::minGainDb);

        cur.levels.resize (a.from.levels.numInputs(), a.from.levels.numOutputs());

        for (int i = 0; i < cur.levels.numInputs(); ++i)
        {
            if (a.data.isInputActive (i))
                cur.levels.inputDb[(size_t) i] = curve (a.from.levels.inputDb[(size_t) i], a.to.levels.inputDb[(size_t) i], t);

            for (int o = 0; o < cur.levels.numOutputs(); ++o)
                if (a.data.isCrosspointActive (i, o))
                    cur.levels.crosspointDb[(size_t) i][(size_t) o] = curve (a.from.levels.crosspointDb[(size_t) i][(size_t) o],
                                                                              a.to.levels.crosspointDb[(size_t) i][(size_t) o], t);
        }

        for (int o = 0; o < cur.levels.numOutputs(); ++o)
            if (a.data.isOutputActive (o))
                cur.levels.outputDb[(size_t) o] = curve (a.from.levels.outputDb[(size_t) o], a.to.levels.outputDb[(size_t) o], t);
    }

    if (a.data.fadeRate && a.from.rate > 0.0 && a.to.rate > 0.0)
        cur.rate = a.from.rate * std::pow (a.to.rate / a.from.rate, c);   // geometric: equal ratio per second

    for (size_t i = 0; i < a.data.params.size() && i < cur.params.size(); ++i)
        if (a.data.params[i].active)
            cur.params[i] = a.from.params[i] + (a.to.params[i] - a.from.params[i]) * (float) c;

    writeState (a.targetId, a.data, cur, a.data.fadeLevels, a.data.fadeRate, ! a.data.params.empty());
}

void FadeRunner::tick()
{
    if (fades.empty())
        return;

    const double now = clock();
    std::vector<juce::Uuid> stopTargets;

    for (auto& a : fades)
    {
        if (engine.getStartOrder (a.targetId) != a.instance)
        {
            a.duration = -1.0;   // the target stopped or was restarted: this fade belonged to the old instance
            continue;
        }

        const double t = a.duration > 0.0 ? juce::jlimit (0.0, 1.0, (now - a.startTime) / a.duration) : 1.0;
        apply (a, t);

        if (t >= 1.0)
        {
            if (a.data.stopTargetWhenDone)
                stopTargets.push_back (a.targetId);

            a.duration = -1.0;   // mark done
        }
    }

    // a fade that stops its target takes every other fade on that target with it
    for (const auto& id : stopTargets)
        for (auto& a : fades)
            if (a.targetId == id)
                a.duration = -1.0;

    fades.erase (std::remove_if (fades.begin(), fades.end(), [] (const Active& f) { return f.duration < 0.0; }), fades.end());

    for (const auto& id : stopTargets)
        engine.stop (id);
}

void FadeRunner::stop (const juce::Uuid& fadeCueId)
{
    fades.erase (std::remove_if (fades.begin(), fades.end(), [&] (const Active& f) { return f.fadeId == fadeCueId; }), fades.end());
}

void FadeRunner::stopAll()
{
    fades.clear();
}

bool FadeRunner::isRunning (const juce::Uuid& fadeCueId) const
{
    for (const auto& f : fades)
        if (f.fadeId == fadeCueId)
            return true;

    return false;
}

std::vector<FadeRunner::Info> FadeRunner::getRunning() const
{
    std::vector<Info> result;
    const double now = clock();

    for (const auto& f : fades)
        result.push_back ({ f.fadeId, f.targetId, juce::jlimit (0.0, f.duration, now - f.startTime), f.duration });

    return result;
}

bool FadeRunner::revertLast()
{
    while (! revertStack.empty())
    {
        auto entry = revertStack.back();
        revertStack.pop_back();

        if (engine.getStartOrder (entry.targetId) != entry.instance)
            continue;   // that instance is over (or the cue restarted): try the one before

        // the fades still running on that target are cancelled first
        fades.erase (std::remove_if (fades.begin(), fades.end(), [&] (const Active& f) { return f.targetId == entry.targetId; }), fades.end());

        // only what the fade owned goes back: its active cells, its rate, its parameters
        State cur;

        if (! readState (entry.targetId, entry.data, cur))
            continue;

        const auto& d = entry.data;

        if (d.fadeLevels)
        {
            if (d.mainActive)
                cur.mainDb = entry.from.mainDb;

            const int inputs = juce::jmin (cur.levels.numInputs(), entry.from.levels.numInputs());
            const int outputs = juce::jmin (cur.levels.numOutputs(), entry.from.levels.numOutputs());

            for (int i = 0; i < inputs; ++i)
            {
                if (d.isInputActive (i))
                    cur.levels.inputDb[(size_t) i] = entry.from.levels.inputDb[(size_t) i];

                for (int o = 0; o < outputs; ++o)
                    if (d.isCrosspointActive (i, o))
                        cur.levels.crosspointDb[(size_t) i][(size_t) o] = entry.from.levels.crosspointDb[(size_t) i][(size_t) o];
            }

            for (int o = 0; o < outputs; ++o)
                if (d.isOutputActive (o))
                    cur.levels.outputDb[(size_t) o] = entry.from.levels.outputDb[(size_t) o];
        }

        if (d.fadeRate)
            cur.rate = entry.from.rate;

        for (size_t i = 0; i < d.params.size() && i < cur.params.size() && i < entry.from.params.size(); ++i)
            if (d.params[i].active)
                cur.params[i] = entry.from.params[i];

        writeState (entry.targetId, d, cur, d.fadeLevels, d.fadeRate, ! d.params.empty());
        return true;
    }

    return false;
}

} // namespace gocue
