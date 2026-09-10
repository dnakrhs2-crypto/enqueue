#include "record/VideoCfrScheduler.h"
#include "TestSupport.h"
#include <cmath>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
CfrCounters simulate(Rational native, Rational project, unsigned seconds)
{
    VideoCfrScheduler scheduler(native, project);
    std::int64_t input = 0, previous = -1;
    const auto total = VideoCfrScheduler::frameCount(static_cast<std::int64_t>(seconds) * 10000000, project);
    for (std::int64_t k = 0; k < total; ++k)
    {
        const auto grid = VideoCfrScheduler::gridTime(k, project);
        // Independently ensure a bracketing candidate, never feed an unbounded future.
        while (VideoCfrScheduler::gridTime(input, native) <= grid + VideoCfrScheduler::gridTime(1, native))
        {
            scheduler.push({static_cast<std::uint64_t>(input + 1), VideoCfrScheduler::gridTime(input, native), static_cast<int>(input % 16)}); ++input;
        }
        const auto selected = scheduler.select(grid + VideoCfrScheduler::gridTime(1, native) + 1);
        require(selected.has_value() && selected->pts == k && selected->pts > previous, "Every CFR PTS appears exactly once, in order");
        const auto nominal = VideoCfrScheduler::nearestNativeIndex(k, native, project);
        require(selected->input.sourceId == static_cast<std::uint64_t>(nominal + 1), "Absolute rational nearest-frame oracle");
        require(scheduler.size() <= 3, "Selection retires past candidates, memory does not grow with duration");
        previous = selected->pts;
    }
    return scheduler.counters();
}
}
int runCfrSchedulerTests()
{
    Suite s;
    s.test("alignment: delayed camera delivery selects the nearest source without moving the CFR grid", []
    {
        const auto run = [](bool accountForDelivery)
        {
            VideoCfrScheduler c({60,1}, {60,1}); c.push({14,0,0});
            unsigned input = 1, errors = 0;
            // Measured fixture phase 1.5208 ms, delivery ~18.672 ms. All input
            // pixels/IDs arrive in order; a one-period wall deadline is too soon.
            for (std::int64_t now = 185211; now < 11000000 && c.nextPts() < 60; now += 1000)
            {
                while (true)
                {
                    const auto capture = 15208 + VideoCfrScheduler::gridTime(input - 1, {60,1});
                    const auto delivered = capture + 186720;
                    if (delivered > now) break;
                    c.push({14 + input, capture, int(input % 16)}, accountForDelivery ? std::optional<std::int64_t>(delivered) : std::nullopt);
                    ++input;
                }
                if (const auto selected = c.select(now))
                {
                    const auto expected = selected->pts == 0 ? 14 : 15 + selected->pts;
                    if (selected->input.sourceId != std::uint64_t(expected)) ++errors;
                    require(selected->grid100ns == VideoCfrScheduler::gridTime(selected->pts, {60,1}), "Delivery compensation moved output PTS");
                }
                require(c.size() <= 3, "Delayed delivery grew the CFR queue");
            }
            require(c.nextPts() == 60, "Delayed camera fixture did not finish"); return errors;
        };
        const auto baseline = run(false), corrected = run(true);
        std::cout << "CFR alignment: unadjusted deadline wrong frames=" << baseline << "/60, delivery-aware=" << corrected << "/60\n";
        require(baseline == 59 && corrected == 0, "Measured camera delivery did not preserve nearest-frame alignment");
    });
    s.test("alignment: delivery grace remains bounded on camera loss and stop drains immediately", []
    {
        VideoCfrScheduler c({60,1}, {60,1}); c.push({1,0,0}, 200000); require(bool(c.select(200000)), "Select first delivered frame");
        const auto deadline = VideoCfrScheduler::gridTime(1, {60,1}) + 166667 + 200000;
        require(!c.select(deadline - 1) && bool(c.select(deadline)), "Delivery grace did not expire at the bounded deadline");
        require(bool(c.select(0, true)), "Stop waited for missing future camera delivery");
        require(c.counters().maximumDeliveryDelay100ns == 200000, "Delivery diagnostic lost its measured bound");
    });
    s.test("30 -> 60 repeats and 60 -> 30 omissions are native conversion", []
    {
        const auto up = simulate({30,1}, {60,1}, 60);
        require(up.outputs == 3600 && up.repeated == 1800 && !up.omitted, "30 -> 60 exact repetition count");
        require(up.reasons[0].repeated == up.repeated && !up.reasons[1].repeated, "Nominal repeats are not clock/loss");
        const auto down = simulate({60,1}, {30,1}, 60);
        require(down.outputs == 1800 && !down.repeated && down.omitted == 1799 && down.reasons[0].omitted == 1799, "Unselected native frames before last grid counted once; post-stop candidates excluded");
    });
    s.test("59.94 -> 60 three-hour absolute rational oracle, no accumulated rounding", []
    {
        const auto result = simulate({60000,1001}, {60,1}, 3 * 3600);
        require(result.outputs == 648000 && result.repeated == 647 && result.reasons[0].repeated == 647 && !result.reasons[1].repeated, "Three-hour 59.94 cadence conversion");
        require(VideoCfrScheduler::gridTime(648000, {60,1}) == 108000000000LL, "Three-hour grid endpoint exact");
        require(VideoCfrScheduler::frameCount(10000001, {60,1}) == 61, "Subframe logical end rounds up only video length");
    });
    s.test("bounded future wait, closest frame, tie older, late source cannot force distant future", []
    {
        VideoCfrScheduler c({60,1}, {60,1}); c.push({1,0,0});
        require(c.select(0)->input.sourceId == 1, "Origin selected immediately");
        require(!c.select(200000), "Await missing next candidate for <=one native period");
        c.push({2,200000,1});
        require(c.select(210000)->input.sourceId == 2, "Nearest future frame selected");
        c.push({3,10000000,2});
        require(c.select(500001)->input.sourceId == 2, "Distant future frame outside one-period eligibility");
    });
    s.test("capture loss, encode loss and clock correction have separate causes", []
    {
        VideoCfrScheduler c({60,1}, {60,1}); c.push({1,0,0}); c.select(0);
        c.noteLoss(CfrReason::captureLoss, 1); require(c.select(333334)->reason == CfrReason::captureLoss, "Missing decoded input repeat");
        c.noteLoss(CfrReason::encodeLoss, 2); require(c.select(500001)->reason == CfrReason::encodeLoss, "Full encoder pool repeat");
        require(c.counters().reasons[2].missing == 1 && c.counters().reasons[3].missing == 2, "Missing counts do not double-count repeated output");
        VideoCfrScheduler jitter({60,1}, {60,1}); jitter.push({1,0,0}); jitter.select(0);
        require(jitter.select(333334)->reason == CfrReason::clockCorrection, "Unproven device cadence is not software capture loss");
    });
    s.test("PTS/ID regression and queue capacity fail explicitly", []
    {
        VideoCfrScheduler c({60,1}, {60,1}); c.push({1,0,0});
        rejects([&]{ c.push({2,0,1}); }); rejects([&]{ c.push({1,1,1}); });
        for (int i = 1; i < 16; ++i) c.push({static_cast<unsigned>(i + 1), i * 166667LL, i});
        rejects([&]{ c.push({17,3000000,0}); });
        size_t count = 0; c.releaseAll(count); require(count == 16 && c.size() == 0, "All bounded candidate ownership returned");
        rejects([]{ VideoCfrScheduler invalid({60000,1001}, {60000,1001}); });
        rejects([]{ VideoCfrScheduler::gridTime(INT64_MAX, {60,1}); });
    });
    s.test("MF PTS mapper has independent arrival deadline origin", []
    {
        MfPtsTimeMapper map(10000000); FrameStamp first; first.pts100ns = 123000000; first.callback = 456000000;
        require(map.map(first) == 0 && map.now(first.callback) == 0, "MF and QPC epochs independent");
        auto second = first; second.pts100ns += 166667; second.callback += 180000;
        require(map.map(second) == 166667 && map.now(second.callback) == 180000, "Arrival jitter does not rewrite native frame time");
        first.pts100ns -= 1; rejects([&]{ map.map(first); });
    });
    s.test("capture gate excludes first-second cadence and separates device observations", []
    {
        auto telemetry = std::make_unique<CaptureTelemetry>(Rational{60,1}); const auto origin = telemetry->frequency * 10;
        telemetry->firstCallbackQpc.store(origin);
        require(!telemetry->afterWarmup(origin + telemetry->frequency - 1) && telemetry->afterWarmup(origin + telemetry->frequency), "Warmup boundary exactly one second");
        telemetry->loss(LossReason::sourceCadenceGap); telemetry->loss(LossReason::previewStall);
        require(telemetry->softwareLossFree(), "Device/stall observations alone do not imply software loss");
        telemetry->loss(LossReason::lateQueueDiscard); require(!telemetry->softwareLossFree(), "Software queue discard fails gate");
    });
    return s.result("RecorderCfrScheduler");
}
