#pragma once
#include "diagnostics/CaptureTelemetry.h"
#include <optional>

namespace gocue::recorder
{
// The scheduler accepts already mapped time. Round 04 can replace this adapter
// without changing frame selection or the rational grid.
class CameraTimeMapper
{
public:
    virtual ~CameraTimeMapper() = default;
    virtual std::int64_t map(const FrameStamp&) = 0; // take-relative 100 ns
    virtual std::int64_t now(std::int64_t qpc) const = 0;
    // CFR worker only, immediately before mapping the next valid frame. Keep
    // the reserved take origin and output grid; only the source clock restarts.
    virtual void reanchor() noexcept {}
};
class MfPtsTimeMapper final : public CameraTimeMapper
{
public:
    explicit MfPtsTimeMapper(std::int64_t qpcFrequency);
    std::int64_t map(const FrameStamp&) override;
    std::int64_t now(std::int64_t qpc) const override;
private:
    std::int64_t frequency, originPts = 0, originQpc = 0;
    bool started = false;
};
enum class CfrReason { nativeRateConversion, clockCorrection, captureLoss, encodeLoss };
struct CfrChange { std::uint64_t repeated = 0, omitted = 0, missing = 0; };
struct CfrCounters
{
    std::uint64_t inputs = 0, outputs = 0, repeated = 0, omitted = 0;
    std::int64_t maximumDeliveryDelay100ns = 0;
    std::array<CfrChange, 4> reasons{};
    juce::var toJson() const;
};
struct CfrInput
{
    std::uint64_t sourceId = 0;
    std::int64_t time100ns = 0;
    int slot = -1;
};
struct CfrSelection
{
    CfrInput input;
    std::int64_t pts = 0, grid100ns = 0;
    bool repeated = false;
    CfrReason reason = CfrReason::nativeRateConversion;
    std::array<int, 16> released{};
    size_t releasedCount = 0;
};
// Single worker, fixed storage, no device/GPU/clock reads. Ties select the older
// image. A future candidate ends the wait early; otherwise allow one native
// period plus recent capture/decode delivery delay, in the same mapped clock.
// The total wait is capped; diagnostic lifetime maxima never set deadlines.
class VideoCfrScheduler
{
public:
    static constexpr size_t capacity = 16;
    VideoCfrScheduler(Rational nativeRate, Rational projectRate);
    void push(CfrInput, std::optional<std::int64_t> availableTime100ns = {});
    std::optional<CfrSelection> select(std::int64_t now100ns, bool drain = false);
    void noteLoss(CfrReason, std::uint64_t count);
    // Source reanchor/discontinuity invalidates latency observations, not the
    // take's output grid, queued pictures or lifetime diagnostic counters.
    void resetDeliveryDelay() noexcept;
    const CfrCounters& counters() const noexcept { return stats; }
    std::int64_t nextPts() const noexcept { return next; }
    size_t size() const noexcept { return used; }
    std::array<int, capacity> releaseAll(size_t& count) noexcept;
    static std::int64_t gridTime(std::int64_t index, Rational rate);
    static std::int64_t frameCount(std::int64_t duration100ns, Rational rate);
    static std::int64_t nearestNativeIndex(std::int64_t projectIndex, Rational nativeRate, Rational projectRate);
private:
    std::int64_t deliveryDelay(std::int64_t now100ns) const noexcept;
    // Ten 100 ms buckets keep fixed storage even at high native rates. Round
    // observation times down: a peak expires after 900..1000 ms, including
    // when input stops (select evaluates age, not just push).
    static constexpr std::int64_t deliveryBucket100ns = 1000000;
    struct DeliveryBucket { std::int64_t start = -1, maximum = 0; };
    std::array<DeliveryBucket, 10> deliveryHistory{};
    std::int64_t lastDeliveryTime = -1, deliveryLimit = 0;
    Rational native, project;
    std::array<CfrInput, capacity> frames{};
    size_t used = 0;
    std::int64_t next = 0, previousIdeal = -1, lastTime = -1;
    std::uint64_t lastInputId = 0, selectedId = 0;
    std::optional<CfrReason> pendingLoss;
    CfrCounters stats;
};
}
