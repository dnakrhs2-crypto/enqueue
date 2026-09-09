#pragma once
#include "Ffmpeg.h"
#include "video/VideoSurfacePool.h"
#include "support/BoundedSpscQueue.h"
#include <deque>

namespace gocue::recorder
{
struct NvencProfile
{
    int fps = 60;
    std::string preset = "p5";
    static constexpr int hardwareSurfaces = 4;
    void validate() const;
    int surfaceLimit() const { return fps == 60 ? 15 : 8; }
    int cpuSurfaces() const { return surfaceLimit() - hardwareSurfaces; }
    std::int64_t bitRate() const { return fps == 60 ? 35000000 : 20000000; }
    std::int64_t maxRate() const { return fps == 60 ? 50000000 : 30000000; }
    juce::var toJson() const;
};
// Single decode producer, single CFR/encode consumer. All buffers allocated at
// prepare; neither queue overflow nor an encoder retaining a ref borrows preview.
class NvencFramePool
{
public:
    NvencFramePool(int capacity, int width = 1920, int height = 1080);
    bool copy(const VideoSurface&) noexcept;
    bool pop(int& slot) noexcept { return queue.pop(slot); }
    const AVFrame& frame(int slot) const { return *frames.at(static_cast<size_t>(slot)); }
    const FrameStamp& stamp(int slot) const { return stamps.at(static_cast<size_t>(slot)); }
    void release(int slot) noexcept;
    unsigned highWater() const noexcept { return maximum.load(); }
    unsigned occupied() const noexcept { return occupancy.load(); }
    int capacity() const noexcept { return limit; }
private:
    int limit, width, height;
    std::array<FramePtr, 15> frames;
    std::array<FrameStamp, 15> stamps{};
    std::array<std::atomic<bool>, 15> owned{};
    std::atomic<unsigned> occupancy{0}, maximum{0};
    BoundedSpscQueue<int, 15> queue;
};
class NvencEncoder
{
public:
    // Configuration allocation/options can be inspected without opening a GPU.
    static CodecPtr configuredContext(const NvencProfile&);
    explicit NvencEncoder(NvencProfile);
    void open();
    void submit(const AVFrame&, std::int64_t pts, const PacketSink&);
    void drain(const PacketSink&);
    const AVCodecContext& context() const { return *codec; }
    juce::var toJson() const;
private:
    void receive(const PacketSink&, bool draining);
    NvencProfile profile;
    CodecPtr codec;
    FramePtr view = ffFrame();
    PacketPtr packet = ffPacket();
    std::deque<std::int64_t> pending;
    bool opened = false, drained = false;
    std::int64_t lastInput = -1, lastOutput = -1, busyTicks = 0, maxSubmitTicks = 0;
    std::uint64_t submitted = 0, packets = 0, keyframes = 0, pendingMax = 0;
};
}
