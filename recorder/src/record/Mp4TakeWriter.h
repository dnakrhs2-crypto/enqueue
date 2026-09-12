#pragma once
#include "Ffmpeg.h"
#include "support/Platform.h"
#include "storage/RecoverySupport.h"

namespace gocue::recorder
{
// Single mux owner, including AVIO, fragment observations and normal trailer.
// Destruction on an incomplete take closes handles but does not finalize/rename.
class Mp4TakeWriter
{
public:
    static constexpr const char* movFlags = "+hybrid_fragmented+frag_keyframe+empty_moov+default_base_moof";
    Mp4TakeWriter(const juce::File& finalFile, const AVCodecContext& video, const AVCodecContext& audio,
                  FileIoFaultAdapter* = nullptr, recovery::Hook = {});
    ~Mp4TakeWriter();
    Mp4TakeWriter(const Mp4TakeWriter&) = delete;
    void video(const AVPacket&);
    void audio(const AVPacket&);
    void finalize();
    juce::var toJson() const;
    const juce::File& recordingFile() const;
    // Header/packet scan only. Full decode/physical source comparison are external gates.
    static juce::var inspect(const juce::File&);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
