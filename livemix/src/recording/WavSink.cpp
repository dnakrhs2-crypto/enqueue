#include "WavSink.h"
#include <algorithm>
#include <cmath>

namespace gocue::livemix::recording
{
WavSink::~WavSink() { close(); }
bool WavSink::open(const juce::File& path, unsigned rate, std::uint64_t limit, std::uint64_t failBytes)
{
    if (stream || path.exists()) return false;
    stream = std::make_unique<RecordingRecovery::Stream>(path, failBytes);
    if (!stream->ok()) return false;
    juce::WavAudioFormat format;
    auto adapter = RecordingRecovery::borrow(*stream);
    writer = format.createWriterFor(adapter, juce::AudioFormatWriterOptions().withSampleRate(rate)
                                      .withNumChannels(2).withBitsPerSample(static_cast<int>(wavBitsPerSample)));
    if (!writer) return false;
    const auto headerBytes = static_cast<std::uint64_t>(stream->getPosition());
    limit = std::min(limit, wavSplitBytes);
    if (limit < headerBytes + 6) return false;
    maxFrames = (limit - headerBytes) / 6;
    return checkpoint();
}
bool WavSink::write(const std::int32_t* stereo, int n)
{
    if (!writer || n < 0 || static_cast<std::uint64_t>(n) > remainingFrames()) return false;
    left.resize(static_cast<std::size_t>(n)); right.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        // Multiplication avoids undefined left shift of negative signed samples.
        left[static_cast<std::size_t>(i)] = stereo[i * 2] * 256;
        right[static_cast<std::size_t>(i)] = stereo[i * 2 + 1] * 256;
    }
    const int* channels[] { left.data(), right.data(), nullptr };
    if (!writer->write(channels, n) || !stream->ok()) return false;
    written += static_cast<unsigned>(n);
    return true;
}
bool WavSink::checkpoint()
{
    return writer && writer->flush() && recovery.checkpoint(*stream, written);
}
bool WavSink::close()
{
    if (!stream) return true;
    writer.reset(); // destructor updates header; the borrowed stream stays alive
    const bool ok = recovery.checkpoint(*stream, written);
    lastError = stream->error();
    stream.reset();
    return ok;
}
std::uint64_t WavSink::remainingFrames() const noexcept { return maxFrames - written; }
juce::String WavSink::error() const { return stream ? stream->error() : lastError; }
std::int32_t WavSink::saturate(std::int64_t x, std::uint64_t& clips) noexcept
{
    if (x < -8388608) { ++clips; return -8388608; }
    if (x > 8388607) { ++clips; return 8388607; }
    return static_cast<std::int32_t>(x);
}
std::int32_t WavSink::quantize(float x, std::uint64_t& clips) noexcept
{
    if (!std::isfinite(x)) { ++clips; return 0; }
    if (x < -1.0f) { ++clips; return -8388608; }
    if (x > 8388607.0f / 8388608.0f) { ++clips; return 8388607; }
    return static_cast<std::int32_t>(std::llround(static_cast<double>(x) * 8388608.0));
}
}
