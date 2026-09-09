#pragma once
#include "capture/CameraCatalog.h"
#include <array>
#include <atomic>

namespace gocue::recorder
{
struct FrameStamp
{
    std::uint64_t frame = 0, generation = 0;
    std::int64_t pts100ns = 0;
    std::uint64_t deviceTimestamp100ns = 0;
    bool hasDeviceTimestamp = false, deviceTimestampValid = false;
    std::int64_t callback = 0, enqueued = 0, worker = 0;
    std::int64_t decodeStart = 0, decodeEnd = 0, normaliseEnd = 0;
    std::int64_t uploadStart = 0, uploadEnd = 0, presentSubmit = 0;
};
enum class LateReason { onTime, queueWait, timestampRegression, cadenceGap };
LateReason classifyLate(std::int64_t previousPts, std::int64_t pts, bool hasPrevious,
                        double queueMs, Rational fps) noexcept;
enum class LossReason : std::size_t
{
    captureDecodeOverflow, lateQueueDiscard, timestampRegression, sourceCadenceGap,
    sourceStreamTick, sourceDiscontinuity, startupDiscontinuity, sourceError, sourceTypeChanged, endOfStream,
    decoderError, previewMailboxOverwrite, surfacePoolExhausted, uploadBusy,
    presentBusy, presentFailure, previewStall, shutdownDiscard, count
};
const char* lossName(LossReason) noexcept;
enum class Timing : std::size_t
{
    callbackInterval, queueWait, sampleCopy, cpuDecode, colourNormalise,
    workerTotal, uploadSubmit, mailboxWait, callbackToPresent, deviceToCallback,
    deviceToPresent, presentCall, presentInterval, count
};
// Bounded histogram, no duration-dependent allocation. Nearest-rank upper bucket edge;
// 0.1 ms resolution through 2000 ms, overflow uses the exact observed maximum.
class Distribution
{
public:
    void add(double milliseconds) noexcept;
    double percentile(double fraction) const noexcept;
    juce::var toJson() const;
    std::uint64_t count() const noexcept { return samples; }
    double max() const noexcept { return maximum; }
private:
    std::array<std::uint64_t, 20002> buckets{};
    std::uint64_t samples = 0;
    double maximum = 0, sum = 0;
};
class CaptureTelemetry
{
public:
    explicit CaptureTelemetry(Rational rate, std::string id = {}) : fps(rate), frequency(qpcFrequency()), pipelineId(std::move(id)) {}
    void loss(LossReason reason, std::uint64_t n = 1) noexcept { losses[static_cast<size_t>(reason)].fetch_add(n, std::memory_order_relaxed); }
    std::uint64_t count(LossReason reason) const noexcept { return losses[static_cast<size_t>(reason)].load(std::memory_order_relaxed); }
    double ms(std::int64_t ticks) const noexcept { return 1000.0 * static_cast<double>(ticks) / static_cast<double>(frequency); }
    bool afterWarmup(std::int64_t qpc) const noexcept { const auto first = firstCallbackQpc.load(); return first && qpc - first >= frequency; }
    bool softwareLossFree() const noexcept;
    void reset(); // only while callback/worker/presenter are stopped or held before measurement
    // One owner per timing: worker writes worker timings; presenter writes present timings.
    // Read the JSON/percentiles only after BOTH threads have joined.
    void duration(Timing stage, double milliseconds) noexcept { timings[static_cast<size_t>(stage)].add(milliseconds); }
    const Distribution& distribution(Timing stage) const noexcept { return timings[static_cast<size_t>(stage)]; }
    void recordWorker(const FrameStamp&);
    void recordPresent(const FrameStamp&, std::int64_t returned);
    juce::var toJson() const;
    static void writeJson(const juce::File&, const juce::var&);
    std::atomic<std::uint64_t> callbacks{0}, samples{0}, decoded{0}, presented{0}, repeatedPresents{0}, lateQueue{0};
    std::atomic<std::uint64_t> queueHighWater{0}, latestReadyFrame{0}, missingDeviceTimestamp{0}, invalidDeviceTimestamp{0}, colourAssumptions{0};
    std::atomic<HRESULT> sourceStatus{S_OK};
    std::atomic<std::int64_t> firstCallbackQpc{0};
    Rational fps;
    std::int64_t frequency;
    const std::string pipelineId; // Immutable camera dimension; survives reset().
    std::atomic<DWORD> capturePriorityError{0}, previewPriorityError{0};
private:
    std::array<std::atomic<std::uint64_t>, static_cast<size_t>(LossReason::count)> losses{};
    std::array<Distribution, static_cast<size_t>(Timing::count)> timings{};
    std::array<FrameStamp, 128> workerTrace{}, presentTrace{};
    std::uint64_t workerTraceCount = 0, presentTraceCount = 0;
};
}
