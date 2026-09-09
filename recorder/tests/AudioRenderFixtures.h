#pragma once
#include "playback/TimelineAudioRenderer.h"
#include "playback/ImportedAudioCache.h"
#include <array>

namespace recorder_audio_fixture
{
using namespace gocue::recorder;
// Test-only deterministic PRBS/impulse PCM24. Camera assets are metadata, not fake
// decoded video. The three projects below perform the actual round-13 edits.
struct Fixture
{
    juce::File root;
    RecorderProject project;
    std::array<std::vector<std::int32_t>, 2> pcm;
    std::vector<juce::File> originals;
    explicit Fixture(std::uint32_t Fs = 48000);
    ~Fixture();
    RecorderProject example(unsigned number) const;
    std::vector<juce::String> hashes() const;
    float sample(unsigned channel, Sample at) const;
};
void writePcm24(const juce::File&, std::uint32_t Fs, const std::vector<std::int32_t>&, Sample first, Sample count);
struct StereoRender { std::vector<float> left, right; };
StereoRender render(const RecorderProject&, const std::vector<AudioSourceBinding>&, const AudioSourceMask&, SampleRange, unsigned block = 4096);
}
