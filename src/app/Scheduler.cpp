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

    entries.clear();
}

void Scheduler::tick()
{
    if (inTick)   // an action ticked us re-entrantly: the outer tick already handles everything
        return;

    const double t = clock();
    std::vector<Entry> due;

    for (auto it = entries.begin(); it != entries.end();)
    {
        const bool ready = it->condition ? it->condition() : it->at <= t;

        if (ready)
        {
            due.push_back (std::move (*it));
            it = entries.erase (it);
        }
        else
        {
            ++it;
        }
    }

    if (due.empty())
        return;

    // timed entries in time order (stable: insertion order for equal times), watches after them
    std::stable_sort (due.begin(), due.end(), [] (const Entry& a, const Entry& b)
    {
        const bool aWatch = (bool) a.condition, bWatch = (bool) b.condition;

        if (aWatch != bWatch)
            return ! aWatch;

        return a.at < b.at;
    });

    inTick = true;
    cancelledDuringTick.clear();

    for (auto& e : due)
    {
        if (cancelledDuringTick.count (e.id) != 0)
            continue;

        if (e.action)
            e.action();
    }

    inTick = false;
    cancelledDuringTick.clear();
}

} // namespace gocue
