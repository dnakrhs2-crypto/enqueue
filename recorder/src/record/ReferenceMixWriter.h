#pragma once
#include "Ffmpeg.h"
#include "support/Platform.h"

namespace gocue::recorder
{
// Synthetic reference only; owned by the mux worker. No ASIO input or master clock.
// 44.1 kHz interleaved float -> swresample -> 48 kHz stereo planar float -> AAC LC.
class ReferenceMixWriter
{
public:
    ReferenceMixWriter();
    ~ReferenceMixWriter();
    ReferenceMixWriter(const ReferenceMixWriter&) = delete;
    void advance(std::int64_t presentationSamples, const PacketSink&);
    void finish(std::int64_t presentationSamples, const PacketSink&);
    const AVCodecContext& context() const;
    juce::var toJson() const;
    static std::int64_t padding(std::int64_t validSamples, int frameSize, int initialPadding);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
