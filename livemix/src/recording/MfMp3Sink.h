#pragma once
#include "RecordingRecovery.h"
#include "RecordingTypes.h"
#include <memory>

namespace gocue::livemix::recording
{
// COM, MF, resampling and random state all belong to the calling writer thread.
class MfMp3Sink
{
public:
    MfMp3Sink();
    ~MfMp3Sink();
    bool open(const juce::File&, double deviceRate, unsigned kbps,
              std::uint64_t failAfterBytes = RecordingRecovery::unlimited, bool unavailableForTest = false);
    bool write(const float* stereo, int frames);
    bool checkpoint();
    bool close(); // EOS -> drain -> all complete MP3 frames -> file flush/close
    unsigned sampleRate() const noexcept;
    std::uint64_t writtenFrames() const noexcept; // complete MPEG audio frames on disk, including codec padding
    std::uint64_t checkpointFrames() const noexcept;
    std::uint64_t clippedSamples() const noexcept;
    Error errorCode() const noexcept;
    juce::String error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
