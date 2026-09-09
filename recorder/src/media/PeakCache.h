#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <mutex>
#include <vector>

namespace gocue::recorder
{
struct PeakBin { float minimum = 0, maximum = 0; };
struct PeakSnapshot
{
    unsigned sampleRate = 0, channels = 0;
    std::uint64_t samplesPerBin = 1, samples = 0;
    bool complete = false;
    std::vector<std::array<PeakBin, 8>> bins;
};
// One WAV worker writes; UI copies a bounded summary. No calls on the ASIO thread.
// At the cap, adjacent bins merge without losing extrema or any earlier duration.
class PeakCache
{
public:
    static constexpr std::size_t maximumBins = 16384;
    PeakCache(unsigned Fs, unsigned channels, unsigned samplesPerBin = 0);
    void append(const std::int32_t* interleavedPcm24, unsigned frames, std::uint64_t firstSample);
    void finish();
    PeakSnapshot snapshot() const;
    static juce::Result write(const juce::File&, const PeakSnapshot&);
    static PeakSnapshot read(const juce::File&);
private:
    mutable std::mutex mutex;
    PeakSnapshot state;
};
}
