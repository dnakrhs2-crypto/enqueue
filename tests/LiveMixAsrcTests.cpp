#include "lm_asrc.h"
#include "lm_obs_protocol.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

// A second C compilation of the production source redirects its allocator calls here.
// This works in Release too, without replacing the allocator used by other suites.
namespace { thread_local int allocationCalls = 0; }
extern "C"
{
void* lm_test_malloc (size_t n) { ++allocationCalls; return std::malloc (n); }
void* lm_test_calloc (size_t n, size_t size) { ++allocationCalls; return std::calloc (n, size); }
void* lm_test_realloc (void* p, size_t n) { ++allocationCalls; return std::realloc (p, n); }
void lm_test_free (void* p) { ++allocationCalls; std::free (p); }
size_t lm_test_asrc_size (int);
void lm_test_asrc_init (lm_asrc*, int, double, double);
void lm_test_asrc_reset (lm_asrc*);
void lm_test_asrc_set_correction_ppm (lm_asrc*, double);
int lm_test_asrc_process (lm_asrc*, const float*, int, int*, float*, int);
}

namespace gocue::livemix::tests
{
namespace
{
struct AsrcStorage
{
    explicit AsrcStorage (int channels, double inRate, double outRate)
        : bytes ((lm_asrc_size (channels) + sizeof (std::max_align_t) - 1) / sizeof (std::max_align_t))
    {
        lm_asrc_init (get(), channels, inRate, outRate);
    }

    lm_asrc* get() { return reinterpret_cast<lm_asrc*> (bytes.data()); }
    std::vector<std::max_align_t> bytes;
};

struct RingStorage
{
    RingStorage() : pcm (LM_OBS_CAPACITY_FRAMES * LM_OBS_CHANNELS, -99.0f)
    {
        header.magic = LM_OBS_MAGIC;
        header.protocol_major = LM_OBS_PROTOCOL_MAJOR;
        header.header_bytes = sizeof (header);
        header.data_offset = sizeof (header);
        header.mapping_bytes = (uint32_t) (sizeof (header) + pcm.size() * sizeof (float));
        header.channels = LM_OBS_CHANNELS;
        header.capacity_frames = LM_OBS_CAPACITY_FRAMES;
        header.epoch = 1;
        header.sample_rate = 48000;
        header.send_enabled = 1;
    }

    lm_obs_ring_header header {};
    std::vector<float> pcm;
};

struct DriftStats
{
    double minFill = std::numeric_limits<double>::max(), maxFill = 0.0;
    double maxSettledError = 0.0, maxPpm = 0.0, maxSlew = 0.0, maxSlewLate = 0.0, finalPpm = 0.0;
};

DriftStats simulateClocks (double errorPpm, bool reverse = false, bool acquired = false)
{
    constexpr double target = 1440.0, dt = 0.01;
    lm_drift controller;
    lm_drift_init (&controller, target, 1.0 / dt);
    // At the correction clamp there is no spare authority to recover the fill
    // lost during startup. Test both a previously acquired clock and cold start.
    if (acquired) controller.ppm = controller.integ = errorPpm;
    double producer = 0.0, consumer = 0.0, correction = controller.ppm;
    DriftStats stats;

    for (int tick = 0; tick < 720000; ++tick) // two hours; fractional clock counters, no wall clock
    {
        const double mismatch = reverse && tick >= 180000 ? -errorPpm : errorPpm;
        producer += 480.0 * (1.0 + mismatch * 1.0e-6);
        consumer += 480.0 * (1.0 + correction * 1.0e-6);
        const double fill = target + std::floor (producer) - std::floor (consumer);
        const double next = lm_drift_update (&controller, fill, dt);
        stats.minFill = std::min (stats.minFill, fill);
        stats.maxFill = std::max (stats.maxFill, fill + 481.0); // includes the producer's next block
        stats.maxPpm = std::max (stats.maxPpm, std::abs (next));
        stats.maxSlew = std::max (stats.maxSlew, std::abs (next - correction) / dt);

        if (tick >= 1000)   // after the 10 s acquisition the slow slew applies
            stats.maxSlewLate = std::max (stats.maxSlewLate, std::abs (next - correction) / dt);

        if (tick >= 6000 && (! reverse || tick < 180000 || tick >= 186000))
            stats.maxSettledError = std::max (stats.maxSettledError, std::abs (fill - target));

        correction = next;
    }

    stats.finalPpm = correction;
    return stats;
}
}

class LiveMixAsrcTests : public juce::UnitTest
{
public:
    LiveMixAsrcTests() : juce::UnitTest ("LiveMix shared ASRC and protocol", "LiveMix") {}

