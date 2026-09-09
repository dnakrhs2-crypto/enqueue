#pragma once
#include "Platform.h"

namespace gocue::recorder
{
// Driver-owned ASIO callbacks keep the driver's scheduling class. Never change
// process priority or enter REALTIME_PRIORITY_CLASS from a capture callback.
enum class RecorderThreadRole { audio, capturePreview, encodeWrite, background };
class ScopedRecorderPriority
{
public:
    explicit ScopedRecorderPriority(RecorderThreadRole role) noexcept
        : previous(GetThreadPriority(GetCurrentThread()))
    {
        const int wanted = role == RecorderThreadRole::audio ? THREAD_PRIORITY_HIGHEST
            : role == RecorderThreadRole::capturePreview ? THREAD_PRIORITY_ABOVE_NORMAL
            : role == RecorderThreadRole::background ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
        applied = SetThreadPriority(GetCurrentThread(), wanted) != FALSE;
        error = applied ? ERROR_SUCCESS : GetLastError();
    }
    ~ScopedRecorderPriority()
    { if (applied && previous != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), previous); }
    DWORD error = ERROR_SUCCESS;
    bool applied = false;
private:
    int previous;
};
}
