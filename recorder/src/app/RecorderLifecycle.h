#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <juce_core/juce_core.h>

namespace gocue::recorder
{
enum class RecorderFault { none, camera1Disconnected, camera2Disconnected, storageWrite,
    audioOverflow, audioReset, audioRateChanged, dubbingUnderrun, finalize, processingDelay,
    resume, gpuRemoved, save, recovery, updateBusy, audioInput };
juce::String recorderFaultText(RecorderFault);

// The message owner publishes document/device state; the updater reads only these
// atomics. A successful begin is required BEFORE dispatching work to any worker.
class RecorderLifecycle
{
public:
    enum Activity : std::uint32_t { recording = 1, dubbing = 2, exporting = 4, recovering = 8,
        finalizing = 16, unsaved = 32, fileWork = 64, configuring = 128, closing = 256 };
    bool begin(Activity) noexcept;
    void end(Activity) noexcept;
    void set(Activity, bool) noexcept;
    // Bind once before publishing this state to WinSparkle. The audio engine
    // updates its own independent flag even when a dubbing worker owns capture.
    void bindCaptureBlocker(std::shared_ptr<const std::atomic<bool>> value) { captureBlocker = std::move(value); }
    bool captureBusy() const noexcept { return captureBlocker && captureBlocker->load(); }
    bool canShutdown() const noexcept { return flags.load() == 0 && !captureBusy(); }
    bool acceptsCommands() const noexcept { return !(flags.load() & closing); }
    std::uint32_t snapshot() const noexcept { return flags.load(); }
    std::uint64_t generation() const noexcept { return epoch.load(); }
    bool accepts(std::uint64_t g) const noexcept { return acceptsCommands() && g == generation(); }
    void invalidate() noexcept { ++epoch; }
    void blockCommands() noexcept { set(closing, true); invalidate(); }
private:
    std::atomic<std::uint32_t> flags{0};
    std::atomic<std::uint64_t> epoch{1};
    std::shared_ptr<const std::atomic<bool>> captureBlocker;
};
}
