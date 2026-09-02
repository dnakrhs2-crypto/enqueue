#pragma once

#include <juce_events/juce_events.h>

#include <functional>
#include <set>
#include <vector>

namespace gocue
{

/** Message-thread timeline for pre-waits, post-waits, auto-follows, wait cues and wall-clock triggers.
    Timed entries run when their time comes; watches run (once) when their condition turns true.
    tick() is driven by a 1 ms juce::Timer in the app and called by hand in tests (with a fake clock). */
class Scheduler : private juce::Timer
{
public:
    using Clock = std::function<double()>;   // seconds

    /** @param clock  seconds source; defaults to juce::Time::getMillisecondCounterHiRes() / 1000. */
    explicit Scheduler (Clock clock = {});
    ~Scheduler() override;

    /** Runs 'action' once 'atSeconds' (clock time) has passed. Returns an id > 0. */
    int schedule (double atSeconds, std::function<void()> action);
    /** Runs 'action' once, on the first tick where 'condition' returns true. */
    int watch (std::function<bool()> condition, std::function<void()> action);
    void cancel (int id);
    void cancelAll();

    /** Runs everything that is due, in time order. Actions may schedule or cancel entries. */
    void tick();
    int pendingCount() const noexcept { return (int) entries.size(); }
    double now() const { return clock(); }

    void startTicking (int intervalMs = 1) { startTimer (intervalMs); }
    void stopTicking() { stopTimer(); }

private:
    struct Entry
    {
        int id = 0;
        double at = 0.0;
        std::function<void()> action;
        std::function<bool()> condition;   // set for watches
    };

    void timerCallback() override { tick(); }

    Clock clock;
    std::vector<Entry> entries;
    std::set<int> cancelledDuringTick;
    int nextId = 1;
    bool inTick = false;
};

} // namespace gocue
