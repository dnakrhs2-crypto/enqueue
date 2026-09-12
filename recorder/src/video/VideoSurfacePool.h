#pragma once
#include "diagnostics/CaptureTelemetry.h"
#include <vector>

namespace gocue::recorder
{
// Normalised CPU planes owned by one producer and one presenter. D3D resources/context
// belong exclusively to PreviewPresenter. Each camera owns an independent instance.
struct VideoSurface
{
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> nv12;
    FrameStamp stamp;
    bool colourAssumed = false;
    void prepare(std::uint32_t w, std::uint32_t h);
    std::uint8_t* y() noexcept { return nv12.data(); }
    std::uint8_t* uv() noexcept { return nv12.data() + static_cast<size_t>(width) * height; }
    static constexpr const char* outputFormat = "NV12 BT.709 limited SDR 8-bit 4:2:0";
};
class VideoSurfacePool
{
public:
    static constexpr int none = -1;
    static constexpr size_t size = 3; // producer + latest mailbox + presenter
    struct Snapshot
    {
        std::uint64_t acquired = 0, published = 0, consumed = 0, overwritten = 0, exhausted = 0;
    };
    VideoSurfacePool(std::uint32_t width, std::uint32_t height);
    int acquireWrite() noexcept;
    // Returns true when an unconsumed mailbox frame was replaced; frees it on the producer.
    bool publish(int index) noexcept;
    // Consumer transfers ownership of the latest slot; release before taking another.
    int takeLatest() noexcept;
    // CPU data is needed only until staging upload returns, including failure/exception.
    template<class Upload> void uploadLatest(Upload&& upload)
    {
        const int index = takeLatest();
        if (index == none) return;
        struct Release { VideoSurfacePool& pool; int index; ~Release() { pool.release(index); } } release{*this, index};
        upload(surface(index));
    }
    void release(int index) noexcept;
    VideoSurface& surface(int index) noexcept { return surfaces[static_cast<size_t>(index)]; }
    Snapshot snapshot() const noexcept;
private:
    enum State { free, writing, mailbox, presenting };
    std::array<VideoSurface, size> surfaces;
    std::array<std::atomic<int>, size> states{};
    std::atomic<int> latest{none};
    std::atomic<std::uint64_t> acquired{0}, publications{0}, consumed{0}, overwritten{0}, exhausted{0};
};
}
