#include "ClockMapper.h"
#include <stdexcept>

namespace gocue::recorder
{
using namespace clock_math;
namespace
{
ClockSource sourceFor(const BlockStamp& s) noexcept
{
    const auto required = timeInfoPresent | samplePositionValid | systemTimeValid;
    return (s.flags & required) == required ? ClockSource::asioPositionCallbackQpc : ClockSource::accumulatedCallbackQpc;
}
bool withinEpoch(const ClockSnapshot& s, std::int64_t qpc) noexcept
{
    return difference(qpc, s.epochStartQpc) >= -static_cast<double>(s.extrapolationTicks)
        && difference(qpc, s.observedThroughQpc) <= static_cast<double>(s.extrapolationTicks);
}
}
const char* clockSourceName(ClockSource s) noexcept
{
    switch (s) { case ClockSource::asioPositionCallbackQpc: return "asio-position/callback-qpc";
        case ClockSource::accumulatedCallbackQpc: return "accumulated-samples/callback-qpc";
        default: return "unavailable"; }
}
const char* clockGradeName(ClockGrade g) noexcept
{
    switch (g) { case ClockGrade::warmingUp: return "warming-up"; case ClockGrade::asioObserved: return "asio-observed";
        case ClockGrade::callbackEstimated: return "callback-estimated-low"; default: return "unavailable"; }
}
std::optional<std::int64_t> ClockSnapshot::mapToSample(std::int64_t qpc) const noexcept
{
    if (!valid || !withinEpoch(*this, qpc)) return {};
    return roundedOffset(sampleOrigin, samplesPerQpcTick * difference(qpc, qpcOrigin) + sampleOffsetAtOrigin);
}
std::optional<std::int64_t> ClockSnapshot::mapToQpc(std::int64_t sample) const noexcept
{
    if (!valid || samplesPerQpcTick <= 0) return {};
    const auto q = roundedOffset(qpcOrigin, (difference(sample, sampleOrigin) - sampleOffsetAtOrigin) / samplesPerQpcTick);
    return q && withinEpoch(*this, *q) ? q : std::nullopt;
}
ClockMapper::ClockMapper(std::int64_t frequency, ClockMapperConfig c) : config(c), fixedFrequency(frequency)
{
    const double parameters[]{c.windowSeconds, c.refitSeconds, c.minimumFitSeconds, c.slopeTimeConstantSeconds,
        c.slopeSlewPpmPerSecond, c.phaseTimeConstantSeconds, c.phaseSlewSecondsPerSecond, c.timeJumpSeconds, c.maximumExtrapolationSeconds};
    for (const auto p : parameters) if (!std::isfinite(p) || p <= 0) throw std::invalid_argument("ClockMapper: invalid configuration");
    if (frequency <= 0 || frequency > 1000000000000LL || c.windowSeconds < 5 || c.windowSeconds > 10
        || c.refitSeconds > c.windowSeconds || c.minimumFitSeconds > c.windowSeconds || c.maximumExtrapolationSeconds > 10)
        throw std::invalid_argument("ClockMapper: frequency/window out of range");
    state.qpcFrequency = frequency;
    state.extrapolationTicks = static_cast<std::int64_t>(c.maximumExtrapolationSeconds * static_cast<double>(frequency));
    window.reserve(2048); events.reserve(128); published.publish(state);
}
void ClockMapper::beginEpoch(ClockEpochReason reason, const BlockStamp& stamp)
{
    const auto epoch = state.epoch + 1;
    const auto frequency = state.qpcFrequency, extrapolation = state.extrapolationTicks;
    state = {}; state.epoch = epoch; state.qpcFrequency = frequency; state.extrapolationTicks = extrapolation;
    state.reason = reason; state.epochStartQpc = stamp.callbackQpc; state.observedThroughQpc = stamp.callbackQpc;
    window.clear(); lastFitQpc = 0; accumulated = 0; havePrevious = false;
    if (events.size() == 128) { events.erase(events.begin()); ++omittedEvents; }
    events.push_back({epoch, stamp.sequence, stamp.callbackQpc, stamp.samplePosition, reason});
    published.publish(state); // invalidate old mapping immediately; never smooth across epochs
}
void ClockMapper::reset(ClockEpochReason reason)
{
    beginEpoch(reason, previous);
}
bool ClockMapper::observe(const BlockStamp& s)
{
    ++blocks;
    if (s.callbackQpc <= 0 || !s.numSamples || s.numSamples > 262144 || s.bufferIndex < 0 || s.bufferIndex > 1
        || !std::isfinite(s.sampleRate) || s.sampleRate < 8000 || s.sampleRate > 768000)
    {
        ++invalid; beginEpoch(ClockEpochReason::invalidStamp, s); return false;
    }
    auto reason = state.epoch == 0 ? ClockEpochReason::initial : ClockEpochReason::none;
    const auto source = sourceFor(s);
    if (havePrevious)
    {
        if (s.resets != previous.resets) reason = reason | ClockEpochReason::asioReset;
        if (s.resyncs != previous.resyncs) reason = reason | ClockEpochReason::resync;
        if (s.xruns != previous.xruns) reason = reason | ClockEpochReason::xrun;
        if (s.latencyChanges != previous.latencyChanges) reason = reason | ClockEpochReason::latencyChange;
        if (s.sampleRate != previous.sampleRate) reason = reason | ClockEpochReason::sampleRateChange;
        if (s.numSamples != previous.numSamples) reason = reason | ClockEpochReason::bufferChange;
        if (s.sequence != previous.sequence + 1) reason = reason | ClockEpochReason::observationGap;
        if (source != state.quality.source) reason = reason | ClockEpochReason::timingSourceChange;
        const auto dq = difference(s.callbackQpc, previous.callbackQpc) / static_cast<double>(state.qpcFrequency);
        if (dq <= 0) reason = reason | ClockEpochReason::qpcRegression;
        if (std::abs(dq - previous.numSamples / previous.sampleRate) > config.timeJumpSeconds)
            reason = reason | ClockEpochReason::timeJump;
        // Even in fallback, a driver-declared samplePosition discontinuity is
        // visible. Never use sequence gaps to manufacture missing input samples.
        if ((s.flags & samplePositionValid) && (previous.flags & samplePositionValid))
        {
            const auto ds = difference(s.samplePosition, previous.samplePosition);
            if (ds <= 0) reason = reason | ClockEpochReason::sampleRegression;
            else if (s.sequence == previous.sequence + 1 && ds != previous.numSamples) reason = reason | ClockEpochReason::sampleJump;
        }
    }
    if (reason != ClockEpochReason::none) beginEpoch(reason, s);
    if (!havePrevious)
    {
        state.epochStartQpc = s.callbackQpc;
        // Fallback has a declared local origin of zero. A valid position can
        // anchor that origin, but subsequent coordinates use ONLY numSamples.
        accumulated = (s.flags & samplePositionValid) ? s.samplePosition : 0;
    }
    else if (const auto next = add(accumulated, previous.numSamples)) accumulated = *next;
    else { ++invalid; beginEpoch(ClockEpochReason::invalidStamp, s); return false; }
    const auto sample = source == ClockSource::asioPositionCallbackQpc ? s.samplePosition : accumulated;
    state.nominalSampleRate = s.sampleRate; state.quality.source = source; state.observedThroughQpc = s.callbackQpc;
    const double ticks = static_cast<double>(state.qpcFrequency);
    const auto first = std::find_if(window.begin(), window.end(), [&](const auto& p)
        { return difference(s.callbackQpc, p.x) <= config.windowSeconds * ticks; });
    window.erase(window.begin(), first);
    // At least 5 ms between retained pairs => a full 5..10 s window at any ASIO buffer size.
    if (window.empty() || difference(s.callbackQpc, window.back().x) >= ticks * 0.005)
        window.push_back({s.callbackQpc, sample});
    previous = s; havePrevious = true;
    if (lastFitQpc == 0 || difference(s.callbackQpc, lastFitQpc) >= config.refitSeconds * ticks) fit();
    published.publish(state);
    return true;
}
void ClockMapper::fit()
{
    if (window.size() < 4) return;
    const auto span = difference(window.back().x, window.front().x) / static_cast<double>(state.qpcFrequency);
    if (span < config.minimumFitSeconds) return;
    const auto estimate = clock_detail::robustFit(window, std::max(1.0, state.nominalSampleRate * 0.00005));
    if (!estimate.valid) return;
    const auto nominalSlope = state.nominalSampleRate / static_cast<double>(state.qpcFrequency);
    if (std::abs(estimate.slope / nominalSlope - 1) > 0.01)
    {
        state.valid = false; state.quality.grade = ClockGrade::unavailable; lastFitQpc = state.observedThroughQpc; return;
    }
    const auto pivotQpc = window.back().x, pivotSample = window.back().y;
    const auto target = difference(estimate.y0, pivotSample) + estimate.intercept
        + estimate.slope * difference(pivotQpc, estimate.x0);
    // A short startup fit can have thousands of ppm of jitter bias. Hold the
    // nominal slope until five seconds of evidence exists, then slew toward the
    // robust estimate. A bad initial slope must not take minutes to unwind.
    double slope = span < 5 ? nominalSlope : estimate.slope, offset = target;
    if (state.valid)
    {
        const auto dt = difference(state.observedThroughQpc, lastFitQpc) / static_cast<double>(state.qpcFrequency);
        slope = span < 5 ? state.samplesPerQpcTick : clock_detail::slew(state.samplesPerQpcTick, slope, dt,
            config.slopeTimeConstantSeconds, nominalSlope * config.slopeSlewPpmPerSecond * 1e-6);
        const auto oldAtPivot = difference(state.sampleOrigin, pivotSample) + state.sampleOffsetAtOrigin
            + state.samplesPerQpcTick * difference(pivotQpc, state.qpcOrigin);
        offset = clock_detail::slew(oldAtPivot, target, dt, config.phaseTimeConstantSeconds,
            state.nominalSampleRate * config.phaseSlewSecondsPerSecond);
    }
    state.valid = true; state.qpcOrigin = pivotQpc; state.sampleOrigin = pivotSample;
    state.samplesPerQpcTick = slope; state.sampleOffsetAtOrigin = offset;
    auto& q = state.quality;
    q.grade = span < 5 ? ClockGrade::warmingUp : q.source == ClockSource::asioPositionCallbackQpc
        ? ClockGrade::asioObserved : ClockGrade::callbackEstimated;
    q.observations = estimate.inliers; q.outliers = estimate.rejected; q.windowSeconds = span;
    q.ppm = (slope / nominalSlope - 1) * 1e6; q.fittedPpm = (estimate.slope / nominalSlope - 1) * 1e6;
    q.fitResidualRmsSamples = estimate.rms;
    // Published-model RMS includes phase/slope smoothing error on robust inliers.
    double squares = 0; std::size_t count = 0;
    for (const auto p : window)
    {
        const auto fitError = difference(p.y, estimate.y0) - estimate.intercept - estimate.slope * difference(p.x, estimate.x0);
        if (std::abs(fitError) > std::max(state.nominalSampleRate * 0.00005, 6 * estimate.rms)) continue;
        const auto error = difference(p.y, pivotSample) - offset - slope * difference(p.x, pivotQpc);
        squares += error * error; ++count;
    }
    q.residualRmsSamples = count ? std::sqrt(squares / static_cast<double>(count)) : estimate.rms;
    lastFitQpc = state.observedThroughQpc;
}
std::optional<std::int64_t> ClockMapper::mapToSample(std::int64_t qpc) const noexcept
{ const auto s = snapshot(); return s ? s->mapToSample(qpc) : std::nullopt; }
std::optional<std::int64_t> ClockMapper::mapToQpc(std::int64_t sample) const noexcept
{ const auto s = snapshot(); return s ? s->mapToQpc(sample) : std::nullopt; }
std::optional<ClockQuality> ClockMapper::quality() const noexcept
{ const auto s = snapshot(); return s ? std::optional<ClockQuality>(s->quality) : std::nullopt; }
std::optional<std::int64_t> outputOriginSample(std::int64_t p, std::int64_t reported, const OutputBufferStamp& buffer, std::int64_t residual) noexcept
{
    const auto offset = subtract(p, buffer.projectSampleAtBufferStart), latency = add(reported, residual);
    if (!buffer.epoch || !buffer.numSamples || !offset || *offset < 0 || *offset >= buffer.numSamples
        || reported < 0 || !latency || *latency < 0) return {};
    const auto reference = add(buffer.masterSampleAtBufferStart, *offset);
    return reference ? add(*reference, *latency) : std::nullopt;
}
std::optional<std::int64_t> projectVideoSample(std::int64_t p, std::int64_t nv, std::int64_t o) noexcept
{ const auto delta = subtract(nv, o); return delta ? add(p, *delta) : std::nullopt; }
std::optional<std::int64_t> projectInputSample(std::int64_t p, std::int64_t input, std::int64_t o) noexcept
{ return projectVideoSample(p, input, o); }
}
