#include "app/Scheduler.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class SchedulerTests : public juce::UnitTest
{
public:
    SchedulerTests() : juce::UnitTest ("Scheduler", "Enqueue") {}

    void runTest() override
    {
        double now = 0.0;
        Scheduler scheduler ([&now] { return now; });
        juce::StringArray log;

        beginTest ("timed entries run when due, in time order");
        {
            scheduler.schedule (2.0, [&log] { log.add ("two"); });
            scheduler.schedule (1.0, [&log] { log.add ("one"); });
            scheduler.schedule (1.0, [&log] { log.add ("one-b"); });
            expectEquals (scheduler.pendingCount(), 3);

            scheduler.tick();
            expectEquals (log.size(), 0);

            now = 1.0;
            scheduler.tick();
            expectEquals (log.joinIntoString (","), juce::String ("one,one-b"));
            expectEquals (scheduler.pendingCount(), 1);

            now = 5.0;
            scheduler.tick();
            expectEquals (log.joinIntoString (","), juce::String ("one,one-b,two"));
            expectEquals (scheduler.pendingCount(), 0);
            log.clear();
        }

        beginTest ("cancel removes an entry before and during a tick");
        {
            const int a = scheduler.schedule (10.0, [&log] { log.add ("a"); });
            scheduler.schedule (10.0, [&log, &scheduler, a] { log.add ("b"); juce::ignoreUnused (a); });
            scheduler.cancel (a);
            now = 10.0;
            scheduler.tick();
            expectEquals (log.joinIntoString (","), juce::String ("b"));
            log.clear();

            int later = 0;
            scheduler.schedule (20.0, [&log, &scheduler, &later] { log.add ("first"); scheduler.cancel (later); });
            later = scheduler.schedule (20.0, [&log] { log.add ("second"); });
            now = 20.0;
            scheduler.tick();
            expectEquals (log.joinIntoString (","), juce::String ("first"));   // cancelled by an earlier action of the same tick
            log.clear();
        }

        beginTest ("watches fire once when their condition becomes true");
        {
            bool flag = false;
            scheduler.watch ([&flag] { return flag; }, [&log] { log.add ("watched"); });
            scheduler.tick();
            scheduler.tick();
            expectEquals (log.size(), 0);
            flag = true;
            scheduler.tick();
            scheduler.tick();
            expectEquals (log.joinIntoString (","), juce::String ("watched"));
            expectEquals (scheduler.pendingCount(), 0);
            log.clear();
        }

        beginTest ("actions may schedule follow-ups; re-entrant ticks are ignored");
        {
            now = 30.0;
            scheduler.schedule (30.0, [&log, &scheduler, &now]
            {
                log.add ("parent");
                scheduler.schedule (now, [&log] { log.add ("child-now"); });
                scheduler.schedule (now + 1.0, [&log] { log.add ("child-later"); });
                scheduler.tick();   // re-entrant: must not run the children here
            });
            scheduler.tick();
            expectEquals (log.joinIntoString (","), juce::String ("parent"));
            scheduler.tick();
            expectEquals (log.joinIntoString (","), juce::String ("parent,child-now"));
            now = 31.0;
            scheduler.tick();
            expectEquals (log.joinIntoString (","), juce::String ("parent,child-now,child-later"));
            log.clear();
        }

        beginTest ("timed starts run before watches are judged, and a watch re-checks its condition");
        {
            now = 0.0;
            bool started = false;
            int followed = 0;
            scheduler.watch ([&started] { return ! started; }, [&followed] { ++followed; });   // "A has ended" naively
            scheduler.schedule (1.0, [&started] { started = true; });
            now = 1.0;
            scheduler.tick();
            expect (started);
            expectEquals (followed, 0);   // the timed start ran first, so the watch saw A running
            expectEquals (scheduler.pendingCount(), 1);
            started = false;
            scheduler.tick();
            expectEquals (followed, 1);
            expectEquals (scheduler.pendingCount(), 0);

            // cancelAll from an action stops the rest of the tick
            int ran = 0;
            scheduler.schedule (2.0, [&] { ++ran; scheduler.cancelAll(); });
            scheduler.schedule (2.0, [&] { ++ran; });
            const int watched = scheduler.watch ([] { return true; }, [&] { ++ran; });
            expect (scheduler.isPending (watched));
            now = 2.0;
            scheduler.tick();
            expectEquals (ran, 1);
            expect (! scheduler.isPending (watched));
            expectEquals (scheduler.pendingCount(), 0);
        }

        beginTest ("cancelAll clears everything");
        {
            scheduler.schedule (100.0, [&log] { log.add ("x"); });
            scheduler.watch ([] { return true; }, [&log] { log.add ("y"); });
            scheduler.cancelAll();
            now = 200.0;
            scheduler.tick();
            expectEquals (log.size(), 0);
            expectEquals (scheduler.pendingCount(), 0);
        }
    }
};

static SchedulerTests schedulerTests;

} // namespace gocue::tests
