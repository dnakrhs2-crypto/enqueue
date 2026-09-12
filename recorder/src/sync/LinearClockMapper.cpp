#include "IClockMapper.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gocue::recorder
{
namespace
{
std::int64_t checkedRound(double value)
{
    if (!std::isfinite(value) || value <= double((std::numeric_limits<std::int64_t>::min)())
        || value >= double((std::numeric_limits<std::int64_t>::max)())) throw std::overflow_error("Clock mapping outside int64");
    return std::llround(value);
}
std::int64_t add(std::int64_t a, std::int64_t b)
{
    if ((b > 0 && a > (std::numeric_limits<std::int64_t>::max)() - b)
        || (b < 0 && a < (std::numeric_limits<std::int64_t>::min)() - b)) throw std::overflow_error("Clock origin overflow");
    return a + b;
}
double difference(std::int64_t a, std::int64_t b)
{ return (a < 0) == (b < 0) ? double(a - b) : double(a) - double(b); }
}
std::int64_t ClockMapping::mapToSample(std::int64_t qpc) const
{
    if (!valid || samplesPerTick <= 0) throw std::logic_error("ASIO/QPC mapper is not ready");
    return add(originSample, checkedRound(difference(qpc, originQpc) * samplesPerTick + intercept));
}
std::int64_t ClockMapping::mapToQpc(std::int64_t sample) const
{
    if (!valid || samplesPerTick <= 0) throw std::logic_error("ASIO/QPC mapper is not ready");
    return add(originQpc, checkedRound((difference(sample, originSample) - intercept) / samplesPerTick));
}
void LinearClockMapper::observe(const AsioStampStatistics& statistics)
{
    const auto r = statistics.regression();
    ClockMapping next{r.valid, r.originQpc, r.originSamplePosition, r.samplesPerQpcTick, r.interceptAtOrigin, r.residualRmsSamples};
    std::lock_guard<std::mutex> lock(mutex); current = next;
}
ClockMapping LinearClockMapper::snapshot() const { std::lock_guard<std::mutex> lock(mutex); return current; }
}
