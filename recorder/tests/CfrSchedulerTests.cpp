#include "record/VideoCfrScheduler.h"
#include "TestSupport.h"
#include <algorithm>
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
            unsigned input = 1, errors = 0, phaseFrames = 0; std::int64_t phaseTotal = 0;
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
                    if (selected->pts) { phaseTotal += selected->input.time100ns - selected->grid100ns; ++phaseFrames; }
                }
                require(c.size() <= 3, "Delayed delivery grew the CFR queue");
            }
            require(c.nextPts() == 60, "Delayed camera fixture did not finish");
            std::cout << "CFR measured-delay fixture: deliveryAware=" << accountForDelivery
                      << ", meanSourceMinusGridMs=" << double(phaseTotal) / phaseFrames / 10000.0 << '\n';
            return errors;
        };
        const auto baseline = run(false), corrected = run(true);
        std::cout << "CFR alignment: unadjusted deadline wrong frames=" << baseline << "/60, delivery-aware=" << corrected << "/60\n";
        require(baseline == 59 && corrected == 0, "Measured camera delivery did not preserve nearest-frame alignment");
    });
    s.test("alignment: delivery grace remains bounded on camera loss and stop drains immediately", []
    {
        VideoCfrScheduler c({60,1}, {60,1}); c.push({1,0,0}, 200000); require(bool(c.select(200000)), "Select first delivered frame");
        const auto deadline = VideoCfrScheduler::gridTime(1, {60,1}) + 2 * 166667;
        require(!c.select(deadline - 1) && bool(c.select(deadline)), "Delivery grace did not expire at the bounded deadline");
        require(bool(c.select(0, true)), "Stop waited for missing future camera delivery");
        require(c.counters().maximumDeliveryDelay100ns == 200000, "Delivery diagnostic lost its measured bound");
    });
    s.test("alignment: 200ms spike, normal recovery and input loss obey the total wait cap and expire the spike", []
    {
        for (unsigned fps : {30u, 60u})
        {
            const Rational rate{fps, 1}; VideoCfrScheduler c(rate, rate);
            const auto period = (10000000LL + fps - 1) / fps;
            const auto cap = std::min(2 * period, 500000LL);
            c.push({1,0,0}, 2000000);
            while (c.select(2000000)) {} // Catch up after the actual 200ms stall.
            c.push({2,2100000,1}, 2200000); // Delivery returns to 10ms.
            while (c.select(2200000)) {}
            auto grid = VideoCfrScheduler::gridTime(c.nextPts(), rate);
            c.noteLoss(CfrReason::captureLoss, 1);
            require(!c.select(grid + cap - 1), "Spike grace ended before its capped deadline");
            const auto lost = c.select(grid + cap);
            require(lost && lost->reason == CfrReason::captureLoss, "Spike exceeded cap or changed existing loss classification");
            // Resume normal delivery for >one second so the old peak expires.
            const auto resumed = grid + cap;
            for (unsigned i = 1; i <= fps * 2; ++i)
            {
                const auto capture = resumed + VideoCfrScheduler::gridTime(i, rate);
                const auto delivered = capture + 100000;
                c.push({2 + i, capture, int(i % 16)}, delivered);
                while (c.select(delivered)) {}
            }
            grid = VideoCfrScheduler::gridTime(c.nextPts(), rate);
            const auto normalDeadline = grid + period + 100000;
            require(!c.select(normalDeadline - 1) && bool(c.select(normalDeadline)), "Recovered input loss still used the expired spike budget");
            require(c.counters().maximumDeliveryDelay100ns == 2000000, "Expiry erased the lifetime spike diagnostic");
            std::cout << "CFR spike recovery: fps=" << fps << ", cappedWaitMs=" << double(cap) / 10000.0
                      << ", recoveredWaitMs=" << double(normalDeadline - grid) / 10000.0 << ", diagnosticMaxMs=200\n";
        }
    });
    s.test("alignment: delivery observations expire while no input arrives", []
    {
        VideoCfrScheduler c({60,1}, {60,1}); c.push({1,0,0}, 2000000);
        require(bool(c.select(2000000)), "Initial spike frame missing");
        // At exactly one second after observing the spike, frame 71's original
        // one-period deadline is due. A still-active grace would block it.
        while (c.nextPts() < 72) require(bool(c.select(12000000)), "Idle input retained an expired delivery observation");
        require(c.counters().maximumDeliveryDelay100ns == 2000000, "Idle expiry erased diagnostic");
    });
    s.test("alignment: reanchor resets only delivery budget and preserves CFR grid, candidates and diagnostics", []
    {
        VideoCfrScheduler c({60,1}, {60,1}); c.push({1,0,0}, 2000000);
        while (c.select(2000000)) {}
        const auto next = c.nextPts(); const auto outputs = c.counters().outputs;
        const auto deadline = VideoCfrScheduler::gridTime(next, {60,1}) + 166667;
        require(!c.select(deadline), "Fixture has no live spike grace to reset");
        c.resetDeliveryDelay();
        require(c.nextPts() == next && c.counters().outputs == outputs && c.size() == 1, "Reanchor reset more than latency observations");
        const auto selected = c.select(deadline);
        require(selected && selected->pts == next && selected->input.sourceId == 1, "Reanchor retained the old delivery budget or moved the grid");
        require(c.counters().maximumDeliveryDelay100ns == 2000000, "Reanchor erased lifetime diagnostic");
    });
    s.test("alignment: up to one native period of normal delivery still selects the nearest future picture", []
    {
        for (const Rational rate : {Rational{30,1}, Rational{60,1}, Rational{30000,1001}, Rational{60000,1001}, Rational{24,1}})
            for (unsigned fraction : {0u, 1u, 2u})
        {
            const Rational project{rate.numerator / rate.denominator > 30 ? 60u : 30u, 1};
            VideoCfrScheduler c(rate, project);
            const auto period = (10000000LL * rate.denominator + rate.numerator - 1) / rate.numerator;
            const auto delay = period * fraction / 2, phase = period * 49 / 100;
            c.push({1,0,0}, 0); unsigned input = 0;
            for (std::int64_t now = 0; now < VideoCfrScheduler::gridTime(65, project) && c.nextPts() < 60; now += 100)
            {
                const auto capture = phase + VideoCfrScheduler::gridTime(input, rate);
                if (capture + delay <= now)
                { c.push({2 + input, capture, int((input + 1) % 16)}, capture + delay); ++input; }
                if (const auto selected = c.select(now))
                {
                    // Brute-force all fixture capture times, including pictures
                    // not delivered yet: an independent nearest-frame oracle.
                    std::uint64_t expected = 1; auto distance = selected->grid100ns;
                    for (unsigned candidate = 0; candidate < 130; ++candidate)
                    {
                        const auto delta = std::abs(phase + VideoCfrScheduler::gridTime(candidate, rate) - selected->grid100ns);
                        if (delta + 1 < distance) { expected = 2 + candidate; distance = delta; }
                    }
                    require(selected->input.sourceId == expected, "Bounded delivery picked the older, more distant picture");
                }
                require(c.size() <= 3, "Normal delivery grew candidate storage");
            }
            require(c.nextPts() == 60, "Normal delayed stream failed to complete");
        }
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
