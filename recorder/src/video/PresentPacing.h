#pragma once
#include <dxgi.h>

namespace gocue::recorder
{
// The consumed latency opportunity is retained until an actual Present succeeds.
// A visibility TEST never replenishes it. Used by the presenter and fault fixtures.
struct PresentPacing
{
    bool retry = false, occluded = false;
    bool needsWait() const noexcept { return !retry; }
    bool needsVisibilityTest() const noexcept { return occluded; }
    void presented(HRESULT hr) noexcept
    {
        retry = hr == DXGI_ERROR_WAS_STILL_DRAWING || hr == DXGI_STATUS_OCCLUDED;
        occluded = hr == DXGI_STATUS_OCCLUDED;
    }
    void visibilityTest(HRESULT hr) noexcept { occluded = hr != S_OK; }
};
inline const char* displayRefreshVerdict(int hz) noexcept
{
    // EnumDisplaySettings reports 59.94 as 59, and 0/1 mean unknown/default.
    return hz <= 1 ? "UNAVAILABLE" : hz >= 59 ? "PASS" : "FAIL";
}
}