    void runTest() override
    {
        beginTest ("protocol layout is stable and every shared counter is aligned");
        {
            expectEquals ((int) sizeof (lm_obs_ring_header), 256);
            expectEquals ((int) sizeof (lm_obs_reader_slot), 32);
            expectEquals ((int) sizeof (lm_obs_readers), 16 + 32 * LM_OBS_MAX_READERS);
            expectEquals ((int) alignof (lm_obs_ring_header), 8);
            for (size_t offset : { offsetof (lm_obs_ring_header, epoch), offsetof (lm_obs_ring_header, sample_rate),
                                   offsetof (lm_obs_ring_header, write_frames), offsetof (lm_obs_ring_header, heartbeat_qpc),
                                   offsetof (lm_obs_ring_header, qpc_frequency), offsetof (lm_obs_ring_header, send_enabled),
                                   offsetof (lm_obs_ring_header, writer_pid), offsetof (lm_obs_ring_header, silent_frames),
                                   offsetof (lm_obs_readers, slot) })
                expectEquals ((int) (offset % 8), 0);
        }

        beginTest ("three ramp blocks survive seven uneven reads, including a ring wrap and 64-bit positions");
        for (int64_t start : { int64_t (0), (int64_t (1) << 33) + LM_OBS_CAPACITY_FRAMES - 233 })
        {
            RingStorage ring;
            ring.header.write_frames = start;
            std::array<float, 480> left {}, right {};
            for (int block = 0; block < 3; ++block)
            {
                for (int i = 0; i < 480; ++i)
                {
                    left[(size_t) i] = (float) (block * 480 + i);
                    right[(size_t) i] = -left[(size_t) i] - 0.5f;
                }
                lm_obs_write (&ring.header, ring.pcm.data(), left.data(), right.data(), 480);
            }
            expectEquals ((juce::int64) ring.header.write_frames, (juce::int64) (start + 1440));
            int64_t read = start, secondReader = start;
            std::array<float, 2048> output {};
            int expected = 0;
            for (uint32_t count : { 1u, 127u, 352u, 13u, 501u, 199u, 247u })
            {
                expectEquals ((int) lm_obs_read (&ring.header, ring.pcm.data(), 1, &read, output.data(), count), (int) count);
                bool identical = true;
                for (uint32_t i = 0; i < count; ++i, ++expected)
                    identical = identical && output[2 * i] == (float) expected && output[2 * i + 1] == -(float) expected - 0.5f;
                expect (identical);
            }
            expectEquals ((int) lm_obs_read (&ring.header, ring.pcm.data(), 1, &read, output.data(), 1), 0);
            expectEquals ((int) lm_obs_read (&ring.header, ring.pcm.data(), 1, &secondReader, output.data(), 127), 127);
            expectEquals (output[0], 0.0f);
            expectEquals ((juce::int64) read, (juce::int64) (start + 1440));
        }

        beginTest ("silence advances the ring, an overrun resyncs, and an epoch change rejects the read");
        {
            RingStorage ring;
            std::array<float, 960> output;
            output.fill (99.0f);
            int64_t read = 0;
            lm_obs_write_silence (&ring.header, ring.pcm.data(), 480);
            expectEquals ((juce::int64) ring.header.write_frames, (juce::int64) 480);
            expectEquals ((int) lm_obs_read (&ring.header, ring.pcm.data(), 1, &read, output.data(), 480), 480);
            expect (std::all_of (output.begin(), output.end(), [] (float x) { return x == 0.0f; }));
            lm_obs_write_silence (&ring.header, ring.pcm.data(), LM_OBS_CAPACITY_FRAMES);
            expectEquals ((int) lm_obs_read (&ring.header, ring.pcm.data(), 1, &read, output.data(), 480), -1);
            expectEquals ((juce::int64) read, (juce::int64) 480); // errors leave resync policy to the reader
            read = ring.header.write_frames - 1440;
            expectEquals ((int) lm_obs_read (&ring.header, ring.pcm.data(), 1, &read, output.data(), 480), 480);
            ++ring.header.epoch;
            const auto before = read;
            expectEquals ((int) lm_obs_read (&ring.header, ring.pcm.data(), 1, &read, output.data(), 480), -1);
            expectEquals ((juce::int64) read, (juce::int64) before);
        }

        beginTest ("reads work on a read-only mapping and reject an overwrite before it is committed");
        {
            RingStorage initial;
            const auto mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                     initial.header.mapping_bytes, nullptr); // unnamed, never a real OBS mapping
            expect (mapping != nullptr);
            if (mapping != nullptr)
            {
                auto* writer = static_cast<lm_obs_ring_header*> (MapViewOfFile (mapping, FILE_MAP_WRITE, 0, 0, 0));
                const auto* reader = static_cast<const lm_obs_ring_header*> (MapViewOfFile (mapping, FILE_MAP_READ, 0, 0, 0));
                expect (writer != nullptr && reader != nullptr);
                if (writer != nullptr && reader != nullptr)
                {
                    std::memcpy (writer, &initial.header, sizeof (initial.header));
                    auto* pcm = reinterpret_cast<float*> (writer + 1);
                    const auto* readPcm = reinterpret_cast<const float*> (reader + 1);
                    lm_obs_write_silence (writer, pcm, 480);
                    std::array<float, 960> output {};
                    int64_t read = 0;
                    expectEquals ((int) lm_obs_read (reader, readPcm, 1, &read, output.data(), 480), 480);
                    read = 0;
                    lm_obs_store_release (reinterpret_cast<volatile int64_t*> (writer->reserved), LM_OBS_CAPACITY_FRAMES);
                    expectEquals ((juce::int64) writer->write_frames, (juce::int64) 480);
                    expectEquals ((int) lm_obs_read (reader, readPcm, 1, &read, output.data(), 480), -1);
                    expectEquals ((juce::int64) read, (juce::int64) 0);
                }
                if (reader != nullptr) UnmapViewOfFile (reader);
                if (writer != nullptr) UnmapViewOfFile (writer);
                CloseHandle (mapping);
            }
        }

