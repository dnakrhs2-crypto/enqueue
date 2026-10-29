#if defined (LM_PLC_BURST_BEFORE)
#include "reference/plcBeforeFix2/CollabPlc.h"
#else
#include "CollabPlc.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#if ! defined (LM_PLC_BURST_STANDALONE)
#include <juce_core/juce_core.h>
#endif

namespace
{
using gocue::livemix::collab::CollabPlc;
int framesAt (int rate, double ms) { return (int) std::lround (rate * ms / 1000.0); }
std::vector<float> signal (int frames, int rate, bool harmonic, int offset = 0, double hz = 137)
{
    std::vector<float> audio ((size_t) frames * 2);
    for (int i = 0; i < frames; ++i)
    {
        const double phase = 6.283185307179586 * hz * (offset + i) / rate + 0.37;
        const float x = harmonic
            ? (float) (0.27 * std::sin (phase) + 0.12 * std::sin (2 * phase + 0.2)
                + 0.08 * std::sin (3 * phase + 0.5) + 0.04 * std::sin (5 * phase + 0.9))
            : (float) (0.4 * std::sin (phase));
        audio[(size_t) i * 2] = x; audio[(size_t) i * 2 + 1] = 0.63f * x;
    }
    return audio;
}
double maxStep (const float* audio, int frames, float previous)
{
    double result = 0;
    for (int i = 0; i < frames; ++i)
    {
        const float x = audio[(size_t) i * 2];
        result = std::max (result, (double) std::abs (x - previous)); previous = x;
    }
    return result;
}
struct Result
{
    int losses = 0, zeroLosses = 0;
    double minRatio = 1.0e9, step = 0, isolatedStep = 0, peak60 = 0, rms900 = 0;
    bool stereo = true;
};
Result burst (int rate, bool harmonic, bool stopped, double lossMs, double goodMs)
{
    const int warmFrames = framesAt (rate, 100), missing = framesAt (rate, lossMs), good = framesAt (rate, goodMs);
    auto warm = signal (warmFrames, rate, harmonic);
    auto original = signal (rate, rate, harmonic, warmFrames), output = original;
    if (stopped) std::fill (output.begin(), output.end(), 0.0f);
    CollabPlc plc; plc.prepare (rate); plc.receive (warm.data(), warmFrames);
    Result result;
    for (int at = 0; at < rate;)
    {
        const int lost = std::min (missing, rate - at);
        plc.conceal (output.data() + (size_t) at * 2, lost);
        double energy = 0, ideal = 0;
        for (int i = 0; i < lost; ++i)
        {
            const float x = output[(size_t) (at + i) * 2], y = original[(size_t) (at + i) * 2];
            energy += x * x; ideal += y * y;
        }
        ++result.losses; if (energy == 0) ++result.zeroLosses;
        result.minRatio = std::min (result.minRatio, std::sqrt (energy / ideal));
        const int received = std::min (good, rate - at - lost);
        plc.receive (output.data() + (size_t) (at + lost) * 2, received);

        // Same phase and duration with a full genuine history, but only one
        // loss: a reference for ordinary waveform slopes and recovery joins.
        if (! stopped || at == 0)
        {
            CollabPlc isolated; isolated.prepare (rate);
            auto preceding = signal (warmFrames, rate, harmonic, at);
            isolated.receive (preceding.data(), warmFrames);
            auto one = signal (lost + received, rate, harmonic, warmFrames + at);
            if (stopped) std::fill (one.begin(), one.end(), 0.0f);
            isolated.conceal (one.data(), lost);
            isolated.receive (one.data() + (size_t) lost * 2, received);
            result.isolatedStep = std::max (result.isolatedStep,
                maxStep (one.data(), lost + received, preceding[preceding.size() - 2]));
        }
        at += lost + received;
    }
    result.step = maxStep (output.data(), rate, warm[warm.size() - 2]);
    const int deadline = framesAt (rate, 60), late = framesAt (rate, 900);
    for (int i = 0; i < rate; ++i)
    {
        const float x = output[(size_t) i * 2];
        result.stereo = result.stereo && std::isfinite (x)
            && std::abs (output[(size_t) i * 2 + 1] - 0.63f * x) < 1.0e-6;
        if (i >= deadline) result.peak60 = std::max (result.peak60, (double) std::abs (x));
        if (i >= late) result.rms900 += x * x;
    }
    result.rms900 = std::sqrt (result.rms900 / (rate - late));
    return result;
}

template <typename Check, typename Log>
void runBurstTests (Check check, Log log)
{
    for (int rate : { 44100, 48000, 96000, 192000 })
    for (bool harmonic : { false, true })
    for (bool stopped : { false, true })
    for (auto pattern : { std::pair<double, double> { 2.5, 27.5 }, { 2.5, 5 }, { 2.5, 10 },
                         { 5, 20 }, { 2.5, 29.9 }, { 2.5, 30.1 }, { 10, 20 } })
    {
        const auto r = burst (rate, harmonic, stopped, pattern.first, pattern.second);
        const auto label = std::to_string (rate) + "Hz," + (harmonic ? "voice," : "sine,")
            + (stopped ? "stopped," : "continuing,") + "loss=" + std::to_string (pattern.first)
            + "ms,good=" + std::to_string (pattern.second) + "ms";
        char values[320];
        std::snprintf (values, sizeof (values), ",losses=%d,zero_losses=%d,min_rms_ratio=%.9f,max_step=%.9f,isolated_step=%.9f,peak_after60=%.9f,rms_900_1000=%.9f",
            r.losses, r.zeroLosses, r.minRatio, r.step, r.isolatedStep, r.peak60, r.rms900);
        log ("PLC FIX2 " + label + values);
        if (stopped)
        {
            check (r.peak60 == 0, label + ": old voice expires within 60 ms");
            check (r.rms900 == 0, label + ": no late residual voice");
        }
        else
        {
            check (r.zeroLosses == 0, label + ": no loss may start with a forced silent block");
            // A 5 ms good fragment interrupts the 137 Hz recovery; it cannot
            // promise a newly estimated pitch. Longer fragments finish recovery.
            check (pattern.second < 10 || r.minRatio > 0.5, label + ": recovered voice remains concealed at normal level");
        }
        check (r.step <= r.isolatedStep * 1.35 + 0.001, label + ": joins stay at isolated-loss slope scale");
        check (r.stereo, label + ": finite output with one shared stereo phase");
    }

    // After the old 400 Hz voice recovers in 2.5 ms, only this new, contiguous
    // 5/10 ms input may seed the next loss. 5 ms cannot support the new 137 Hz
    // period, so it must fade its own final sample; 10 ms can estimate it.
    for (int rate : { 44100, 48000, 96000, 192000 })
    for (double goodMs : { 5.0, 10.0 })
    {
        CollabPlc plc, fresh; plc.prepare (rate); fresh.prepare (rate);
        auto warm = signal (framesAt (rate, 100), rate, false, 0, 400);
        plc.receive (warm.data(), (int) warm.size() / 2);
        std::vector<float> gap ((size_t) framesAt (rate, 2.5) * 2);
        plc.conceal (gap.data(), (int) gap.size() / 2);
        auto raw = signal (framesAt (rate, goodMs), rate, true, framesAt (rate, 102.5)), heard = raw;
        plc.receive (heard.data(), (int) heard.size() / 2); fresh.receive (raw.data(), (int) raw.size() / 2);
        std::vector<float> a ((size_t) framesAt (rate, 10) * 2), b (a);
        plc.conceal (a.data(), (int) a.size() / 2); fresh.conceal (b.data(), (int) b.size() / 2);
        const auto label = std::to_string (rate) + "Hz short_history=" + std::to_string (goodMs) + "ms";
        const bool silentTail = std::all_of (a.begin() + framesAt (rate, 2.5) * 2, a.end(), [] (float x) { return x == 0; });
        log ("PLC FIX2 " + label + ",raw_history_equal=" + std::to_string (a == b) + ",fallback=" + std::to_string (silentTail));
        check (a == b, label + ": only current contiguous input is the source");
        check (silentTail == (goodMs == 5), label + ": short range uses a real-tail fade or a supported pitch");
    }
}
}

#if defined (LM_PLC_BURST_STANDALONE)
int main()
{
    int passed = 0, failed = 0;
    runBurstTests ([&] (bool ok, const std::string& message)
        { if (ok) ++passed; else { ++failed; std::printf ("FAIL %s\n", message.c_str()); } },
        [] (const std::string& message) { std::puts (message.c_str()); });
    std::printf ("PLC FIX2 checks: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
#else
namespace gocue::tests
{
class LiveMixCollabPlcBurstTests : public juce::UnitTest
{
public:
    LiveMixCollabPlcBurstTests() : UnitTest ("LiveMix Collab PLC bursts", "LiveMix") {}
    void runTest() override
    {
        beginTest ("recovered voice starts a fresh loss; only interrupted overlap retains the old loss");
        runBurstTests ([&] (bool ok, const std::string& message) { expect (ok, juce::String (message)); },
            [&] (const std::string& message) { logMessage (juce::String (message)); });
    }
} liveMixCollabPlcBurstTests;
}
#endif
