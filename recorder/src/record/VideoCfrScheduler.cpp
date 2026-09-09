#include "VideoCfrScheduler.h"
#include <algorithm>
#include <limits>

namespace gocue::recorder
{
namespace
{
std::int64_t multiply(std::int64_t a, std::int64_t b)
{
    if (a < 0 || b <= 0 || a > std::numeric_limits<std::int64_t>::max() / b)
        throw std::invalid_argument("CFR time outside checked int64 domain");
    return a * b;
}
void rateCheck(Rational rate)
{
    if (!rate.numerator || !rate.denominator || rate.numerator > 1000000 || rate.denominator > 1000000)
        throw std::invalid_argument("Invalid CFR rational rate");
}
}
MfPtsTimeMapper::MfPtsTimeMapper(std::int64_t f) : frequency(f)
{
    if (f <= 0) throw std::invalid_argument("Invalid mapper QPC frequency");
}
std::int64_t MfPtsTimeMapper::map(const FrameStamp& s)
{
    if (!started) { originPts = s.pts100ns; originQpc = s.callback; started = true; }
    if (s.pts100ns < originPts) throw std::runtime_error("MF PTS epoch regressed");
    return s.pts100ns - originPts;
}
std::int64_t MfPtsTimeMapper::now(std::int64_t qpc) const
{
    if (!started || qpc < originQpc) return 0;
    const auto delta = qpc - originQpc;
    return delta / frequency * 10000000 + (delta % frequency) * 10000000 / frequency;
}
juce::var CfrCounters::toJson() const
{
    auto value = jsonObject();
    jsonSet(value, "inputs", jsonInt(inputs)); jsonSet(value, "outputs", jsonInt(outputs));
    jsonSet(value, "repeated", jsonInt(repeated)); jsonSet(value, "omitted", jsonInt(omitted));
    const char* names[] = {"nativeRateConversion", "clockCorrection", "captureLoss", "encodeLoss"};
    for (size_t i = 0; i < reasons.size(); ++i)
    {
        auto reason = jsonObject();
        jsonSet(reason, "repeated", jsonInt(reasons[i].repeated)); jsonSet(reason, "omitted", jsonInt(reasons[i].omitted));
        jsonSet(reason, "missing", jsonInt(reasons[i].missing)); jsonSet(value, names[i], reason);
    }
    jsonSet(value, "definition", "missing counts known rejected input/output frames; repeated/omitted classify selection changes, not additional losses. Native repeats use the selected camera frame phase, independently of project grid phase. Device cadence alone is not captureLoss.");
    return value;
}
VideoCfrScheduler::VideoCfrScheduler(Rational n, Rational p) : native(n), project(p)
{
    rateCheck(n); rateCheck(p);
    if (p.denominator != 1 || (p.numerator != 30 && p.numerator != 60)) throw std::invalid_argument("Project CFR must be 30/1 or 60/1");
}
std::int64_t VideoCfrScheduler::gridTime(std::int64_t index, Rational rate)
{
    rateCheck(rate);
    return multiply(multiply(index, rate.denominator), 10000000) / rate.numerator;
}
std::int64_t VideoCfrScheduler::frameCount(std::int64_t duration, Rational rate)
{
    rateCheck(rate);
    const auto n = multiply(duration, rate.numerator), d = static_cast<std::int64_t>(rate.denominator) * 10000000;
    return n / d + (n % d != 0);
}
std::int64_t VideoCfrScheduler::nearestNativeIndex(std::int64_t index, Rational n, Rational p)
{
    rateCheck(n); rateCheck(p);
    const auto numerator = multiply(multiply(index, p.denominator), n.numerator);
    const auto denominator = static_cast<std::int64_t>(p.numerator) * n.denominator;
    // Older on an exact half-frame tie.
    return numerator / denominator + (numerator % denominator > denominator / 2);
}
void VideoCfrScheduler::push(CfrInput frame)
{
    if (used == capacity) throw std::overflow_error("CFR candidate capacity exceeded");
    if (frame.slot < 0 || !frame.sourceId || frame.sourceId <= lastInputId || frame.time100ns < 0
        || (lastInputId && frame.time100ns <= lastTime)) throw std::invalid_argument("CFR input ID/time must increase within one epoch");
    frames[used++] = frame; lastInputId = frame.sourceId; lastTime = frame.time100ns; ++stats.inputs;
}
void VideoCfrScheduler::noteLoss(CfrReason reason, std::uint64_t count)
{
    if (reason != CfrReason::captureLoss && reason != CfrReason::encodeLoss) throw std::invalid_argument("Only known software losses may be annotated");
    stats.reasons[static_cast<size_t>(reason)].missing += count;
    if (count) pendingLoss = reason;
}
std::optional<CfrSelection> VideoCfrScheduler::select(std::int64_t now, bool drain)
{
    if (!used) return {};
    const auto grid = gridTime(next, project);
    // ceil(native period) makes the deadline no shorter due to 100 ns rounding.
    const auto wait = (10000000LL * native.denominator + native.numerator - 1) / native.numerator;
    if (!drain && frames[used - 1].time100ns < grid && now < grid + wait) return {};
    size_t best = 0;
    auto distance = [grid](const CfrInput& f) { return f.time100ns > grid ? f.time100ns - grid : grid - f.time100ns; };
    if (frames[0].time100ns > grid + wait) return {};
    for (size_t i = 1; i < used && frames[i].time100ns <= grid + wait; ++i)
        // MF timestamps and grid rescale quantize to 100 ns. Treat <=1 tick
        // distance differences as ties, so native 30 -> 60 does not jitter ties.
        if (distance(frames[i]) + 1 < distance(frames[best])) best = i;
    CfrSelection result;
    result.input = frames[best]; result.pts = next; result.grid100ns = grid;
    result.repeated = selectedId == result.input.sourceId;
    const auto ideal = nearestNativeIndex(next, native, project);
    if (result.repeated)
    {
        // Cameras start at independent phases. A nominal 30 -> 60 repeat may
        // land on either project-grid parity, including after applying Lcam.
        const bool upsample = std::uint64_t(native.numerator) * project.denominator < std::uint64_t(project.numerator) * native.denominator;
        const auto halfNative = (5000000LL * native.denominator + native.numerator - 1) / native.numerator;
        const bool nominalRepeat = upsample && distance(result.input) <= halfNative + 1;
        result.reason = nominalRepeat ? CfrReason::nativeRateConversion : pendingLoss.value_or(CfrReason::clockCorrection);
        ++stats.repeated; ++stats.reasons[static_cast<size_t>(result.reason)].repeated;
    }
    auto nominalOmissions = std::max<std::int64_t>(0, previousIdeal < 0 ? 0 : ideal - previousIdeal - 1);
    for (size_t i = 0; i < best; ++i)
    {
        result.released[result.releasedCount++] = frames[i].slot;
        if (frames[i].sourceId != selectedId)
        {
            const auto reason = nominalOmissions-- > 0 ? CfrReason::nativeRateConversion : pendingLoss.value_or(CfrReason::clockCorrection);
            ++stats.omitted; ++stats.reasons[static_cast<size_t>(reason)].omitted;
        }
    }
    if (!result.repeated) pendingLoss.reset();
    for (size_t i = best; i < used; ++i) frames[i - best] = frames[i];
    used -= best; selectedId = result.input.sourceId; previousIdeal = ideal;
    ++next; ++stats.outputs;
    return result;
}
std::array<int, VideoCfrScheduler::capacity> VideoCfrScheduler::releaseAll(size_t& count) noexcept
{
    std::array<int, capacity> result{}; count = used;
    for (size_t i = 0; i < used; ++i) result[i] = frames[i].slot;
    used = 0; return result;
}
}
