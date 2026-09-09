#pragma once
// Synthetic fixtures only; compiled into tests/harness, never RecorderCore.
#include "storage/RecoveryScanner.h"
#include "storage/Mp4RecoveryIndex.h"
#include "record/Mp4TakeWriter.h"
#include "record/ReferenceMixWriter.h"
#include "record/WavTrackWriter.h"
#include <map>

namespace gocue::recorder::crashFixture
{
juce::File directory(const char* label);
std::int32_t pcm(std::uint64_t sample, unsigned mic);
WavTrackWriter::Config wavConfig(const juce::File&, unsigned mics = 8);
void push(WavTrackWriter&, std::uint64_t first, std::uint32_t frames, unsigned mics);
struct TicketSink : IEditJournalSink
{
    EditDelta ticket;
    juce::Result enqueue(const EditDelta& d) override { ticket = d; return juce::Result::ok(); }
};
class Camera
{
public:
    Camera(const juce::File&, FileIoFaultAdapter* = nullptr, recovery::Hook = {});
    void frame();
    void finish();
    Sample samples() const { return count * 1600; }
private:
    void receive();
    CodecPtr codec;
    FramePtr picture = ffFrame();
    ReferenceMixWriter audio;
    std::unique_ptr<Mp4TakeWriter> mux;
    Sample count = 0;
};
RecorderProject baseline(const juce::File& root);
std::map<juce::String, juce::String> hashes(const juce::File& root, bool originalsOnly = false);
bool verifyPcm(const juce::File& root, const MediaAsset&, unsigned mic);
juce::var largeFaultChecks(const juce::File& root);
}
