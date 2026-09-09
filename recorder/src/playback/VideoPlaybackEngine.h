#pragma once
#include "media/MediaIndex.h"
#include <functional>

namespace gocue::recorder
{
struct PlaybackTexture; // GPU-only immutable texture; final releases on GPU/control workers
struct PlaybackVideoClip
{
    RenderClip mapping;
    unsigned camera = 0; // cam1=0, cam2=1
    std::shared_ptr<const VideoIndex> source;
};
struct PlaybackVideoFrame
{
    Id clipId;
    Sample begin = 0, end = 0;
    std::int64_t pts = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<const VideoIndex> source;
    std::shared_ptr<const PlaybackTexture> texture;
    bool current(std::uint64_t g) const noexcept { return generation == g && source && source->current(); }
};
class IVideoFrameDecoder
{
public:
    virtual ~IVideoFrameDecoder() = default;
    virtual std::shared_ptr<const PlaybackTexture> decodeFrame(std::size_t packet,
                                                              const std::function<bool()>& cancelled) = 0;
};
struct PlaybackPresentation
{
    std::uint64_t generation = 0;
    Sample begin = 0, end = 0;
    std::int64_t pts = 0, qpc = 0;
};
struct PlaybackDisplaySelection
{
    std::shared_ptr<const PlaybackVideoFrame> frame;
    std::uint64_t generation = 0;
    bool gap = true;
};
class VideoPlaybackEngine
{
public:
    using DecoderFactory = std::function<std::unique_ptr<IVideoFrameDecoder>(std::shared_ptr<const VideoIndex>)>;
    explicit VideoPlaybackEngine(DecoderFactory = {}); // empty = H.264 + D3D11VA only
    ~VideoPlaybackEngine();
    void prepare(std::vector<PlaybackVideoClip>); // indexed immutable clip set; no TakeController
    std::uint64_t seek(Sample);
    void seek(Sample, std::uint64_t generation);
    bool requestFrame(unsigned camera, Sample, std::uint64_t generation);
    bool ready(Sample, std::uint64_t generation) const;
    void attachPlaybackView(unsigned camera, void* hwnd); // same left/right HWND host as live
    void stop(); // join present, then decoder workers; no callback owns textures
    juce::Result status() const;
    juce::var telemetry() const;
    PlaybackPresentation lastPresentation(unsigned camera) const;
    // GPU-present-thread endpoints. Late frames retain the previous picture until
    // the next audio-cursor selection; gaps clear it. No independent video clock.
    PlaybackDisplaySelection displaySelection(unsigned camera) const;
    void presented(unsigned camera, const PlaybackVideoFrame&, std::int64_t qpc);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
