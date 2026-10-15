#include "LiveMixCollabTestHelpers.h"

namespace gocue::tests
{
using namespace collabtest;
class LiveMixCollabPlayoutTests : public juce::UnitTest
{
public:
    LiveMixCollabPlayoutTests() : UnitTest ("LiveMix Collab playout", "LiveMix") {}
    void runTest() override
    {
        beginTest ("ordered and reordered packets, duplicate counted once, late discarded");
        for (bool reversed : { false, true })
        {
            CollabPlayout play; play.prepare (48000, 120);
            std::array<float, 120> left {}, right {};
            play.pull (left.data(), right.data(), 120);
            expect (std::all_of (left.begin(), left.end(), [] (float v) { return v == 0; }));
            for (int i = 0; i < 8; ++i)
            {
                auto p = tone (reversed ? 7 - i : i); p.arrival = (uint64_t) i * 2500000ULL;
                expect (push (play, p));
            }
            expect (! push (play, tone (4)));
            expect (play.getStats().received == 8);
            for (int i = 0; i < 3; ++i) play.pull (left.data(), right.data(), 120);
            expectWithinAbsoluteError (left.back(), 0.25f, 1.0e-5f);
            expectWithinAbsoluteError (right.back(), 0.25f, 1.0e-5f);
            expect (! push (play, tone (0)));
            expect (play.getStats().late == 1);
            play.rebuffer(); play.pull (left.data(), right.data(), 120);
            expect (! push (play, tone (0)));
            expect (play.getStats().late == 2);
        }

        beginTest ("one missing packet fades out and in without a click, loss window expires");
        {
            CollabPlayout play; play.prepare (48000, 120);
            std::array<float, 120> left {}, right {};
            float previous = 0; double maxStep = 0; bool sawLoss = false;
            for (int block = 0; block < 5600; ++block)
            {
                if (block != 800) push (play, tone ((uint64_t) block, 0.5f, 137));
                play.pull (left.data(), right.data(), 120);
                for (float v : left) { if (block >= 790 && block <= 830) maxStep = std::max (maxStep, (double) std::abs (v - previous)); previous = v; }
                if (block == 850) sawLoss = play.getStats().lossPercent10s > 0;
            }
            expect (play.getStats().lost == 1);
            expect (play.getStats().underruns == 0);
            expect (sawLoss);
            expectEquals (play.getStats().lossPercent10s, 0.0);
            expectLessOrEqual (maxStep, 2.0 * 0.5 * std::sin (juce::MathConstants<double>::pi * 137 / 48000) + 0.006);
        }

        beginTest ("empty buffer increases target once, thirty stable seconds reduces it");
        for (uint32_t rate : { 48000u, 44100u })
        {
            CollabPlayout play; play.prepare (rate, 120, rate);
            std::array<float, 120> left {}, right {};
            double before = 0, increased = 0;
            for (int block = 0; block < 15000; ++block)
            {
                if (block < 800 || block >= 840) push (play, tone ((uint64_t) block, 0.25f, 0, true, rate));
                play.pull (left.data(), right.data(), 120);
                if (block == 799) before = play.getStats().targetMs;
                if (block == 839) increased = play.getStats().targetMs;
            }
            expectWithinAbsoluteError (before, packetDurationMs (rate), 0.0001);
            expectWithinAbsoluteError (increased, before + packetDurationMs (rate), 0.0001);
            expect (play.getStats().underruns == 1);
            expectWithinAbsoluteError (play.getStats().targetMs, increased - packetDurationMs (rate) * 0.5, 0.0001);
        }

        beginTest ("first second p99 jitter floor at 3, 10 and 30 ms, measured again after rebuffer");
        for (double jitter : { 3.0, 10.0, 30.0 })
        {
            CollabPlayout play; play.prepare (48000, 120);
            std::array<float, 120> left {}, right {};
            auto run = [&] (double spread, int start)
            {
                std::vector<Packet> packets;
                for (int i = start; i < start + 1200; ++i)
                {
                    auto p = tone ((uint64_t) i);
                    p.arrival += (uint64_t) ((i % 41) / 40.0 * spread * 1.0e6); packets.push_back (p);
                }
                std::sort (packets.begin(), packets.end(), [] (const Packet& a, const Packet& b) { return a.arrival < b.arrival; });
                size_t at = 0;
                for (int block = start; block < start + 1200; ++block)
                {
                    while (at < packets.size() && packets[at].arrival <= (uint64_t) block * 2500000ULL) push (play, packets[at++]);
                    play.pull (left.data(), right.data(), 120);
                }
            };
            run (jitter, 0);
            expectGreaterOrEqual (play.getStats().targetMs, jitter);
            expectLessOrEqual (play.getStats().targetMs, 80.0);
            play.rebuffer(); run (jitter + 5.0, 1200);
            expectGreaterOrEqual (play.getStats().targetMs, jitter + 5.0);
        }

        beginTest ("new sender resets frame epoch, stereo stays separate");
        {
            CollabPlayout play; play.prepare (48000, 120);
            std::array<float, 120> left {}, right {};
            for (int block = 0; block < 200; ++block)
            {
                auto p = tone ((uint64_t) block, 0.25f, 1000, false); push (play, p); play.pull (left.data(), right.data(), 120);
            }
            expect (std::all_of (right.begin(), right.end(), [] (float v) { return v == 0; }));
            for (int block = 0; block < 200; ++block)
            {
                auto p = tone ((uint64_t) block, -0.25f); p.header.sender = 2; p.arrival += 500000000ULL;
                push (play, p); play.pull (left.data(), right.data(), 120);
            }
            expectWithinAbsoluteError (left.back(), -0.25f, 1.0e-5f);
            expectWithinAbsoluteError (right.back(), -0.25f, 1.0e-5f);
        }
    }
};

class LiveMixCollabClockTests : public juce::UnitTest
{
public:
    LiveMixCollabClockTests() : UnitTest ("LiveMix Collab clock", "LiveMix") {}
    void runTest() override
    {
        for (const auto rates : { std::pair<int, int> { 48000, 48000 }, { 44100, 48000 }, { 44100, 44100 } })
        for (double skew : { 50.0, -150.0, 500.0, -1000.0 })
        {
            const int roomRate = rates.first, deviceRate = rates.second;
            beginTest ("one simulated hour: room " + juce::String (roomRate) + ", device " + juce::String (deviceRate)
                       + ", sender " + juce::String (skew) + " ppm");
            CollabPlayout play; play.prepare (deviceRate, 120, roomRate);
            std::array<float, 120> left {}, right {};
            auto p = tone (0);
            const double packetSeconds = 120.0 / (roomRate * (1.0 + skew * 1.0e-6));
            uint64_t nextPacket = 0;
            uint64_t lastUnderruns = 0;
            double minError = 1.0e9, maxError = -1.0e9;
            for (int block = 0; block < 3600 * deviceRate / 120; ++block)
            {
                const double time = block * 120.0 / deviceRate;
                while (nextPacket * packetSeconds <= time + 1.0e-10)
                {
                    p.header.firstFrame = nextPacket * 120; p.arrival = (uint64_t) (nextPacket * packetSeconds * 1.0e9);
                    push (play, p); ++nextPacket;
                }
                play.pull (left.data(), right.data(), 120);
                if (play.getStats().underruns != lastUnderruns)
                {
                    const auto stats = play.getStats(); lastUnderruns = stats.underruns;
                    logMessage ("underrun at " + juce::String (time, 3) + " s, target=" + juce::String (stats.targetMs, 3)
                        + ", correction=" + juce::String (stats.correctionPpm, 3));
                }
                if (time > 60.0)
                {
                    const auto stats = play.getStats();
                    minError = std::min (minError, stats.fillMs - stats.targetMs); maxError = std::max (maxError, stats.fillMs - stats.targetMs);
                }
            }
            const auto stats = play.getStats();
            logMessage ("ppm=" + juce::String (skew) + ", correction=" + juce::String (stats.correctionPpm, 3)
                + ", fill-target=" + juce::String (minError, 3) + ".." + juce::String (maxError, 3)
                + " ms, underruns=" + juce::String (stats.underruns));
            expect (stats.underruns == 0); expect (stats.lost == 0);
            // Keep the original 96-wire-frame allowance (2 ms at 48 kHz) as packet duration changes.
            const double allowedMs = 96000.0 / roomRate;
            expectGreaterOrEqual (minError, -allowedMs); expectLessOrEqual (maxError, allowedMs);
            expectWithinAbsoluteError (stats.correctionPpm, skew, 80.0);
        }
    }
};
static LiveMixCollabPlayoutTests liveMixCollabPlayoutTests;
static LiveMixCollabClockTests liveMixCollabClockTests;
}
