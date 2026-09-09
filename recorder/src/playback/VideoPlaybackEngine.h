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
struct PlaybackDecodeTiming
{
    std::int64_t idrSeekTicks = 0, flushTicks = 0, decodeTicks = 0, convertTicks = 0;
    std::size_t idrPacket = 0, targetPacket = 0, receivedFrames = 0;
    bool fromIdr = false;
};
class IVideoFrameDecoder
{
public:
    virtual ~IVideoFrameDecoder() = default;
    // Camera worker only: retain the device/codec, invalidate sequential decode
    // position after a seek (including a cancelled decode that advanced the DPB).
    virtual void resetForSeek() {}
    virtual PlaybackDecodeTiming decodeTiming() const { return {}; }
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
// A successful DXGI latency wait belongs to the next successful Present, even
// when a seek, busy texture, resize or occlusion prevents this iteration's submit.
// Device-free seam for the same state machine used by the playback presenter.
struct PlaybackPresentOpportunity
{
    bool held = false;
    bool needsWait() const noexcept { return !held; }
    void acquired() noexcept { held = true; }
    void submitted(bool accepted) noexcept { if (accepted) held = false; }
};
struct PlaybackSeekTiming
{
    std::uint64_t generation = 0;
    Sample target = 0;
    std::int64_t requestedQpc = 0, workerQpc = 0;
    std::int64_t decoderBeginQpc = 0, decoderEndQpc = 0;
    std::int64_t decodeBeginQpc = 0, decodeEndQpc = 0, readyQpc = 0, presentQpc = 0;
    bool cacheKnown = false, cacheHit = false;
    PlaybackDecodeTiming decode;
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
    bool requestFrame(unsigned camera, Sample, std::uint64_t generation, bool advancing = true);
    bool ready(Sample, std::uint64_t generation) const;
    void attachPlaybackView(unsigned camera, void* hwnd); // same left/right HWND host as live
    void stop(); // join present, then decoder workers; no callback owns textures
    juce::Result status() const;
    juce::var telemetry() const;
    PlaybackPresentation lastPresentation(unsigned camera) const;
    PlaybackSeekTiming seekTiming(unsigned camera) const;
    // GPU-present-thread endpoints. Late frames retain the previous picture until
    // the next audio-cursor selection; gaps clear it. No independent video clock.
    // Ordinary inspections (including gap UI and startup polling) never count as
    // late. Only the present thread marks a real advancing presentation tick.
    PlaybackDisplaySelection displaySelection(unsigned camera, bool presentationTick = false) const;
    void presented(unsigned camera, const PlaybackVideoFrame&, std::int64_t qpc);
    void presenterInitialised(unsigned camera, std::int64_t beginQpc, std::int64_t endQpc);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