        for (auto rates : { std::pair<double, double> { 48000.0, 48000.0 }, { 48000.0, 44100.0 }, { 44100.0, 48000.0 } })
        {
            beginTest ("1 kHz error below -80 dBFS and DC within 0.01 dB: " + juce::String (rates.first) + " -> " + juce::String (rates.second));
            AsrcStorage storage (2, rates.first, rates.second);
            constexpr int frames = 20000, warmup = 4096;
            std::vector<float> input (50000 * 2), output (frames * 2);
            for (int i = 0; i < 50000; ++i)
            {
                input[(size_t) i * 2] = (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rates.first);
                input[(size_t) i * 2 + 1] = 1.0f;
            }
            int used = 0;
            expectEquals (lm_asrc_process (storage.get(), input.data(), 50000, &used, output.data(), frames), frames);
            expect (lm_asrc_latency_input_frames (storage.get()) >= 15.0 && lm_asrc_latency_input_frames (storage.get()) <= 16.0);
            double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;
            for (int i = warmup; i < frames; ++i)
            {
                const double angle = juce::MathConstants<double>::twoPi * 1000.0 * i / rates.second;
                const double s = std::sin (angle), c = std::cos (angle), y = output[(size_t) i * 2];
                ss += s * s; cc += c * c; sc += s * c; ys += y * s; yc += y * c;
            }
            const double phase = std::atan2 ((yc * ss - ys * sc), (ys * cc - yc * sc));
            double error = 0.0, dcError = 0.0;
            bool validDc = true;
            for (int i = warmup; i < frames; ++i)
            {
                const double ideal = std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rates.second + phase);
                error += std::pow (output[(size_t) i * 2] - ideal, 2.0); // phase fitted, amplitude remains exactly one
                const double dc = output[(size_t) i * 2 + 1];
                validDc = validDc && std::isfinite (dc) && dc > 0.0;
                if (validDc) dcError = std::max (dcError, std::abs (20.0 * std::log10 (dc)));
            }
            const double errorDb = 10.0 * std::log10 (error / (frames - warmup));
            logMessage ("Sine error " + juce::String (errorDb, 3) + " dBFS; peak DC error " + juce::String (dcError, 6) + " dB");
            expect (errorDb < -80.0, "error = " + juce::String (errorDb, 3) + " dBFS");
            expect (validDc && dcError < 0.01, "DC error = " + juce::String (dcError, 6) + " dB");
        }

