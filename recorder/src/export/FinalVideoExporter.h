#pragma once
#include "TimelineExporter.h"
#include "record/Ffmpeg.h"

namespace gocue::recorder
{
struct FinalExportSelection
{
    TrackKind video = TrackKind::cam1;
    AudioSourceMask audio{AudioSourceMask::Kind::microphoneMix};
};
struct ExportVideoMapping
{
    Sample outputFrame = 0, timelineSample = 0, sourceSample = 0, sourceFrame = 0;
    Id assetId, clipId;
    bool black() const { return assetId.isEmpty(); }
};
struct ExportVerificationObserver
{
    std::function<void(Sample outputFrame, const AVFrame&)> video;
    // Decoded valid presentation PCM at 48k, excluding priming/padding.
    std::function<void(Sample first, unsigned count, const float*, const float*)> audio;
    std::function<void()> finish; // additional oracle verdict, before manifest/publish
};

// Ordinary moov-at-end MP4, custom seekable DurableFile AVIO. AAC contexts/packets
// retain initial_padding, negative PTS and the final valid packet duration.
// Kept independently inspectable with a software video codec in CPU-only tests;
// the product exporter below always supplies NvencEncoder's H.264 context.
class FinalMp4Writer
{
public:
    FinalMp4Writer(juce::File partialFile, const AVCodecContext& video, const AVCodecContext& audio, FileIoFaultAdapter* = nullptr);
    ~FinalMp4Writer();
    FinalMp4Writer(const FinalMp4Writer&) = delete;
    void video(const AVPacket&);
    void audio(const AVPacket&);
    void finish();
private:
    struct State;
    std::unique_ptr<State> state;
};
class FinalVideoExporter
{
public:
    static AudioSourceMask audioSource(const ExportJob&, const juce::String& selection); // mix | mic:2 | import:<asset ID>
    static void validateSelection(const ExportJob&, const FinalExportSelection&);
    static ExportVideoMapping mappingAt(const ExportJob&, TrackKind camera, Sample outputFrame);
    static juce::var run(const ExportJob&, const FinalExportSelection&, ExportControl&,
                         const ExportVerificationObserver& = {}, FileIoFaultAdapter* = nullptr);
    // Full decode/EOF, stream contract, packet/presentation length, AAC tail.
    // expectedVideoCodec is a CPU-test seam only; run() always requires H.264.
    static juce::var verify(const juce::File&, Sample frames, Sample projectSamples,
                            std::uint32_t Fs, FrameRate, ExportControl&,
                            const ExportVerificationObserver& = {}, AVCodecID expectedVideoCodec = AV_CODEC_ID_H264);
};
}
