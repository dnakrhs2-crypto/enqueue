#pragma once
#include "media/MediaIndex.h"
#include <functional>
#include <optional>
#include <deque>

namespace gocue::recorder
{
struct PlaybackTexture; // GPU-only immutable texture; final releases on GPU/control workers
// Auto-reset notification, one waiter per instance. Signal before wait is retained.
// Construct on the control/worker thread; signal() only calls SetEvent (no allocation).
class PlaybackWakeEvent
{
public:
    PlaybackWakeEvent();
    ~PlaybackWakeEvent();
    PlaybackWakeEvent(const PlaybackWakeEvent&) = delete;
    PlaybackWakeEvent& operator=(const PlaybackWakeEvent&) = delete;
    void signal() const noexcept;
    void* nativeHandle() const noexcept { return handle; }
private:
    void* handle = nullptr;
};
struct PlaybackDecodePlan
{
    std::size_t firstPacket = 0, targetPacket = 0, decodeOnlyFrames = 0;
    bool fromIdr = true;
    bool convert(std::int64_t pts, const VideoIndex& source) const
    { return pts == source.packets.at(targetPacket).pts; }
};
PlaybackDecodePlan playbackDecodePlan(const VideoIndex&, std::size_t target,
                                      std::optional<std::size_t> lastDecoded = {});
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
// Non-RT LRU. Payload identity is source epoch + PTS, independent of an edit's
// clip ID and timeline placement. Cache wrappers never carry publication rights.
class PlaybackFrameCache
{
public:
    struct Limits { std::size_t frames = 4, bytes = 32 * 1024 * 1024, gops = 2; };
    struct Stats { std::size_t frames = 0, bytes = 0, peakBytes = 0; std::uint64_t hits = 0, misses = 0, evictions = 0; };
    PlaybackFrameCache();
    explicit PlaybackFrameCache(Limits);
    std::shared_ptr<const PlaybackVideoFrame> find(const std::shared_ptr<const VideoIndex>&, std::size_t packet);
    void insert(std::shared_ptr<const PlaybackVideoFrame>);
    void clear();
    Stats stats() const noexcept { return counters; }
    const Limits limits;
private:
    struct Entry { std::shared_ptr<const PlaybackVideoFrame> frame; std::size_t bytes, gop; };
    void prune();
    void evict(std::size_t);
    std::deque<Entry> entries;
    Stats counters;
};
struct PlaybackDecodeTiming
{
    std::int64_t idrSeekTicks = 0, flushTicks = 0, decodeTicks = 0, convertTicks = 0;
    std::size_t idrPacket = 0, targetPacket = 0, receivedFrames = 0;
    std::size_t plannedDecodeOnlyFrames = 0, decodeOnlyFrames = 0, convertedFrames = 0;
    std::int64_t seekBeginQpc = 0, seekEndQpc = 0, prefixBeginQpc = 0, prefixEndQpc = 0;
    std::int64_t convertBeginQpc = 0, resourcesReadyQpc = 0, gpuSubmittedQpc = 0, gpuCompleteQpc = 0;
    bool fromIdr = false;
};
class IVideoFrameDecoder
{
public:
    virtual ~IVideoFrameDecoder() = default;
    // Camera worker only: retain the device/codec, invalidate sequential decode
    // position after an interrupted decode. Completed forward seeks reuse the DPB.
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
    bool buffering = false;
};
enum class PlaybackDisplayAction { skip, picture, clear };
struct PlaybackDisplayDecision
{
    std::shared_ptr<const PlaybackVideoFrame> frame;
    PlaybackDisplayAction action = PlaybackDisplayAction::skip;
    bool shouldSubmit() const noexcept { return action != PlaybackDisplayAction::skip; }
};
// Device-free picture retention and submission policy, shared with the DXGI presenter. Only a
// successful submission becomes the retained picture.
struct PlaybackDisplayState
{
    std::shared_ptr<const PlaybackVideoFrame> retained;
    PlaybackDisplayDecision select(const PlaybackDisplaySelection& selection)
    {
        // A seek revokes publication rights, not the pixels already on screen.
        // Dropping an invalid reference does not erase those pixels. Keep the
        // clear pending across generation changes/busy/occluded submit retries.
        if (selection.gap || (retained && (!retained->source || !retained->source->current())))
        {
            clearPending |= bool(retained);
            retained.reset();
        }
        if (selection.gap) return {{}, PlaybackDisplayAction::clear};
        if (selection.frame) return {selection.frame, PlaybackDisplayAction::picture};
        if (retained) return {retained, PlaybackDisplayAction::picture};
        return {{}, clearPending ? PlaybackDisplayAction::clear : PlaybackDisplayAction::skip};
    }
    // Call only after a successful Present; a replacement picture also fulfils
    // a pending clear, without inserting black ahead of an already ready frame.
    void submitted(std::shared_ptr<const PlaybackVideoFrame> frame)
    { retained = std::move(frame); clearPending = false; }
private:
    bool clearPending = false;
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
    static constexpr unsigned prerollMilliseconds = 250, maximumReadyFrames = 4;
    // Called on independent current/preroll workers (up to two per camera).
    using DecoderFactory = std::function<std::unique_ptr<IVideoFrameDecoder>(std::shared_ptr<const VideoIndex>)>;
    explicit VideoPlaybackEngine(DecoderFactory = {}); // empty = H.264 + D3D11VA only
    ~VideoPlaybackEngine();
    void prepare(std::vector<PlaybackVideoClip>); // indexed immutable clip set; no TakeController
    // Control owner: atomically replace both lanes after audio's block-boundary
    // acknowledgement. Existing HWNDs survive; old workers cancel by generation.
    void handoff(std::vector<PlaybackVideoClip>, Sample, std::uint64_t generation);
    std::uint64_t seek(Sample);
    void seek(Sample, std::uint64_t generation);
    bool requestFrame(unsigned camera, Sample, std::uint64_t generation, bool advancing = true);
    bool requestFrames(Sample audibleSample, std::uint64_t generation, bool advancing = true);
    bool ready(Sample, std::uint64_t generation) const;
    void attachPlaybackView(unsigned camera, void* hwnd); // same left/right HWND host as live
    void stop(); // join present, then decoder workers; no callback owns textures
    juce::Result status() const;
    juce::var telemetry() const;
    PlaybackPresentation lastPresentation(unsigned camera) const;
    PlaybackSeekTiming seekTiming(unsigned camera) const;
    // One non-RT coordinator waiter; independent from each present-thread event.
    void setWakeEvent(std::shared_ptr<PlaybackWakeEvent>);
    void* presentationWakeHandle(unsigned camera) const;
    // GPU-present-thread endpoints. Late frames retain the previous picture until
    // the next audio-cursor selection; gaps clear it. No independent video clock.
    // Ordinary inspections (including gap UI and readiness events) never count as
    // late. Only the present thread marks a real advancing presentation tick.
    PlaybackDisplaySelection displaySelection(unsigned camera, bool presentationTick = false) const;
    void presented(unsigned camera, const PlaybackVideoFrame&, std::int64_t qpc);
    bool submitIfCurrent(unsigned camera, std::uint64_t generation, const std::function<void()>& submit);
    void presenterInitialised(unsigned camera, std::int64_t beginQpc, std::int64_t endQpc);
    void presenterFailed(unsigned camera, const juce::String&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