        beginTest ("+200 ppm consumes 48009.6 +/- one input frame per 48000 output frames");
        {
            AsrcStorage storage (1, 48000.0, 48000.0);
            lm_asrc_set_correction_ppm (storage.get(), 200.0);
            std::vector<float> input (49000, 1.0f), output (48000);
            int used = 0;
            expectEquals (lm_asrc_process (storage.get(), input.data(), (int) input.size(), &used, output.data(), 48000), 48000);
            expectWithinAbsoluteError ((double) used, 48009.6, 1.0);
        }

        beginTest ("uneven input and output chunks, starvation, and reset preserve phase and history");
        for (auto rates : { std::pair<double, double> { 48000.0, 44100.0 }, { 44100.0, 48000.0 } })
        {
            AsrcStorage whole (2, rates.first, rates.second), chunks (2, rates.first, rates.second);
            std::vector<float> input (8192 * 2), reference (7000 * 2), output (7000 * 2);
            for (size_t i = 0; i < input.size(); ++i) input[i] = (float) std::sin ((double) i * 0.17);
            int allUsed = 0;
            expectEquals (lm_asrc_process (whole.get(), input.data(), 8192, &allUsed, reference.data(), 7000), 7000);
            int inPos = 0, outPos = 0, calls = 0;
            while (outPos < 7000 && calls < 30000)
            {
                int used = 0;
                const int available = calls % 7 == 0 ? 0 : std::min (8192 - inPos, 1 + calls % 37);
                const int requested = std::min (7000 - outPos, 1 + calls % 53);
                const int made = lm_asrc_process (chunks.get(), input.data() + inPos * 2, available, &used,
                                                 output.data() + outPos * 2, requested);
                inPos += used; outPos += made; ++calls;
            }
            expectEquals (outPos, 7000);
            expectEquals (inPos, allUsed);
            expect (output == reference);
            lm_asrc_reset (chunks.get());
            int used = 0;
            expectEquals (lm_asrc_process (chunks.get(), input.data(), 8192, &used, output.data(), 7000), 7000);
            expect (output == reference);
        }

        for (double ppm : { 0.0, 200.0, -200.0 })
        {
            beginTest ("two simulated hours, clock error " + juce::String (ppm) + " ppm");
            checkDrift (simulateClocks (ppm));
        }
        beginTest ("+100 to -100 ppm after 30 simulated minutes recovers within 60 seconds");
        checkDrift (simulateClocks (100.0, true));

