#pragma once
#include "AtomicSnapshot.h"
#include "RobustClockFit.h"
#include "audio/AsioTapAbi.h"
#include <array>
#include <vector>

namespace gocue::recorder
{
enum class ClockSource { unavailable, asioPositionCallbackQpc, accumulatedCallbackQpc };
enum class ClockGrade { unavailable, warmingUp, asioObserved, callbackEstimated };
enum class ClockEpochReason : std::uint32_t
{
    none = 0, initial = 1, asioReset = 2, sampleRateChange = 4, bufferChange = 8,
    sampleRegression = 16, sampleJump = 32, qpcRegression = 64, timeJump = 128,
    observationGap = 256, timingSourceChange = 512, resync = 1024, xrun = 2048,
    latencyChange = 4096, invalidStamp = 8192, explicitReset = 16384
};
constexpr ClockEpochReason operator|(ClockEpochReason a, ClockEpochReason b) noexcept
{ return static_cast<ClockEpochReason>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)); }
constexpr bool hasReason(ClockEpochReason value, ClockEpochReason flag) noexcept
{ return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0; }
const char* clockSourceName(ClockSource) noexcept;
const char* clockGradeName(ClockGrade) noexcept;
struct ClockQuality
{
    ClockGrade grade = ClockGrade::unavailable;
    ClockSource source = ClockSource::unavailable;
    std::uint64_t observations = 0, outliers = 0;
    double windowSeconds = 0, residualRmsSamples = 0, fitResidualRmsSamples = 0;
    double ppm = 0, fittedPpm = 0;
};
struct ClockSnapshot
{
    bool valid = false;
    std::uint64_t epoch = 0;
    ClockEpochReason reason = ClockEpochReason::none;
    std::int64_t qpcFrequency = 0, qpcOrigin = 0, sampleOrigin = 0;
    std::int64_t epochStartQpc = 0, observedThroughQpc = 0, extrapolationTicks = 0;
    double samplesPerQpcTick = 0, sampleOffsetAtOrigin = 0, nominalSampleRate = 0;
    ClockQuality quality;
    // S(q) = sampleOrigin + sampleOffsetAtOrigin + a*(q-qpcOrigin).
    // This centred representation preserves int64 detail. No input latency is
    // applied: resolving the driver's buffer reference is an explicit adapter job.
    std::optional<std::int64_t> mapToSample(std::int64_t qpc) const noexcept;
    std::optional<std::int64_t> mapToQpc(std::int64_t sample) const noexcept;
};
struct ClockMapperConfig
{
    double windowSeconds = 8, refitSeconds = 0.25, minimumFitSeconds = 0.25;
    double slopeTimeConstantSeconds = 5, slopeSlewPpmPerSecond = 20;
    double phaseTimeConstantSeconds = 2, phaseSlewSecondsPerSecond = 0.00025;
    double timeJumpSeconds = 0.25, maximumExtrapolationSeconds = 0.5;
};
struct ClockEpochEvent
{
    std::uint64_t epoch = 0, sequence = 0;
    std::int64_t qpc = 0, samplePosition = 0;
    ClockEpochReason reasons = ClockEpochReason::none;
};
class ClockMapper
{
public:
    explicit ClockMapper(std::int64_t qpcFrequency, ClockMapperConfig = {});
    std::int64_t qpcFrequency() const noexcept { return fixedFrequency; }
    // Sole sync worker only; feed via AsioTimingBridge::drain. Regression/scratch
    // allocation never runs on the audio callback. The window is decimated to
    // <=2048 observations while every BlockStamp is checked for discontinuity.
    bool observe(const BlockStamp&);
    void reset(ClockEpochReason = ClockEpochReason::explicitReset);
    std::optional<ClockSnapshot> snapshot() const noexcept { return published.read(); }
    std::optional<std::int64_t> mapToSample(std::int64_t qpc) const noexcept;
    std::optional<std::int64_t> mapToQpc(std::int64_t sample) const noexcept;
    std::optional<ClockQuality> quality() const noexcept;
    // Diagnostic access after worker join (or by that worker).
    const std::vector<ClockEpochEvent>& epochEvents() const noexcept { return events; }
    std::uint64_t omittedEpochEvents() const noexcept { return omittedEvents; }
    std::uint64_t blocksObserved() const noexcept { return blocks; }
    std::uint64_t invalidStamps() const noexcept { return invalid; }
private:
    void beginEpoch(ClockEpochReason, const BlockStamp&);
    void fit();
    ClockMapperConfig config;
    const std::int64_t fixedFrequency;
    ClockSnapshot state;
    AtomicSnapshot<ClockSnapshot> published;
    std::vector<clock_detail::Point> window;
    std::vector<ClockEpochEvent> events;
    BlockStamp previous{};
    bool havePrevious = false;
    std::int64_t accumulated = 0, lastFitQpc = 0;
    std::uint64_t omittedEvents = 0, blocks = 0, invalid = 0;
};

// Explicit output buffer reference. The adapter must establish what the driver
// means by samplePosition before constructing this value. Both coordinates refer
// to the FIRST sample in the SAME submitted buffer; project Pstart is inside it.
struct OutputBufferStamp
{
    std::int64_t masterSampleAtBufferStart = 0, projectSampleAtBufferStart = 0;
    std::uint32_t numSamples = 0;
    std::uint64_t epoch = 0;
};
std::optional<std::int64_t> outputOriginSample(std::int64_t projectStart, std::int64_t reportedOutputLatencySamples,
    const OutputBufferStamp&, std::int64_t residualOutputLatencySamples = 0) noexcept;
std::optional<std::int64_t> projectVideoSample(std::int64_t projectStart, std::int64_t captureSample, std::int64_t outputOrigin) noexcept;
// correctedInputSample already includes input latency. No latency argument exists
// here, so the same [O0,Ostop) input range cannot be shifted a second time.
std::optional<std::int64_t> projectInputSample(std::int64_t projectStart, std::int64_t correctedInputSample, std::int64_t outputOrigin) noexcept;
}
