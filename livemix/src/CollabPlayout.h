#pragma once
#include "CollabProtocol.h"
#include "lm_asrc.h"
#include <atomic>
#include <memory>
#include <vector>

namespace gocue::livemix::collab
{
class CollabPlayout
{
public:
    enum class State { off, buffering, playing };
    struct Stats
    {
        State state;
        double targetMs, fillMs, correctionPpm, lossPercent10s;
        uint64_t received, lost, late, underruns;
    };
    void prepare (double deviceRate, int maxBlock, uint32_t roomRate = defaultWireRate); // lifecycle: both users excluded
    bool push (const Header&, const uint8_t* payload, uint64_t arrivalNs) noexcept; // receiver only
    void pull (float* left, float* right, int frames) noexcept; // audio only
    void rebuffer() noexcept { rebufferRequest.fetch_add (1); }
    Stats getStats() const noexcept;

private:
    static constexpr size_t slotCount = 1024; // at least 2.56 seconds, indexed by wire frame number
    struct Slot
    {
        // CAS gives exclusive access to PCM/metadata, never spins. Release publishes complete packets.
        std::atomic<unsigned> state { 0 }; // empty=0, owned=1, ready=2
        uint64_t frame = 0, epoch = 0;
        std::array<float, framesPerPacket * channels> pcm {};
    };
    void measure (const Header&, uint64_t arrivalNs) noexcept;
    void resetPlayback (uint64_t epoch, bool newSender) noexcept;
    bool startPlayback (int frames) noexcept;
    bool loadPacket() noexcept;
    void beginRefill() noexcept;
    void resetDrift() noexcept;
    int queued() const noexcept;
    void recordPacket (bool missing) noexcept;

    std::unique_ptr<Slot[]> slots;
    std::vector<double> asrcStorage;
    std::vector<float> output;
    lm_asrc* asrc = nullptr;
    lm_drift drift {};
    double deviceRate = defaultWireRate;
    uint32_t wireRate = defaultWireRate;
    double packetMs = packetDurationMs (defaultWireRate);
    int blockSize = 0;

    std::atomic<int> readyFrames { 0 };
    std::atomic<uint64_t> publishedEpoch { 0 }, cursorEpoch { 0 }, cursor { 0 };
    std::atomic<unsigned> rebufferRequest { 0 }, measurementRequest { 0 }, measuredVersion { 0 };
    std::atomic<double> measuredFloor { packetDurationMs (defaultWireRate) };
    std::atomic<double> measuredClock { 0 };
    // Receiver-owned measurement and stream identity.
    uint32_t sender = 0;
    uint64_t producerEpoch = 0, measurementStart = 0;
    unsigned seenMeasurement = 0;
    size_t delayCount = 0;
    bool measuring = true;
    std::array<double, 1024> delays {};
    std::array<double, 1024> frameTimes {};

    // Audio-owned playback, PLC and controller state.
    uint64_t audioEpoch = 0, nextFrame = 0;
    unsigned seenRebuffer = 0, seenVersion = 0;
    bool playing = false, packetReal = false, concealed = false;
    int packetPosition = framesPerPacket, fadeIn = 0, releaseRemaining = 0, releaseFrames = 1;
    int skipAtStart = 0;
    std::array<float, framesPerPacket * channels> packet {};
    std::array<float, 2> lastInput {}, lastOutput {}, releaseTail {};
    double target = 10.0, floor = packetDurationMs (defaultWireRate), stableSeconds = 0.0, lossClock = 0.0;
    // An underrun's raised target is the lowest the stable decay may reach for five minutes: a link whose delay spikes
    // every minute or two must not fall back to the first second's floor between spikes.
    static constexpr double underrunHoldSeconds = 300.0;
    double holdFloor = 0.0, holdSeconds = 0.0;
    std::array<uint32_t, 100> recentTotal {}, recentLost {};
    size_t lossBucket = 0;

    std::atomic<State> state { State::off };
    std::atomic<double> targetMs { 10.0 }, fillMs { 0 }, ppm { 0 }, lossPercent { 0 };
    std::atomic<uint64_t> received { 0 }, lost { 0 }, late { 0 }, underruns { 0 };
};
}
