#if defined (LM_PLC_RECOVERY_STANDALONE)
#include "reference/plcBeforeFix1/CollabPlc.h"
#else
#include "CollabPlc.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#if ! defined (LM_PLC_RECOVERY_STANDALONE)
#include <juce_core/juce_core.h>
#endif

namespace
{
using gocue::livemix::collab::CollabPlc;
int msFrames (int rate, double ms) { return (int) std::lround (rate * ms / 1000.0); }
std::vector<float> voice (int frames, int rate, double hz, int offset = 0)
{
    std::vector<float> audio ((size_t) frames * 2);
    for (int i = 0; i < frames; ++i)
    {
        const double phase = 6.283185307179586 * hz * (offset + i) / rate + 0.37;
        const float x = (float) (0.27 * std::sin (phase) + 0.12 * std::sin (2 * phase + 0.2)
            + 0.08 * std::sin (3 * phase + 0.5) + 0.04 * std::sin (5 * phase + 0.9));
        audio[(size_t) i * 2] = x; audio[(size_t) i * 2 + 1] = 0.63f * x;
    }
    return audio;
}
struct Result
{
    std::string label;
    double rms60 = 0, rms900 = 0, peak60 = 0, frozenError = 0, maxStep = 0;
};
Result interruptedRecovery (int rate, double missingMs, double goodMs)
{
    CollabPlc plc, continuous; plc.prepare (rate); continuous.prepare (rate);
    auto warm = voice (msFrames (rate, 100), rate, 137);
    plc.receive (warm.data(), (int) warm.size() / 2);
    continuous.receive (warm.data(), (int) warm.size() / 2);
    std::vector<float> audio ((size_t) rate * 2), reference (audio);
    continuous.conceal (reference.data(), rate);
    const int missing = msFrames (rate, missingMs), good = msFrames (rate, goodMs);
    Result result; result.label = std::to_string (rate) + "Hz loss=" + std::to_string (missingMs) + "ms good=" + std::to_string (goodMs) + "ms";
    for (int at = 0; at < rate;)
    {
        const int lost = std::min (missing, rate - at);
        plc.conceal (audio.data() + (size_t) at * 2, lost);
        // After the <=3.75 ms boundary join, compare against the same frozen
        // voice with uninterrupted concealment. This checks phase AND the
        // attenuation clock, including time spent in real received fragments.
        for (int i = msFrames (rate, 4); i < lost; ++i)
            result.frozenError = std::max (result.frozenError,
                (double) std::abs (audio[(size_t) (at + i) * 2] - reference[(size_t) (at + i) * 2]));
        at += lost;
        const int received = std::min (good, rate - at);
        // The speaker stopped/muted. These samples are real silence, even if
        // receive() subsequently overlays the old voice on this output buffer.
        std::fill_n (audio.data() + (size_t) at * 2, (size_t) received * 2, 0.0f);
        plc.receive (audio.data() + (size_t) at * 2, received);
        at += received;
    }
    const int deadline = msFrames (rate, 60), late = msFrames (rate, 900);
    float previous = warm[warm.size() - 2];
    for (int i = 0; i < rate; ++i)
    {
        const float x = audio[(size_t) i * 2];
        result.maxStep = std::max (result.maxStep, (double) std::abs (x - previous)); previous = x;
        if (i >= deadline) { result.rms60 += x * x; result.peak60 = std::max (result.peak60, (double) std::abs (x)); }
        if (i >= late) result.rms900 += x * x;
    }
    result.rms60 = std::sqrt (result.rms60 / (rate - deadline));
    result.rms900 = std::sqrt (result.rms900 / (rate - late));
    return result;
}
struct FreshResult { bool rawPassThrough, newHistory; double rms; };
FreshResult genuineRecovery (bool silence)
{
    constexpr int rate = 48000, realFrames = 1440, lossFrames = 3840;
    CollabPlc plc, fresh; plc.prepare (rate); fresh.prepare (rate);
    auto warm = voice (4800, rate, 137); plc.receive (warm.data(), 4800);
    std::vector<float> gap (480); plc.conceal (gap.data(), 240);
    auto raw = silence ? std::vector<float> ((size_t) realFrames * 2) : voice (realFrames, rate, 70, 5040);
    auto heard = raw;
    plc.receive (heard.data(), realFrames); fresh.receive (raw.data(), realFrames);
    // At most a 15 ms recovery overlap; the rest is delivered immediately.
    const bool through = std::equal (heard.begin() + 720 * 2, heard.end(), raw.begin() + 720 * 2);
    std::vector<float> a ((size_t) lossFrames * 2), b (a);
    plc.conceal (a.data(), lossFrames); fresh.conceal (b.data(), lossFrames);
    double energy = 0;
    for (int i = 0; i < 480; ++i) energy += a[(size_t) i * 2] * a[(size_t) i * 2];
    return { through, a == b, std::sqrt (energy / 480) };
}
template <typename Check, typename Log>
void runRecoveryTests (Check check, Log log)
{
    for (int rate : { 48000, 44100 })
    for (const auto pattern : { std::pair<double, double> { 5, 2.5 }, { 2.5, 2.5 }, { 5, 5 }, { 2.5, 10 } })
    {
        const auto r = interruptedRecovery (rate, pattern.first, pattern.second);
        char values[320];
        std::snprintf (values, sizeof (values), ",rms_60_1000=%.9f,rms_900_1000=%.9f,peak_after60=%.9f,frozen_error=%.9f,max_step=%.9f",
            r.rms60, r.rms900, r.peak60, r.frozenError, r.maxStep);
        log ("PLC FIX1 " + r.label + values);
        check (r.peak60 == 0, r.label + ": all output must be exactly zero after the original 60 ms deadline");
        check (r.rms900 == 0, r.label + ": no residual voice at 900-1000 ms");
        check (r.frozenError < 1.0e-6, r.label + ": relapse must retain original frozen voice, phase and attenuation");
        check (r.maxStep < 0.04, r.label + ": relapse joins remain smooth");
    }
    for (bool silence : { false, true })
    {
        const auto r = genuineRecovery (silence);
        log (std::string ("PLC FIX1 fresh30ms=") + (silence ? "silence" : "new70HzVoice")
            + ",raw_passthrough=" + std::to_string (r.rawPassThrough) + ",raw_history_equal=" + std::to_string (r.newHistory)
            + ",first10ms_rms=" + std::to_string (r.rms));
        check (r.rawPassThrough, "good audio must pass through before the episode is rearmed");
        check (r.newHistory, "a new episode uses only the contiguous real input, including pre-overlap samples");
        check (silence ? r.rms == 0 : r.rms > 0.05, "fresh voice can start a new full-level concealment; fresh silence stays silent");
    }
}
}

#if defined (LM_PLC_RECOVERY_STANDALONE)
int main()
{
    int passed = 0, failed = 0;
    runRecoveryTests ([&] (bool ok, const std::string& message)
        { if (ok) ++passed; else { ++failed; std::printf ("FAIL %s\n", message.c_str()); } },
        [] (const std::string& message) { std::puts (message.c_str()); });
    std::printf ("PLC FIX1 checks: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
#else
namespace gocue::tests
{
class LiveMixCollabPlcRecoveryTests : public juce::UnitTest
{
public:
    LiveMixCollabPlcRecoveryTests() : UnitTest ("LiveMix Collab PLC recovery", "LiveMix") {}
    void runTest() override
    {
        beginTest ("speech stops during repeated short losses; interrupted recovery retains the old erasure episode");
        runRecoveryTests ([&] (bool ok, const std::string& message) { expect (ok, juce::String (message)); },
            [&] (const std::string& message) { logMessage (juce::String (message)); });
    }
} liveMixCollabPlcRecoveryTests;
}
#endif
