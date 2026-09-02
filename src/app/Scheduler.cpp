#include "app/Scheduler.h"

#include <algorithm>

namespace gocue
{

Scheduler::Scheduler (Clock c) : clock (std::move (c))
{
    if (! clock)
        clock = [] { return juce::Time::getMillisecondCounterHiRes() * 0.001; };
}

Scheduler::~Scheduler()
{
    stopTimer();
}

int Scheduler::schedule (double atSeconds, std::function<void()> action)
{
    Entry entry;
    entry.id = nextId++;
    entry.at = atSeconds;
    entry.action = std::move (action);
    entries.push_back (std::move (entry));
    return entries.back().id;
}

int Scheduler::watch (std::function<bool()> condition, std::function<void()> action)
{
    Entry entry;
    entry.id = nextId++;
    entry.at = 0.0;
    entry.action = std::move (action);
    entry.condition = std::move (condition);
    entries.push_back (std::move (entry));
    return entries.back().id;
}

void Scheduler::cancel (int id)
{
    entries.erase (std::remove_if (entries.begin(), entries.end(), [id] (const Entry& e) { return e.id == id; }), entries.end());

    if (inTick)
        cancelledDuringTick.insert (id);
}

void Scheduler::cancelAll()
{
    for (const auto& e : entries)
        if (inTick)
            cancelledDuringTick.insert (e.id);

    if (inTick)
        cancelAllDuringTick = true;   // the rest of this tick's due list is dropped too

    entries.clear();
}

bool Scheduler::isPending (int id) const noexcept
{
    for (const auto& e : entries)
        if (e.id == id)
            return true;

    return false;
}

void Scheduler::tick()
{
    if (inTick)   // an action ticked us re-entrantly: the outer tick already handles everything
        return;

    const double t = clock();
    std::vector<Entry> due;

    // 1. timed entries whose time has come, in time order (stable: insertion order for equal times)
    for (auto it = entries.begin(); it != entries.end();)
    {
        if (! it->condition && it->at <= t)
        {
            due.push_back (std::move (*it));
            it = entries.erase (it);
        }
        else
        {
            ++it;
        }
    }

    std::stable_sort (due.begin(), due.end(), [] (const Entry& a, const Entry& b) { return a.at < b.at; });

    inTick = true;
    cancelledDuringTick.clear();
    cancelAllDuringTick = false;

    for (auto& e : due)
    {
        if (cancelAllDuringTick)
            break;

        if (cancelledDuringTick.count (e.id) != 0)
            continue;

        if (e.action)
            e.action();
    }

    // 2. watches, evaluated after the timed starts so "cue A has ended" is not mistaken for "A never started"
    if (! cancelAllDuringTick)
    {
        std::vector<Entry> watches;

        for (auto it = entries.begin(); it != entries.end();)
        {
            if (it->condition && it->condition())
            {
                watches.push_back (std::move (*it));
                it = entries.erase (it);
            }
            else
            {
                ++it;
            }
        }

        for (auto& e : watches)
        {
            if (cancelAllDuringTick)
                break;

            if (cancelledDuringTick.count (e.id) != 0)
                continue;

            if (e.condition && ! e.condition())
            {
                entries.push_back (std::move (e));   // an earlier action changed its mind: keep watching
                continue;
            }

            if (e.action)
                e.action();
        }
    }

    inTick = false;
    cancelledDuringTick.clear();
    cancelAllDuringTick = false;
}

} // namespace gocue
