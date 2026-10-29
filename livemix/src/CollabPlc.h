#pragma once
#include <array>
#include <vector>

namespace gocue::livemix::collab
{
// Audio-thread-only waveform replacement. prepare() allocates; all other methods
// only use that storage. Work at the device rate so packet holes and refill share
// one history/clock, without changing the ASRC or delaying good audio.
class CollabPlc
{
public:
    struct Tuning
    {
        static constexpr double minPitchMs = 2.5, maxPitchMs = 15.0;
        static constexpr double correlationMs = 15.0, historyMs = 48.75;
        static constexpr double searchRate = 8000.0, minCorrelation = 0.7, minRms = 0.0001;
        static constexpr double fullLevelMs = 10.0, attenuationStepMs = 10.0;
        static constexpr double attenuationPerStep = 0.2, muteMs = 60.0, fallbackMs = 2.5;
    };
    void prepare (double rate);
    void reset() noexcept;
    void receive (float* interleaved, int frames) noexcept;
    void conceal (float* interleaved, int frames) noexcept;
    bool needsRecovery() const noexcept { return erasing || recoveryLeft > 0; }

private:
    void remember (const float*, int frames) noexcept;
    void beginLoss() noexcept;
    double findPitch() noexcept;
    float sample (double position, int channel) const noexcept;
    float loop (int time, int periods, int channel) const noexcept;
    std::array<float, 2> prediction (int time) const noexcept;
    std::array<float, 2> synthesize() noexcept;

    std::vector<float> history, frozen, mono, coarse;
    std::vector<double> coarseCorrelation;
    int historyFrames = 0, written = 0, available = 0, frozenFrames = 0, decimation = 1;
    int minPitch = 0, maxPitch = 0, correlationFrames = 0;
    int fullLevelFrames = 0, stepFrames = 0, muteFrames = 0, fallbackFrames = 0;
    int elapsed = 0, recoveryFrames = 0, recoveryLeft = 0;
    int joinStart = 0, joinPosition = 0;
    double pitch = 0, joinFrames = 1;
    // erasing: missing audio; recoveryLeft > 0: overlap; otherwise real audio.
    // Only an interrupted overlap continues the old waveform/attenuation clock.
    bool erasing = false;
    // tail is the rendered boundary only; it is never used as pitch history.
    std::array<float, 2> tail {}, realTail {}, joinCorrection {}, realJoin {};
};
}
