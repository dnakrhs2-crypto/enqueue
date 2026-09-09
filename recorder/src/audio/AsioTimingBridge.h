#pragma once
#include "AsioTapAbi.h"
#include "support/BoundedSpscQueue.h"
#include <array>
#include <cstddef>

namespace gocue::recorder
{
class RawAudioTap;
struct RunningMoments
{
    std::uint64_t count = 0;
    double mean = 0, m2 = 0, minimum = 0, maximum = 0;
    void add(double) noexcept;
    double standardDeviation() const noexcept;
};
struct AsioRegression
{
    std::size_t observations = 0;
    bool valid = false;
    std::int64_t originQpc = 0, originSamplePosition = 0;
    double samplesPerQpcTick = 0, samplesPerSecond = 0, interceptAtOrigin = 0;
    double residualRmsSamples = 0, residualMaxAbsSamples = 0, residualP95AbsSamples = 0, residualP99AbsSamples = 0;
};
class AsioStampStatistics
{
public:
    static constexpr std::size_t observationCapacity = 4096;
    explicit AsioStampStatistics(std::int64_t qpcFrequency);
    void observe(const BlockStamp&) noexcept; // sole sync worker
    void observePoll(const SamplePositionPoll&) noexcept;
    AsioRegression regression() const; // latest uninterrupted epoch, <=10 seconds and <=4096 points
    AsioRegression pollRegression() const; // separate, unsynchronised driver calls; never merged with callback pairs
    std::uint64_t blocks = 0, samples = 0, timeInfoBlocks = 0, samplePositionBlocks = 0, systemTimeBlocks = 0;
    std::uint64_t qpcRegressions = 0, sampleRegressions = 0, sampleDuplicates = 0, sampleJumps = 0;
    std::uint64_t observationGaps = 0, rateChanges = 0, resetEvents = 0, xrunEvents = 0, resyncEvents = 0, latencyChangeEvents = 0;
    std::uint64_t pollFailures = 0, invalidBlocks = 0;
    BlockStamp first{}, last{};
    std::array<std::uint64_t, 8> systemTimeExamples{};
    std::size_t systemTimeExampleCount = 0;
    RunningMoments callbackIntervalMs, callbackJitterMs, sampleInterval, pollCostUs;
private:
    struct Point { std::int64_t qpc = 0, sample = 0; };
    struct Window
    {
        std::array<Point, observationCapacity> points{};
        std::uint64_t written = 0;
        void add(Point p) noexcept { points[written++ % observationCapacity] = p; }
        void clear() noexcept { written = 0; }
    };
    AsioRegression fit(const Window&) const;
    std::int64_t frequency;
    Window callbackWindow, pollWindow;
};
class AsioTimingBridge
{
public:
    static constexpr std::size_t stampQueueCapacity = 1024, pollQueueCapacity = 128;
    explicit AsioTimingBridge(std::int64_t qpcFrequency, RawAudioTap* raw = nullptr);
    ~AsioTimingBridge(); // owner must have closed the driver
    AsioTimingBridge(const AsioTimingBridge&) = delete;
    AsioTimingBridge& operator=(const AsioTimingBridge&) = delete;
    // Single control owner; prepare/register before device.open(), close() before
    // unregister. JUCE open() itself starts buffer switches, before start(callback).
    bool registerTap() noexcept;
    void unregisterAfterDeviceClosed() noexcept;
    void onAsioBlock(const BlockStamp&, const NativeInputView*, std::uint32_t) noexcept;
    bool enqueueStamp(const BlockStamp&) noexcept;
    bool enqueuePoll(const SamplePositionPoll&) noexcept; // one control producer, separate SPSC
    using StampConsumer = void (*)(void*, const BlockStamp&) noexcept;
    // Consumer runs on the sync worker, after statistics, for ClockMapper etc.
    void drain(StampConsumer = nullptr, void* context = nullptr) noexcept;
    const AsioStampStatistics& statistics() const noexcept { return stats; } // after worker join
    std::uint64_t droppedStamps() const noexcept { return dropped.load(std::memory_order_relaxed); }
    std::uint64_t droppedPolls() const noexcept { return pollsDropped.load(std::memory_order_relaxed); }
    std::size_t stampHighWater() const noexcept { return peak.load(std::memory_order_relaxed); }
    static bool hookCompiled() noexcept;
    static bool poll(juce::AudioIODevice&, SamplePositionPoll&) noexcept;
    static bool readEvents(const juce::AudioIODevice&, AsioEventCounters&) noexcept;
private:
    static void dispatch(const BlockStamp&, const NativeInputView*, std::uint32_t) noexcept;
    static AsioTimingBridge* owner; // immutable while ASIO is running; published by tap release store
    RawAudioTap* raw;
    bool registered = false;
    BoundedSpscQueue<BlockStamp, stampQueueCapacity> stamps;
    BoundedSpscQueue<SamplePositionPoll, pollQueueCapacity> polls;
    std::atomic<std::uint64_t> dropped{0}, pollsDropped{0};
    std::atomic<std::size_t> peak{0};
    AsioStampStatistics stats;
};
}
