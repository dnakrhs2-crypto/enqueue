#pragma once
#include "RecordingRecovery.h"
#include "RecordingTypes.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace gocue::livemix::recording
{
// Used only on the recording writer thread. Input is signed, unshifted PCM24.
class WavSink
{
public:
    ~WavSink();
    bool open(const juce::File&, unsigned rate, std::uint64_t limit = wavSplitBytes,
              std::uint64_t failAfterBytes = RecordingRecovery::unlimited);
    bool write(const std::int32_t* stereo, int frames);
    bool checkpoint();
    bool close();
    std::uint64_t remainingFrames() const noexcept;
    std::uint64_t writtenFrames() const noexcept { return written; }
    std::uint64_t checkpointFrames() const noexcept { return recovery.completedFrames(); }
    juce::String error() const;
    static std::int32_t quantize(float, std::uint64_t& clipped) noexcept;
    static std::int32_t saturate(std::int64_t, std::uint64_t& clipped) noexcept;
private:
    std::unique_ptr<RecordingRecovery::Stream> stream;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    RecordingRecovery recovery;
    std::vector<int> left, right;
    std::uint64_t written = 0, maxFrames = 0;
    juce::String lastError;
};
}
