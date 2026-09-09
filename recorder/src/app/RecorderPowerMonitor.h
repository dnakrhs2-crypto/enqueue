#pragma once
#include <atomic>
#include <cstdint>
#include <windows.h>
#include <powrprof.h>

namespace gocue::recorder
{
class RecorderPowerMonitor
{
public:
    RecorderPowerMonitor()
    {
        DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters{};
        parameters.Callback = callback;
        error = PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &parameters, &registration);
        seen = serial.load();
    }
    ~RecorderPowerMonitor() { if (registration) PowerUnregisterSuspendResumeNotification(registration); }
    bool poll() noexcept { const auto current = serial.load(); const bool changed = current != seen; seen = current; return changed; }
    DWORD status() const noexcept { return error; }
private:
    // No application pointer escapes into the OS callback. Late notifications
    // touch process-lifetime atomics only; owner consumes them on its next tick.
    static ULONG CALLBACK callback(PVOID, ULONG type, PVOID)
    {
        if (type == PBT_APMSUSPEND || type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) ++serial;
        return ERROR_SUCCESS;
    }
    inline static std::atomic<std::uint64_t> serial{0};
    std::uint64_t seen = 0;
    HPOWERNOTIFY registration = nullptr;
    DWORD error = 0;
};
}