        for (double ppm : { 1000.0, -1000.0 })
        {
            beginTest ("two simulated hours at the clamp, acquired clock " + juce::String (ppm) + " ppm");
            checkDrift (simulateClocks (ppm, false, true));

            beginTest ("cold start at " + juce::String (ppm) + " ppm settles inside +/-6.25 ms thanks to the fast acquisition slew");
            const auto cold = simulateClocks (ppm);
            // At the clamp there is no authority left to win back the fill lost while the correction ramps. With the
            // 200 ppm/s acquisition slew that is about 273 frames (5.7 ms, measured) instead of 1200 (25 ms) at
            // 20 ppm/s - far from the 1440-frame target's floor. Real interfaces sit well inside +/-200 ppm.
            checkDrift (cold, 300.0);
            logMessage ("Cold-start offset " + juce::String (cold.maxSettledError) + " frames; minimum fill " + juce::String (cold.minFill));
        }

        beginTest ("the slew limit uses the supplied time step and invalid measurements preserve state");
        {
            lm_drift drift;
            lm_drift_init (&drift, 1440.0, 100.0);
            double previous = 0.0;
            for (double dt : { 0.002, 0.02, 0.005, 0.04 })   // acquisition: 200 ppm per second
            {
                const double next = lm_drift_update (&drift, 4000.0, dt);
                expectWithinAbsoluteError (next - previous, 200.0 * dt, 1.0e-10);
                expectWithinAbsoluteError (drift.lp_alpha, 1.0 - std::exp (-dt), 1.0e-12);
                previous = next;
            }
            drift.elapsed = drift.acquire_s;   // acquisition over: 20 ppm per second
            for (double dt : { 0.002, 0.02, 0.005, 0.04 })
            {
                const double next = lm_drift_update (&drift, 4000.0, dt);
                expectWithinAbsoluteError (next - previous, 20.0 * dt, 1.0e-10);
                previous = next;
            }
            expectEquals (lm_drift_update (&drift, 0.0, 0.0), previous);
            expectEquals (lm_drift_update (&drift, 0.0, -1.0), previous);
            expectEquals (lm_drift_update (&drift, std::numeric_limits<double>::quiet_NaN(), 0.01), previous);
        }

        beginTest ("10000 blocks perform no allocator calls, including correction and reset");
        {
            std::vector<std::max_align_t> memory ((lm_test_asrc_size (2) + sizeof (std::max_align_t) - 1) / sizeof (std::max_align_t));
            auto* state = reinterpret_cast<lm_asrc*> (memory.data());
            std::array<float, 1024> input {}, output {};
            allocationCalls = 0;
            lm_test_asrc_init (state, 2, 48000.0, 44100.0);
            expectEquals (allocationCalls, 0); // caller storage also covers the coefficient table at init
            allocationCalls = 0;
            bool full = true;
            for (int i = 0; i < 10000; ++i)
            {
                int used = 0;
                lm_test_asrc_set_correction_ppm (state, i % 2 == 0 ? 200.0 : -200.0);
                full = (lm_test_asrc_process (state, input.data(), 512, &used, output.data(), 440) == 440) && full;
                if (i % 997 == 0) lm_test_asrc_reset (state);
            }
            const int calls = allocationCalls;
            expect (full);
            expectEquals (calls, 0);
        }
    }

private:
    void checkDrift (const DriftStats& stats, double settledBound = 240.0)
    {
        expect (stats.minFill > 0.0, "minimum fill " + juce::String (stats.minFill));
        expect (stats.maxFill < LM_OBS_CAPACITY_FRAMES, "maximum fill " + juce::String (stats.maxFill));
        expect (stats.maxSettledError <= settledBound, "settled error " + juce::String (stats.maxSettledError) + " frames");
        expect (stats.maxPpm <= 1000.0 + 1.0e-9);
        expect (stats.maxSlew <= 200.0 + 1.0e-9, "acquisition slew " + juce::String (stats.maxSlew));
        expect (stats.maxSlewLate <= 20.0 + 1.0e-9, "tracking slew " + juce::String (stats.maxSlewLate));
    }
};

static LiveMixAsrcTests liveMixAsrcTests;
}
