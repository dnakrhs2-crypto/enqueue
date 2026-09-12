#pragma once
#include "RecoverySupport.h"
#include "record/Ffmpeg.h"
#include "model/RecorderModel.h"

namespace gocue::recorder
{
struct RecoveredMp4
{
    Sample samples = 0;
    std::uint64_t packets = 0, videoFrames = 0, audioSamples = 0, fragments = 0;
    bool ignoredTail = false;
    OriginalFormat format;
};
// Crash-only index. Built from actual completed tfhd/tfdt/trun + mdat bytes,
// never from pre-mux AVPacket::pos. It is not a concurrent playback reader.
class Mp4RecoveryIndex
{
public:
    explicit Mp4RecoveryIndex(FileIoFaultAdapter* f = nullptr) : log(f) {}
    static juce::File pathFor(const juce::File& mp4);
    void start(const juce::File& index, const AVFormatContext&, const juce::File& initialMp4 = {});
    void fragment(const juce::File& source, std::uint64_t moof, std::uint64_t end);
    void close() { log.close(); }
    static RecoveredMp4 recover(const juce::File& source, const juce::File& index,
                               const juce::File& destination, std::uint32_t Fs,
                               FileIoFaultAdapter* = nullptr);
    // For older intact fMP4s lacking an index. Read only after taking writer locks.
    static void rebuild(const juce::File& source, const juce::File& newIndex);
    static RecoveredMp4 decode(const juce::File&, std::uint32_t Fs);
private:
    recovery::Log log;
};
}
