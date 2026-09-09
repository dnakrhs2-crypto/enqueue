#include "VideoSurfacePool.h"
#include <cassert>

namespace gocue::recorder
{
void VideoSurface::prepare(std::uint32_t w, std::uint32_t h)
{
    if (!w || !h || (w % 2) || (h % 2) || w > 8192 || h > 8192) throw std::invalid_argument("NV12 requires even dimensions <=8192");
    width = w; height = h;
    nv12.resize(static_cast<size_t>(w) * h * 3 / 2);
}
VideoSurfacePool::VideoSurfacePool(std::uint32_t width, std::uint32_t height)
{
    for (auto& surface : surfaces) surface.prepare(width, height);
}
int VideoSurfacePool::acquireWrite() noexcept
{
    for (size_t i = 0; i < size; ++i)
    {
        int expected = free;
        if (states[i].compare_exchange_strong(expected, writing, std::memory_order_acquire)) return static_cast<int>(i);
    }
    return none;
}
bool VideoSurfacePool::publish(int index) noexcept
{
    assert(index >= 0 && states[static_cast<size_t>(index)].load() == writing);
    states[static_cast<size_t>(index)].store(mailbox, std::memory_order_relaxed);
    const int old = latest.exchange(index, std::memory_order_acq_rel);
    if (old != none) states[static_cast<size_t>(old)].store(free, std::memory_order_release);
    return old != none;
}
int VideoSurfacePool::takeLatest() noexcept
{
    const int index = latest.exchange(none, std::memory_order_acquire);
    if (index != none) states[static_cast<size_t>(index)].store(presenting, std::memory_order_relaxed);
    return index;
}
void VideoSurfacePool::release(int index) noexcept
{
    assert(index >= 0 && static_cast<size_t>(index) < size);
    states[static_cast<size_t>(index)].store(free, std::memory_order_release);
}
}
