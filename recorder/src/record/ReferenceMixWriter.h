#pragma once
#include "Ffmpeg.h"
#include "support/Platform.h"

namespace gocue::recorder
{
// Worker-owned AAC-LC. The default constructor retains the round 02 synthetic
// fixture; the explicit-rate constructor accepts actual take PCM, at interface Fs.
class ReferenceMixWriter
{
public:
    ReferenceMixWriter();
    explicit ReferenceMixWriter(unsigned inputSampleRate);
    ~ReferenceMixWriter();
    ReferenceMixWriter(const ReferenceMixWriter&) = delete;
    void advance(std::int64_t presentationSamples, const PacketSink&);
    void finish(std::int64_t presentationSamples, const PacketSink&);
    void append(const float* stereoInterleaved, unsigned frames, const PacketSink&);
    void finishInput(const PacketSink&); // exact input duration, rescaled once to 48k
    const AVCodecContext& context() const;
    juce::var toJson() const;
    static std::int64_t padding(std::int64_t validSamples, int frameSize, int initialPadding);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
