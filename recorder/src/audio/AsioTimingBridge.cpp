#include "AsioTimingBridge.h"
#include "RawAudioTap.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace gocue::recorder
{
std::atomic<AsioTapFunction> recorderAsioTap{nullptr};
AsioTimingBridge* AsioTimingBridge::owner = nullptr;
namespace
{
double delta(std::int64_t a, std::int64_t b) noexcept
{
    // Subtract integer origins BEFORE converting; retain 1-sample/1-tick detail
    // when a and b are beyond the exact integer range of double.
    if ((a < 0) == (b < 0)) return static_cast<double>(a - b);
    return static_cast<double>(a) - static_cast<double>(b);
}
std::uint64_t eventDelta(std::uint64_t value, std::uint64_t previous) noexcept
{
    return value >= previous ? value - previous : value;
}
}
void RunningMoments::add(double x) noexcept
{
    if (!std::isfinite(x)) return;
    if (count == 0) minimum = maximum = x;
    minimum = std::min(minimum, x); maximum = std::max(maximum, x);
    const double d = x - mean; mean += d / static_cast<double>(++count); m2 += d * (x - mean);
}
double RunningMoments::standardDeviation() const noexcept { return count ? std::sqrt(std::max(0.0, m2 / static_cast<double>(count))) : 0; }
AsioStampStatistics::AsioStampStatistics(std::int64_t hz) : frequency(hz)
{
    if (hz <= 0) throw std::invalid_argument("QPC frequency must be positive");
}
void AsioStampStatistics::observe(const BlockStamp& s) noexcept
{
    const bool havePrevious = blocks != 0;
    if (!havePrevious) first = s;
    ++blocks; samples += s.numSamples;
    timeInfoBlocks += (s.flags & timeInfoPresent) != 0;
    samplePositionBlocks += (s.flags & samplePositionValid) != 0;
    systemTimeBlocks += (s.flags & systemTimeValid) != 0;
    if ((s.flags & systemTimeValid) && systemTimeExampleCount < systemTimeExamples.size()) systemTimeExamples[systemTimeExampleCount++] = s.systemTimeRaw;
    const bool invalid = s.numSamples == 0 || s.bufferIndex < 0 || s.bufferIndex > 1 || s.callbackQpc <= 0
        || !std::isfinite(s.sampleRate) || s.sampleRate <= 0;
    invalidBlocks += invalid;
    const auto resets = havePrevious ? eventDelta(s.resets, last.resets) : 0;
    const auto resyncs = havePrevious ? eventDelta(s.resyncs, last.resyncs) : 0;
    resetEvents += resets; resyncEvents += resyncs;
    xrunEvents += havePrevious ? eventDelta(s.xruns, last.xruns) : 0;
    latencyChangeEvents += havePrevious ? eventDelta(s.latencyChanges, last.latencyChanges) : 0;
    bool discontinuity = invalid || resets != 0 || resyncs != 0;
    if (havePrevious)
    {
        const double ticks = delta(s.callbackQpc, last.callbackQpc);
        if (ticks <= 0) { ++qpcRegressions; discontinuity = true; }
        else
        {
            const double interval = ticks * 1000.0 / static_cast<double>(frequency);
            callbackIntervalMs.add(interval);
            if (last.sampleRate > 0 && std::isfinite(last.sampleRate) && s.sequence == last.sequence + 1)
                callbackJitterMs.add(interval - last.numSamples * 1000.0 / last.sampleRate);
        }
        if (s.sequence != last.sequence + 1) { ++observationGaps; discontinuity = true; }
        if (s.sampleRate != last.sampleRate) { ++rateChanges; discontinuity = true; }
        if ((s.flags & samplePositionValid) && (last.flags & samplePositionValid))
        {
            const auto ds = delta(s.samplePosition, last.samplePosition);
            sampleInterval.add(ds);
            if (ds < 0) { ++sampleRegressions; discontinuity = true; }
            else if (ds == 0) { ++sampleDuplicates; discontinuity = true; }
            else if (!discontinuity && ds != last.numSamples) { ++sampleJumps; discontinuity = true; }
        }
        else discontinuity = true; // never connect across missing sample-position validity
    }
    if (discontinuity) callbackWindow.clear();
    if (!invalid && (s.flags & samplePositionValid)) callbackWindow.add({s.callbackQpc, s.samplePosition});
    last = s;
}
void AsioStampStatistics::observePoll(const SamplePositionPoll& p) noexcept
{
    if (p.qpcAfter >= p.qpcBefore) pollCostUs.add(delta(p.qpcAfter, p.qpcBefore) * 1000000.0 / static_cast<double>(frequency));
    if (!p.valid || p.asioError != 0 || p.qpcAfter < p.qpcBefore) { ++pollFailures; pollWindow.clear(); return; }
    const auto qpc = p.qpcBefore + (p.qpcAfter - p.qpcBefore) / 2;
    if (pollWindow.written)
    {
        const auto previous = pollWindow.points[(pollWindow.written - 1) % observationCapacity];
        if (qpc <= previous.qpc || p.samplePosition < previous.sample) pollWindow.clear();
    }
    pollWindow.add({qpc, p.samplePosition});
}
AsioRegression AsioStampStatistics::fit(const Window& window) const
{
    AsioRegression r;
    const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(window.written, observationCapacity));
    if (!available) return r;
    const auto lastPoint = window.points[(window.written - 1) % observationCapacity];
    std::vector<Point> points; points.reserve(available);
    for (std::uint64_t i = window.written - available; i < window.written; ++i)
    {
        const auto p = window.points[i % observationCapacity];
        if (delta(lastPoint.qpc, p.qpc) <= 10.0 * static_cast<double>(frequency)) points.push_back(p);
    }
    r.observations = points.size();
    if (points.empty()) return r;
    r.originQpc = points.front().qpc; r.originSamplePosition = points.front().sample;
    double meanX = 0, meanY = 0, xx = 0, xy = 0; std::size_t n = 0;
    for (const auto p : points)
    {
        const double x = delta(p.qpc, r.originQpc), y = delta(p.sample, r.originSamplePosition);
        const double dx = x - meanX, dy = y - meanY;
        meanX += dx / static_cast<double>(++n); meanY += dy / static_cast<double>(n);
        xx += dx * (x - meanX); xy += dx * (y - meanY);
    }
    if (points.size() < 3 || xx <= 0) return r;
    r.samplesPerQpcTick = xy / xx; r.samplesPerSecond = r.samplesPerQpcTick * static_cast<double>(frequency);
    r.interceptAtOrigin = meanY - r.samplesPerQpcTick * meanX;
    double squares = 0; std::vector<double> residuals; residuals.reserve(points.size());
    for (const auto p : points)
    {
        const double residual = delta(p.sample, r.originSamplePosition) - (r.samplesPerQpcTick * delta(p.qpc, r.originQpc) + r.interceptAtOrigin);
        squares += residual * residual; residuals.push_back(std::abs(residual));
    }
    std::sort(residuals.begin(), residuals.end());
    r.residualRmsSamples = std::sqrt(squares / static_cast<double>(points.size())); r.residualMaxAbsSamples = residuals.back();
    r.residualP95AbsSamples = residuals[static_cast<std::size_t>(std::ceil(0.95 * residuals.size())) - 1];
    r.residualP99AbsSamples = residuals[static_cast<std::size_t>(std::ceil(0.99 * residuals.size())) - 1];
    r.valid = std::isfinite(r.samplesPerSecond) && r.samplesPerSecond > 0;
    return r;
}
AsioRegression AsioStampStatistics::regression() const { return fit(callbackWindow); }
AsioRegression AsioStampStatistics::pollRegression() const { return fit(pollWindow); }
AsioTimingBridge::AsioTimingBridge(std::int64_t hz, RawAudioTap* tap) : raw(tap), stats(hz) {}
AsioTimingBridge::~AsioTimingBridge() { unregisterAfterDeviceClosed(); }
bool AsioTimingBridge::registerTap() noexcept
{
    if (registered || recorderAsioTap.load(std::memory_order_acquire) != nullptr) return false;
    owner = this; registered = true; recorderAsioTap.store(&dispatch, std::memory_order_release); return true;
}
void AsioTimingBridge::unregisterAfterDeviceClosed() noexcept
{
    if (!registered) return;
    recorderAsioTap.store(nullptr, std::memory_order_release); owner = nullptr; registered = false;
}
void AsioTimingBridge::dispatch(const BlockStamp& stamp, const NativeInputView* views, std::uint32_t count) noexcept
{
    owner->onAsioBlock(stamp, views, count);
}
void AsioTimingBridge::onAsioBlock(const BlockStamp& stamp, const NativeInputView* views, std::uint32_t count) noexcept
{
    if (raw) raw->onAsioBlock(stamp, views, count);
    enqueueStamp(stamp);
}
bool AsioTimingBridge::enqueueStamp(const BlockStamp& stamp) noexcept
{
    auto* slot = stamps.reserve();
    if (!slot) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
    *slot = stamp;
    const auto size = stamps.producerSize() + 1;
    if (size > peak.load(std::memory_order_relaxed)) peak.store(size, std::memory_order_relaxed);
    stamps.commit();
    return true;
}
bool AsioTimingBridge::enqueuePoll(const SamplePositionPoll& p) noexcept
{
    if (polls.push(p)) return true;
    pollsDropped.fetch_add(1, std::memory_order_relaxed); return false;
}
void AsioTimingBridge::drain(StampConsumer consume, void* context) noexcept
{
    BlockStamp stamp{}; while (stamps.pop(stamp)) { stats.observe(stamp); if (consume) consume(context, stamp); }
    SamplePositionPoll p{}; while (polls.pop(p)) stats.observePoll(p);
}
bool AsioTimingBridge::hookCompiled() noexcept { return RECORDER_ASIO_TAP_AVAILABLE != 0; }
bool AsioTimingBridge::poll(juce::AudioIODevice& device, SamplePositionPoll& p) noexcept
{
#if RECORDER_ASIO_TAP_AVAILABLE
    return pollAsioSamplePosition(device, p);
#else
    (void) device; p = {}; return false;
#endif
}
bool AsioTimingBridge::readEvents(const juce::AudioIODevice& device, AsioEventCounters& events) noexcept
{
#if RECORDER_ASIO_TAP_AVAILABLE
    return readAsioEventCounters(device, events);
#else
    (void) device; events = {}; return false;
#endif
}
}
