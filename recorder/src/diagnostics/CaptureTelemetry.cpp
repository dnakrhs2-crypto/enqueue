#include "CaptureTelemetry.h"
#include "model/SafeFileWrite.h"
#include <algorithm>
#include <cmath>

namespace gocue::recorder
{
namespace
{
constexpr const char* timingNames[] = {"callbackInterval", "queueWait", "sampleCopy", "cpuDecode", "colourNormalise",
    "workerTotal", "uploadSubmit", "mailboxWait", "callbackToPresent", "deviceToCallback", "deviceToPresent", "presentCall", "presentInterval"};
constexpr const char* lossNames[] = {"captureDecodeOverflow", "lateQueueDiscard", "timestampRegression", "sourceCadenceGap",
    "sourceStreamTick", "sourceDiscontinuity", "startupDiscontinuity", "sourceError", "sourceTypeChanged", "endOfStream", "decoderError",
    "previewMailboxOverwrite", "surfacePoolExhausted", "uploadBusy", "presentBusy", "presentFailure", "previewStall", "shutdownDiscard"};
static_assert(std::size(timingNames) == static_cast<size_t>(Timing::count));
static_assert(std::size(lossNames) == static_cast<size_t>(LossReason::count));
juce::var stampJson(const FrameStamp& stamp)
{
    auto value = jsonObject();
    jsonSet(value, "frame", jsonInt(stamp.frame)); jsonSet(value, "generation", jsonInt(stamp.generation));
    jsonSet(value, "pts100ns", juce::var(static_cast<juce::int64>(stamp.pts100ns)));
    jsonSet(value, "deviceTimestamp100ns", stamp.hasDeviceTimestamp ? jsonInt(stamp.deviceTimestamp100ns) : juce::var());
    jsonSet(value, "deviceTimestampValidForLatency", stamp.deviceTimestampValid);
    auto times = jsonObject();
    const std::pair<const char*, std::int64_t> items[] = {{"callback", stamp.callback}, {"enqueued", stamp.enqueued},
        {"worker", stamp.worker}, {"decodeStart", stamp.decodeStart}, {"decodeEnd", stamp.decodeEnd},
        {"normaliseEnd", stamp.normaliseEnd}, {"uploadStart", stamp.uploadStart}, {"uploadEnd", stamp.uploadEnd}, {"presentSubmit", stamp.presentSubmit}};
    for (const auto& item : items) jsonSet(times, item.first, item.second ? juce::var(static_cast<juce::int64>(item.second)) : juce::var());
    jsonSet(value, "qpc", times);
    return value;
}
template<size_t N> juce::var traceJson(const std::array<FrameStamp, N>& trace, std::uint64_t count)
{
    juce::Array<juce::var> result;
    const auto retained = std::min<std::uint64_t>(count, N);
    const auto start = count > N ? count % N : 0;
    for (std::uint64_t i = 0; i < retained; ++i) result.add(stampJson(trace[(start + i) % N]));
    return result;
}
}
const char* lossName(LossReason reason) noexcept { return lossNames[static_cast<size_t>(reason)]; }
bool CaptureTelemetry::softwareLossFree() const noexcept
{
    for (const auto reason : {LossReason::captureDecodeOverflow, LossReason::lateQueueDiscard, LossReason::decoderError,
        LossReason::surfacePoolExhausted, LossReason::uploadBusy, LossReason::presentFailure})
        if (count(reason)) return false;
    return true;
}
LateReason classifyLate(std::int64_t previous, std::int64_t pts, bool hasPrevious, double waitMs, Rational fps) noexcept
{
    if (hasPrevious && pts <= previous) return LateReason::timestampRegression;
    if (waitMs > 2.0) return LateReason::queueWait;
    if (hasPrevious && static_cast<double>(pts - previous) / 10000.0 > fps.periodMs() * 1.5) return LateReason::cadenceGap;
    return LateReason::onTime;
}
void Distribution::add(double value) noexcept
{
    if (!std::isfinite(value) || value < 0) return;
    const auto index = value > 2000.0 ? buckets.size() - 1 : static_cast<size_t>(std::ceil(value * 10.0));
    ++buckets[index]; ++samples; sum += value; maximum = std::max(maximum, value);
}
double Distribution::percentile(double fraction) const noexcept
{
    if (!samples) return 0;
    const auto rank = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(samples))));
    std::uint64_t cumulative = 0;
    for (size_t i = 0; i < buckets.size(); ++i)
    {
        cumulative += buckets[i];
        if (cumulative >= rank) return i == buckets.size() - 1 ? maximum : std::min(maximum, static_cast<double>(i) / 10.0);
    }
    return maximum;
}
juce::var Distribution::toJson() const
{
    auto value = jsonObject(); jsonSet(value, "count", jsonInt(samples));
    jsonSet(value, "p50Ms", samples ? juce::var(percentile(0.50)) : juce::var());
    jsonSet(value, "p95Ms", samples ? juce::var(percentile(0.95)) : juce::var());
    jsonSet(value, "p99Ms", samples ? juce::var(percentile(0.99)) : juce::var());
    jsonSet(value, "maxMs", samples ? juce::var(maximum) : juce::var());
    jsonSet(value, "meanMs", samples ? juce::var(sum / static_cast<double>(samples)) : juce::var());
    return value;
}
void CaptureTelemetry::recordWorker(const FrameStamp& stamp)
{
    duration(Timing::queueWait, ms(stamp.worker - stamp.enqueued));
    duration(Timing::sampleCopy, ms(stamp.decodeStart - stamp.worker));
    duration(Timing::cpuDecode, ms(stamp.decodeEnd - stamp.decodeStart));
    duration(Timing::colourNormalise, ms(stamp.normaliseEnd - stamp.decodeEnd));
    duration(Timing::workerTotal, ms(stamp.normaliseEnd - stamp.worker));
    workerTrace[workerTraceCount++ % workerTrace.size()] = stamp;
}
void CaptureTelemetry::recordPresent(const FrameStamp& stamp, std::int64_t returned)
{
    duration(Timing::uploadSubmit, ms(stamp.uploadEnd - stamp.uploadStart));
    duration(Timing::mailboxWait, ms(stamp.uploadStart - stamp.normaliseEnd));
    duration(Timing::callbackToPresent, ms(stamp.presentSubmit - stamp.callback));
    duration(Timing::presentCall, ms(returned - stamp.presentSubmit));
    if (stamp.hasDeviceTimestamp && stamp.deviceTimestampValid)
        duration(Timing::deviceToPresent, ms(stamp.presentSubmit) - static_cast<double>(stamp.deviceTimestamp100ns) / 10000.0);
    presentTrace[presentTraceCount++ % presentTrace.size()] = stamp;
}
juce::var CaptureTelemetry::toJson() const
{
    auto value = jsonObject(), metrics = jsonObject(), counts = jsonObject();
    for (size_t i = 0; i < timings.size(); ++i) jsonSet(metrics, timingNames[i], timings[i].toJson());
    for (size_t i = 0; i < losses.size(); ++i) jsonSet(counts, lossNames[i], jsonInt(losses[i].load()));
    jsonSet(counts, "callbacks", jsonInt(callbacks.load())); jsonSet(counts, "samples", jsonInt(samples.load()));
    jsonSet(counts, "decoded", jsonInt(decoded.load())); jsonSet(counts, "presentedUnique", jsonInt(presented.load()));
    jsonSet(counts, "repeatedPresents", jsonInt(repeatedPresents.load())); jsonSet(counts, "lateQueueOver2Ms", jsonInt(lateQueue.load()));
    jsonSet(counts, "missingDeviceTimestamp", jsonInt(missingDeviceTimestamp.load()));
    jsonSet(counts, "invalidDeviceTimestamp", jsonInt(invalidDeviceTimestamp.load()));
    jsonSet(counts, "colourAssumptions", jsonInt(colourAssumptions.load()));
    jsonSet(value, "counts", counts); jsonSet(value, "timings", metrics);
    jsonSet(value, "qpcFrequency", juce::var(static_cast<juce::int64>(frequency)));
    jsonSet(value, "captureQueueCapacity", 2); jsonSet(value, "captureQueueHighWater", jsonInt(queueHighWater.load()));
    jsonSet(value, "sourceHresult", hresultText(sourceStatus.load()));
    jsonSet(value, "tracePolicy", "last 128 worker frames and last 128 uniquely presented frames; bounded ring, QPC ticks");
    jsonSet(value, "workerTrace", traceJson(workerTrace, workerTraceCount));
    jsonSet(value, "presentTrace", traceJson(presentTrace, presentTraceCount));
    jsonSet(value, "percentileDefinition", "nearest rank; 0.1ms upper buckets <=2000ms; larger values use observed max; empty=null");
    jsonSet(value, "captureLossCertification", "UNAVAILABLE: no independent source frame-pattern/optical oracle; PTS gaps are observations, not proven lost frame counts");
    jsonSet(value, "gpuCompletionTiming", "UNAVAILABLE: upload and Present timestamps measure CPU submission, not GPU completion/scanout");
    jsonSet(value, "cadenceWarmupSeconds", 1);
    jsonSet(value, "deviceObservations", "sourceCadenceGap (PTS or callback interval >1.5 native periods), stream tick/discontinuity and previewStall are observations; cadence/stall exclude first second from first callback. They do not count as proven software loss.");
    jsonSet(value, "softwareLossFree", softwareLossFree());
    return value;
}
void CaptureTelemetry::writeJson(const juce::File& file, const juce::var& value)
{
    const auto result = gocue::SafeFileWrite::writeTextVerified(file, juce::JSON::toString(value, false), [](const juce::String& text)
    {
        juce::var read;
        return juce::JSON::parse(text, read);
    });
    if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString());
}
}
