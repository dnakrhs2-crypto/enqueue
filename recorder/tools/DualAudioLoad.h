#pragma once
#include "DualProbeSupport.h"
#include <memory>

namespace gocue::recorder { class RawAudioTap; class WavTrackWriter; }
namespace gocue::recorder::probe
{
void checkAudioQueues(const RawAudioTap&, const WavTrackWriter&, TakeStopSignal&) noexcept;
// Diagnostic adapter over production RawAudioTap/NativePcmConverter/WavTrackWriter.
// ASIO mode requires eight actual active inputs, never duplicates two FlexASIO
// channels into an apparent eight-channel hardware pass.
class DualAudioLoad
{
public:
    struct Config
    {
        bool synthetic = false;
        int asioDevice = 0;
        unsigned seconds = 60;
        juce::File directory;
        FileIoFaultAdapter* faults = nullptr;
    };
    DualAudioLoad(Config, TakeStopSignal&, const std::atomic<std::int64_t>& originQpc);
    ~DualAudioLoad();
    void prepare(); // JUCE message thread; driver callbacks may start in open().
    void stopInput(); // same control owner, closes ASIO before unregistering tap
    void finish(); // after stopInput; drain + WAV stop on worker (not UI)
    juce::var toJson() const; // after finish
    bool complete() const noexcept;
    std::uint64_t queuedWavFrames() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
// Deterministic native PCM24 source oracle shared with offline tests.
std::int32_t audioPattern(std::uint64_t sample, unsigned channel) noexcept;
}
