#include "LiveMixCollabTestHelpers.h"
#include "reference/CollabPlayout0176.h"
#include <chrono>
#include <cstring>

namespace gocue::tests
{
using namespace collabtest;
namespace
{
enum class Signal { sine, voice, noise, silence };
const char* signalName (Signal s)
{
    switch (s) { case Signal::sine: return "sine"; case Signal::voice: return "voice";
                 case Signal::noise: return "noise"; default: return "silence"; }
}
float signal (Signal kind, uint64_t frame, double rate, int ch)
{
    const double t = frame / rate;
    const double phase = juce::MathConstants<double>::twoPi * (kind == Signal::sine ? 137.0 : 173.3) * t + 0.37;
    const double scale = ch == 0 ? 1.0 : 0.63;
    if (kind == Signal::sine) return (float) (scale * 0.4 * std::sin (phase));
    if (kind == Signal::voice)
        return (float) (scale * (0.8 + 0.2 * std::cos (juce::MathConstants<double>::twoPi * 3.1 * t))
            * (0.27 * std::sin (phase) + 0.12 * std::sin (2 * phase + 0.2)
                + 0.08 * std::sin (3 * phase + 0.5) + 0.04 * std::sin (5 * phase + 0.9)));
    if (kind == Signal::silence) return 0;
    uint32_t bits = (uint32_t) frame + 0x9e3779b9u;
    bits ^= bits >> 16; bits *= 0x7feb352du; bits ^= bits >> 15; bits *= 0x846ca68bu; bits ^= bits >> 16;
    return (float) (scale * 0.35 * ((bits & 0xffffff) / 8388608.0 - 1.0));
}
Packet packet (Signal kind, uint64_t number, uint32_t rate = 48000)
{
    auto p = tone (number, 0, 0, true, rate);
    std::array<float, 120> left, right;
    for (int i = 0; i < 120; ++i)
    {
        left[(size_t) i] = signal (kind, number * 120 + i, rate, 0);
        right[(size_t) i] = signal (kind, number * 120 + i, rate, 1);
    }
    packPcm24 (p.payload.data(), left.data(), right.data(), 120);
    return p;
}
template <typename Play>
bool send (Play& play, const Packet& p) { return play.push (p.header, p.payload.data(), p.arrival); }
bool sameStats (const CollabPlayout::Stats& a, const CollabPlayout0176::Stats& b)
{
    return (int) a.state == (int) b.state && a.targetMs == b.targetMs && a.fillMs == b.fillMs
        && a.correctionPpm == b.correctionPpm && a.lossPercent10s == b.lossPercent10s
        && a.received == b.received && a.lost == b.lost && a.late == b.late && a.underruns == b.underruns;
}
double snr (const std::vector<float>& output, const std::vector<float>& original, int count)
{
    double energy = 0, error = 0;
    for (int i = 0; i < count; ++i)
    {
        energy += original[(size_t) i] * original[(size_t) i];
        const double delta = output[(size_t) i] - original[(size_t) i]; error += delta * delta;
    }
    return 10 * std::log10 (std::max (1.0e-20, energy) / std::max (1.0e-20, error));
}
double minWindowRms (const std::vector<float>& audio, int count, int window)
{
    if (count < window) return 0; // no complete window; the report prints n/a
    double minimum = 1.0, energy = 0;
    for (int i = 0; i < count; ++i)
    {
        energy += audio[(size_t) i] * audio[(size_t) i];
        if (i >= window) energy -= audio[(size_t) (i - window)] * audio[(size_t) (i - window)];
        if (i + 1 >= window) minimum = std::min (minimum, std::sqrt (std::max (0.0, energy) / window));
    }
    return minimum;
}
struct GapResult
{
    std::vector<float> oldAudio, newAudio, original;
    bool statsEqual = true, stereo = true;
    uint64_t lost = 0, underruns = 0;
    double startStep = 0, recoveryStep = 0, oldStartStep = 0, oldRecoveryStep = 0;
};
GapResult gap (Signal kind, bool web, bool refill, int length)
{
    CollabPlayout now (web ? CollabPlayout::Profile::web : CollabPlayout::Profile::native);
    CollabPlayout0176 old (web ? CollabPlayout0176::Profile::web : CollabPlayout0176::Profile::native);
    CollabPlayout ideal (web ? CollabPlayout::Profile::web : CollabPlayout::Profile::native);
    now.prepare (48000, 120); old.prepare (48000, 120); ideal.prepare (48000, 120);
    std::array<float, 120> nl {}, nr {}, ol {}, oright {}, il {}, ir {};
    GapResult result;
    for (int b = 0; b < 180; ++b)
    {
        const auto p = packet (kind, (uint64_t) b);
        send (now, p); send (old, p); send (ideal, p);
        now.pull (nl.data(), nr.data(), 120); old.pull (ol.data(), oright.data(), 120); ideal.pull (il.data(), ir.data(), 120);
        result.statsEqual = result.statsEqual && sameStats (now.getStats(), old.getStats());
    }
    // Already playing: queue a future run containing an interior hole. Refill
    // instead stops delivery entirely, then resumes in a burst after emptying.
    if (! refill)
        for (int p = 180; p < 320; ++p)
        {
            const auto data = packet (kind, (uint64_t) p);
            send (ideal, data);
            if (p < 200 || p >= 200 + length / 120) { send (now, data); send (old, data); }
        }
    bool started = false;
    float previous = nl.back(), oldPrevious = ol.back();
    int idealNext = 180, resumeNext = 0;
    // Single-frame pulls locate the erasure exactly, including partial packets
    // and a refill in the middle of a normal 120-frame callback.
    for (int i = 0; i < 18000; ++i)
    {
        if (refill && i % 120 == 0) send (ideal, packet (kind, (uint64_t) idealNext++));
        const int at = (int) result.newAudio.size();
        if (refill && started && at == length)
        {
            resumeNext = 180 + i / 120;
            const int primingPackets = (int) std::ceil (now.getStats().targetMs / 2.5) + 2;
            for (int p = 0; p < primingPackets; ++p)
            {
                const auto data = packet (kind, (uint64_t) resumeNext++); send (now, data); send (old, data);
            }
        }
        else if (refill && started && at > length && (at - length) % 120 == 0)
        {
            const auto data = packet (kind, (uint64_t) resumeNext++); send (now, data); send (old, data);
        }
        now.pull (nl.data(), nr.data(), 1); old.pull (ol.data(), oright.data(), 1); ideal.pull (il.data(), ir.data(), 1);
        const auto stats = now.getStats();
        result.statsEqual = result.statsEqual && sameStats (stats, old.getStats());
        if (! started && stats.lost > 0)
        {
            started = true; result.startStep = std::abs (nl[0] - previous);
            result.oldStartStep = std::abs (ol[0] - oldPrevious);
        }
        if (started)
        {
            result.newAudio.push_back (nl[0]); result.oldAudio.push_back (ol[0]); result.original.push_back (il[0]);
            result.stereo = result.stereo && std::abs (nr[0] - 0.63f * nl[0]) < 2.0e-5f;
            if (at >= length && at < length + 900)
            {
                result.recoveryStep = std::max (result.recoveryStep, (double) std::abs (nl[0] - previous));
                result.oldRecoveryStep = std::max (result.oldRecoveryStep, (double) std::abs (ol[0] - oldPrevious));
            }
            if ((int) result.newAudio.size() >= length + 1200)
            { result.lost = stats.lost; result.underruns = stats.underruns; break; }
        }
        previous = nl[0]; oldPrevious = ol[0];
    }
    return result;
}
}

class LiveMixCollabPlcTests : public juce::UnitTest
{
public:
    LiveMixCollabPlcTests() : UnitTest ("LiveMix Collab PLC", "LiveMix") {}
    void runTest() override
    {
        beginTest ("lossless audio and every statistic are bit identical to dae954ba, including startup and measured target refill");
        uint64_t compared = 0;
        for (bool web : { false, true })
        for (uint32_t wire : { 44100u, 48000u })
        for (int device : { 44100, 48000, 96000 })
        for (int block : { 120, 256, 1024 })
        {
            CollabPlayout now (web ? CollabPlayout::Profile::web : CollabPlayout::Profile::native);
            CollabPlayout0176 old (web ? CollabPlayout0176::Profile::web : CollabPlayout0176::Profile::native);
            now.prepare (device, block, wire); old.prepare (device, block, wire);
            std::vector<float> nl ((size_t) block), nr (nl), ol (nl), other (nl);
            uint64_t next = 0; int rendered = 0; bool equal = true;
            while (rendered < device * 2)
            {
                while (next * 120.0 / (wire * 1.00015) <= rendered / (double) device)
                {
                    auto p = packet (Signal::voice, next++, wire);
                    p.arrival = (uint64_t) (p.header.firstFrame * 1.0e9 / (wire * 1.00015));
                    equal = (send (now, p) == send (old, p)) && equal;
                }
                const int n = std::min (block, device * 2 - rendered);
                now.pull (nl.data(), nr.data(), n); old.pull (ol.data(), other.data(), n);
                equal = equal && std::memcmp (nl.data(), ol.data(), n * sizeof (float)) == 0
                    && std::memcmp (nr.data(), other.data(), n * sizeof (float)) == 0
                    && sameStats (now.getStats(), old.getStats());
                rendered += n; compared += (uint64_t) n * 2;
            }
            expect (equal, "profile=" + juce::String ((int) web) + " wire=" + juce::String ((int) wire)
                + " device=" + juce::String (device) + " block=" + juce::String (block));
        }
        logMessage ("PLC lossless sample comparisons=" + juce::String (compared));

        for (bool web : { false, true })
        for (Signal kind : { Signal::sine, Signal::voice, Signal::noise })
        for (int test = 0; test < 7; ++test)
        {
            const bool refill = test >= 3;
            const int length = refill ? (480 << (test - 3)) : (120 << test);
            const juce::String label = juce::String (web ? "web," : "native,") + signalName (kind)
                + (refill ? ",refill," : ",hole,") + juce::String (length / 48.0, 1) + "ms";
            beginTest (label);
            const auto r = gap (kind, web, refill, length);
            expectEquals ((int) r.newAudio.size(), length + 1200);
            if ((int) r.newAudio.size() < length + 1200) continue;
            const double before = snr (r.oldAudio, r.original, length), after = snr (r.newAudio, r.original, length);
            const double rms = minWindowRms (r.newAudio, std::min (length, 2880), 480);
            const double oldRms = minWindowRms (r.oldAudio, std::min (length, 2880), 480);
            logMessage ("PLC " + label + ",snr_old=" + juce::String (before, 3) + ",snr_new=" + juce::String (after, 3)
                + ",min_10ms_rms_old=" + (length < 480 ? juce::String ("n/a") : juce::String (oldRms, 6))
                + ",min_10ms_rms_new=" + (length < 480 ? juce::String ("n/a") : juce::String (rms, 6))
                + ",start_step_old=" + juce::String (r.oldStartStep, 6) + ",start_step_new=" + juce::String (r.startStep, 6)
                + ",recovery_step_old=" + juce::String (r.oldRecoveryStep, 6) + ",recovery_step_new=" + juce::String (r.recoveryStep, 6));
            expect (r.statsEqual, "state, target, fill, ppm and all loss counters must match 0.17.6 on every pull");
            expect (r.stereo, "one shared pitch preserves the stereo level relationship");
            expectEquals ((int) r.lost, refill ? 1 : length / 120);
            expectEquals ((int) r.underruns, refill ? 1 : 0);
            if (kind != Signal::noise)
            {
                expectGreaterThan (after - before, length <= 480 ? 5.0 : 1.0);
                if (length >= 480) expectGreaterThan (rms, 0.001);
                expectLessThan (r.startStep, kind == Signal::sine ? 0.02 : 0.06);
                expectLessThan (r.recoveryStep, kind == Signal::sine ? 0.012 : 0.04);
            }
            else
            {
                // Noise has no predictable period: it must take the short fade,
                // not turn into a repeated pitched fragment.
                for (int i = 121; i < length; ++i) expectEquals (r.newAudio[(size_t) i], 0.0f);
            }
            if (length > 2880)
                for (int i = 2880; i < length; ++i) expectEquals (r.newAudio[(size_t) i], 0.0f);
        }

        beginTest ("device-rate synthesis spans callback splits; low confidence and silence do not invent a tone");
        for (int rate : { 44100, 48000, 96000, 192000 })
        for (Signal kind : { Signal::sine, Signal::voice, Signal::noise, Signal::silence })
        {
            CollabPlc whole, split; whole.prepare (rate); split.prepare (rate);
            std::vector<float> history ((size_t) rate / 10 * 2), a ((size_t) rate / 10 * 2), b (a);
            for (size_t i = 0; i < history.size() / 2; ++i)
                for (int ch = 0; ch < 2; ++ch) history[i * 2 + ch] = signal (kind, i, rate, ch);
            whole.receive (history.data(), (int) history.size() / 2); split.receive (history.data(), (int) history.size() / 2);
            whole.conceal (a.data(), (int) a.size() / 2);
            for (int i = 0; i < (int) b.size() / 2; i += 73)
                split.conceal (b.data() + i * 2, std::min (73, (int) b.size() / 2 - i));
            expect (a == b);
            const int silentAt = (int) std::lround (rate * (kind == Signal::noise || kind == Signal::silence ? 0.0025 : 0.060));
            expect (std::all_of (a.begin() + silentAt * 2, a.end(), [] (float x) { return x == 0; }));
            if (kind == Signal::sine || kind == Signal::voice)
            {
                std::vector<float> left;
                for (int i = 0; i < (int) std::lround (rate * 0.060); ++i) left.push_back (a[(size_t) i * 2]);
                expectGreaterThan (minWindowRms (left, (int) left.size(), rate / 100), 0.001);
            }
            whole.reset(); std::fill (a.begin(), a.end(), 1.0f); whole.conceal (a.data(), (int) a.size() / 2);
            expect (std::all_of (a.begin(), a.end(), [] (float x) { return x == 0; }));
        }

        beginTest ("CPU: paired 120-frame pulls, steady and one missing packet per 100");
        for (bool loss : { false, true })
        {
            double bestOld = 1.0e9, bestNew = 1.0e9;
            for (int repeat = 0; repeat < 3; ++repeat)
            {
                bestOld = std::min (bestOld, benchmark<CollabPlayout0176> (loss));
                bestNew = std::min (bestNew, benchmark<CollabPlayout> (loss));
            }
            logMessage ("PLC CPU loss=" + juce::String ((int) loss) + ",old_us_per_120=" + juce::String (bestOld, 3)
                + ",new_us_per_120=" + juce::String (bestNew, 3) + ",delta_percent=" + juce::String ((bestNew / bestOld - 1) * 100, 2));
        }
        CollabPlc plc; plc.prepare (48000);
        std::vector<float> history (4800);
        for (size_t i = 0; i < history.size() / 2; ++i)
            history[i * 2] = history[i * 2 + 1] = signal (Signal::voice, i, 48000, 0);
        std::array<float, 240> audio {};
        std::vector<double> onset;
        for (int run = 0; run < 300; ++run)
        {
            plc.reset(); plc.receive (history.data(), 2400);
            const auto start = std::chrono::steady_clock::now(); plc.conceal (audio.data(), 120);
            onset.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count());
        }
        std::sort (onset.begin(), onset.end());
        logMessage ("PLC CPU onset_120_us p50=" + juce::String (onset[150], 3) + ",p99=" + juce::String (onset[297], 3));
    }
private:
    template <typename Play> double benchmark (bool loss)
    {
        Play play; play.prepare (48000, 120);
        std::array<float, 120> left {}, right {};
        auto p = packet (Signal::voice, 0);
        double microseconds = 0;
        constexpr int blocks = 6000;
        for (int i = 0; i < blocks; ++i)
        {
            p.header.firstFrame = (uint64_t) i * 120; p.arrival = (uint64_t) i * 2500000;
            if (! loss || i < 1000 || i % 100 != 50) send (play, p);
            const auto start = std::chrono::steady_clock::now(); play.pull (left.data(), right.data(), 120);
            microseconds += std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count();
        }
        return microseconds / blocks;
    }
} liveMixCollabPlcTests;
}
