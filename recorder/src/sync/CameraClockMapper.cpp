#include "CameraClockMapper.h"
#include <stdexcept>
#include <thread>

namespace gocue::recorder
{
using namespace clock_math;
namespace
{
std::optional<std::int64_t> deviceQpc(const FrameStamp& s, std::int64_t frequency) noexcept
{
    // https://learn.microsoft.com/en-us/windows/win32/medfound/mfsampleextension-devicetimestamp
    // MFTIME / 100 ns, QPC epoch. It is NOT a raw QPC tick count.
    if (!s.hasDeviceTimestamp || !s.deviceTimestampValid || !s.deviceTimestamp100ns
        || s.deviceTimestamp100ns > static_cast<std::uint64_t>(INT64_MAX)) return {};
    const auto q = rescale(static_cast<std::int64_t>(s.deviceTimestamp100ns), static_cast<std::uint64_t>(frequency),
        10000000, Rounding::nearest);
    // FrameStamp validity is supplied by capture; reject wrong-epoch/future data
    // as well. The two-second plausibility bound is not an exposure guarantee.
    if (!q || *q <= 0 || difference(s.callback, *q) < -static_cast<double>(frequency) / 10000000
        || difference(s.callback, *q) > 2.0 * static_cast<double>(frequency)) return {};
    return q;
}
}
const char* cameraClockSourceName(CameraClockSource s) noexcept
{
    switch (s) { case CameraClockSource::deviceTimestamp: return "device-timestamp/100ns-qpc-epoch";
        case CameraClockSource::ptsArrivalEstimated: return "pts-arrival-estimated-low"; default: return "unavailable"; }
}
const char* cameraEpochReasonName(CameraEpochReason r) noexcept
{
    switch (r) { case CameraEpochReason::initial: return "initial"; case CameraEpochReason::generationChange: return "generation-change";
        case CameraEpochReason::ptsRegression: return "pts-regression"; case CameraEpochReason::qpcRegression: return "qpc-regression";
        case CameraEpochReason::timestampJump: return "timestamp-jump"; case CameraEpochReason::timingSourceChange: return "timing-source-change";
        case CameraEpochReason::explicitDiscontinuity: return "explicit-discontinuity"; case CameraEpochReason::calibrationChange: return "calibration-change";
        case CameraEpochReason::invalidStamp: return "invalid-stamp"; default: return "none"; }
}
std::optional<std::int64_t> CameraClockSnapshot::timestampQpc(const FrameStamp& s) const noexcept
{
    if (!valid || s.generation != generation || s.pts100ns < firstPts100ns || s.pts100ns > lastPts100ns) return {};
    if (quality.source == CameraClockSource::deviceTimestamp) return deviceQpc(s, qpcFrequency);
    if (deviceQpc(s, qpcFrequency)) return {}; // never silently reuse the other clock path
    return roundedOffset(qpcOrigin, qpcOffsetAtOrigin + qpcTicksPer100ns * difference(s.pts100ns, ptsOrigin100ns));
}
CameraClockMapper::CameraClockMapper(const ClockMapper& m, std::int64_t frequency, Rational native, std::int64_t latency)
    : master(m), fps(native)
{
    if (frequency <= 0 || frequency > 1000000000000LL || !native.numerator || !native.denominator
        || native.numerator > 1000000 || native.denominator > 1000000 || std::abs(static_cast<double>(latency)) > 100000000)
        throw std::invalid_argument("CameraClockMapper: invalid clock, rate or residual latency");
    if (master.qpcFrequency() != frequency) throw std::invalid_argument("Camera and master QPC frequencies differ");
    state.qpcFrequency = frequency; state.cameraResidualLatency100ns = latency;
    window.reserve(2048); directDelays.reserve(2048); published.publish(state);
}
void CameraClockMapper::beginEpoch(CameraEpochReason r, const FrameStamp& s)
{
    const auto epoch = state.epoch + 1;
    const auto frequency = state.qpcFrequency, latency = state.cameraResidualLatency100ns;
    state = {}; state.epoch = epoch; state.generation = s.generation; state.reason = r;
    state.qpcFrequency = frequency; state.cameraResidualLatency100ns = latency;
    state.firstPts100ns = s.pts100ns; state.lastPts100ns = s.pts100ns;
    window.clear(); directDelays.clear(); havePrevious = false; lastFitQpc = 0;
    published.publish(state);
}
void CameraClockMapper::reset(CameraEpochReason reason) { beginEpoch(reason, previous); }
void CameraClockMapper::setResidualLatency(std::int64_t latency)
{
    if (std::abs(static_cast<double>(latency)) > 100000000) throw std::invalid_argument("Camera residual latency exceeds 10 seconds");
    if (latency == state.cameraResidualLatency100ns) return;
    state.cameraResidualLatency100ns = latency; reset(CameraEpochReason::calibrationChange);
}
bool CameraClockMapper::observe(const FrameStamp& s)
{
    if (s.callback <= 0 || !s.frame) { beginEpoch(CameraEpochReason::invalidStamp, s); return false; }
    const auto direct = deviceQpc(s, state.qpcFrequency);
    const auto source = direct ? CameraClockSource::deviceTimestamp : CameraClockSource::ptsArrivalEstimated;
    auto reason = state.epoch ? CameraEpochReason::none : CameraEpochReason::initial;
    if (havePrevious)
    {
        // An adapter may see a frame already used for clock warmup.
        if (s.frame == previous.frame && s.generation == previous.generation && s.callback == previous.callback
            && s.pts100ns == previous.pts100ns && s.deviceTimestamp100ns == previous.deviceTimestamp100ns
            && source == state.quality.source) return true;
        const auto dp = difference(s.pts100ns, previous.pts100ns) / 10000000.0;
        const auto dq = difference(s.callback, previous.callback) / static_cast<double>(state.qpcFrequency);
        if (s.generation != previous.generation) reason = CameraEpochReason::generationChange;
        else if (s.pts100ns <= previous.pts100ns || s.frame <= previous.frame) reason = CameraEpochReason::ptsRegression;
        else if (s.callback <= previous.callback) reason = CameraEpochReason::qpcRegression;
        else if (std::abs(dp - dq) > 0.25 || dq > 8) reason = CameraEpochReason::timestampJump;
        else if (source != state.quality.source) reason = CameraEpochReason::timingSourceChange;
        else if (direct)
        {
            const auto before = deviceQpc(previous, state.qpcFrequency);
            if (!before || *direct <= *before || std::abs(difference(*direct, *before) / static_cast<double>(state.qpcFrequency) - dp) > 0.25)
                reason = CameraEpochReason::timestampJump;
        }
    }
    if (reason != CameraEpochReason::none) beginEpoch(reason, s);
    if (!havePrevious) { state.firstPts100ns = s.pts100ns; state.generation = s.generation; }
    state.lastPts100ns = s.pts100ns; state.quality.source = source;
    while (!window.empty() && (difference(s.pts100ns, window.front().x) > 80000000 || window.size() >= 2048))
    { window.erase(window.begin()); directDelays.erase(directDelays.begin()); }
    // Normal UVC <=240 fps fits an eight-second window. Higher frame rates are
    // decimated at 5ms; discontinuity checks still inspect every stamp.
    if (window.empty() || difference(s.pts100ns, window.back().x) >= 50000)
    {
        window.push_back({s.pts100ns, s.callback});
        directDelays.push_back(direct ? difference(s.callback, *direct) * 1000 / static_cast<double>(state.qpcFrequency) : 0);
    }
    if (direct)
    {
        state.valid = true; state.ptsOrigin100ns = s.pts100ns; state.qpcOrigin = *direct;
        state.qpcOffsetAtOrigin = 0; state.qpcTicksPer100ns = static_cast<double>(state.qpcFrequency) / 10000000;
        state.quality.warmingUp = false;
    }
    previous = s; havePrevious = true;
    if (!lastFitQpc || difference(s.callback, lastFitQpc) >= static_cast<double>(state.qpcFrequency) * 0.25) fit();
    published.publish(state); return true;
}
void CameraClockMapper::fit()
{
    auto& q = state.quality;
    q.windowSeconds = difference(window.back().x, window.front().x) / 10000000;
    q.observations = window.size();
    if (q.source == CameraClockSource::deviceTimestamp)
    {
        q.arrivalDelayP10Ms = clock_detail::quantile(directDelays, 0.1);
        q.arrivalDelayP50Ms = clock_detail::quantile(directDelays, 0.5);
        q.arrivalDelayP95Ms = clock_detail::quantile(directDelays, 0.95);
        lastFitQpc = previous.callback; return;
    }
    if (window.size() < 4 || q.windowSeconds < 0.25) return;
    const auto estimate = clock_detail::robustFit(window, static_cast<double>(state.qpcFrequency) * 0.0002);
    const auto nominal = static_cast<double>(state.qpcFrequency) / 10000000;
    if (!estimate.valid || std::abs(estimate.slope / nominal - 1) > 0.01)
    { state.valid = false; lastFitQpc = previous.callback; return; }
    std::vector<double> offsets;
    for (const auto p : window) offsets.push_back(difference(p.y, estimate.y0) - estimate.slope * difference(p.x, estimate.x0));
    // Lower arrival envelope removes variable queuing from the PTS clock. The
    // unknown constant pipeline/exposure offset remains for measured Lcam.
    const auto intercept = clock_detail::quantile(offsets, 0.1);
    const auto pivot = window.back();
    const auto target = difference(estimate.y0, pivot.y) + intercept + estimate.slope * difference(pivot.x, estimate.x0);
    double slope = q.windowSeconds < 5 ? nominal : estimate.slope, offset = target;
    if (state.valid)
    {
        const auto dt = difference(previous.callback, lastFitQpc) / static_cast<double>(state.qpcFrequency);
        slope = q.windowSeconds < 5 ? state.qpcTicksPer100ns
            : clock_detail::slew(state.qpcTicksPer100ns, slope, dt, 5, nominal * 20e-6);
        const auto oldAtPivot = difference(state.qpcOrigin, pivot.y) + state.qpcOffsetAtOrigin
            + state.qpcTicksPer100ns * difference(pivot.x, state.ptsOrigin100ns);
        offset = clock_detail::slew(oldAtPivot, target, dt, 2, static_cast<double>(state.qpcFrequency) * 0.00025);
    }
    state.valid = true; state.ptsOrigin100ns = pivot.x; state.qpcOrigin = pivot.y;
    state.qpcTicksPer100ns = slope; state.qpcOffsetAtOrigin = offset;
    q.warmingUp = q.windowSeconds < 5; q.observations = estimate.inliers; q.outliers = estimate.rejected;
    q.ppm = (slope / nominal - 1) * 1e6; q.residualRmsMs = estimate.rms * 1000 / static_cast<double>(state.qpcFrequency);
    for (auto& value : offsets) value = (value - intercept) * 1000 / static_cast<double>(state.qpcFrequency);
    q.arrivalDelayP10Ms = clock_detail::quantile(offsets, 0.1);
    q.arrivalDelayP50Ms = clock_detail::quantile(offsets, 0.5);
    q.arrivalDelayP95Ms = clock_detail::quantile(offsets, 0.95);
    lastFitQpc = previous.callback;
}
std::optional<CaptureSample> CameraClockMapper::captureSample(const FrameStamp& frame) const noexcept
{
    const auto audio = master.snapshot(); return audio ? captureSample(frame, *audio) : std::nullopt;
}
std::optional<CaptureSample> CameraClockMapper::captureSample(const FrameStamp& frame, const ClockSnapshot& audio) const noexcept
{
    const auto camera = snapshot();
    if (!camera || camera->qpcFrequency != audio.qpcFrequency) return {};
    const auto qpc = camera->timestampQpc(frame);
    const auto latency = rescale(camera->cameraResidualLatency100ns, static_cast<std::uint64_t>(camera->qpcFrequency), 10000000, Rounding::nearest);
    if (!qpc || !latency) return {};
    const auto capture = subtract(*qpc, *latency);
    const auto sample = capture ? audio.mapToSample(*capture) : std::nullopt;
    if (!sample) return {};
    return CaptureSample{*sample, *capture, audio.epoch, camera->epoch, audio.quality.grade, camera->quality.source};
}
CameraSampleTimeMapper::CameraSampleTimeMapper(CameraClockMapper& mapper, std::int64_t n0, std::uint32_t fs,
    std::uint64_t a, std::uint64_t v) : camera(mapper), origin(n0), rate(fs), audioEpoch(a), videoEpoch(v)
{
    if (!fs || !a || !v) throw std::invalid_argument("CFR adapter requires rate and reserved epochs");
}
std::int64_t CameraSampleTimeMapper::map(const FrameStamp& stamp)
{
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        const auto audio = camera.masterClock().snapshot();
        const auto video = camera.snapshot();
        if ((audio && (audio->epoch != audioEpoch || audio->nominalSampleRate != rate)) || (video && video->epoch != videoEpoch))
            throw std::runtime_error("CFR clock epoch/rate changed; stop take/mark camera gap");
        const auto sample = audio ? camera.captureSample(stamp, *audio) : std::nullopt;
        if (sample)
        {
            if (sample->masterEpoch != audioEpoch || sample->cameraEpoch != videoEpoch) throw std::runtime_error("CFR clock epoch changed");
            const auto delta = subtract(sample->sample, origin);
            const auto time = delta ? sampleToTime100ns(*delta, rate) : std::nullopt;
            if (!time) throw std::overflow_error("CFR sample time overflow");
            return *time;
        }
        std::this_thread::yield(); // worker only; retry an overlapping snapshot publication, never reuse an old epoch
    }
    throw std::runtime_error("CFR clock unavailable; mark camera gap");
}
std::int64_t CameraSampleTimeMapper::now(std::int64_t qpc) const
{
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        const auto audio = camera.masterClock().snapshot(); const auto video = camera.snapshot();
        if ((audio && (audio->epoch != audioEpoch || audio->nominalSampleRate != rate)) || (video && video->epoch != videoEpoch))
            throw std::runtime_error("CFR clock epoch changed");
        if (audio && video)
        {
            const auto sample = audio->mapToSample(qpc);
            const auto delta = sample ? subtract(*sample, origin) : std::nullopt;
            const auto time = delta ? sampleToTime100ns(*delta, rate) : std::nullopt;
            if (time) return *time;
        }
        std::this_thread::yield();
    }
    throw std::runtime_error("CFR deadline clock unavailable");
}
AnchoredCameraTimeMapper::AnchoredCameraTimeMapper(ClockSnapshot audio, CameraClockSnapshot video,
    std::int64_t n0, std::uint32_t fs) : master(audio), preparedCamera(video), origin(n0), rate(fs)
{
    const auto q = master.mapToQpc(origin);
    if (!rate || !master.valid || !preparedCamera.valid || master.nominalSampleRate != rate
        || master.qpcFrequency != preparedCamera.qpcFrequency || !q)
        throw std::invalid_argument("Dubbing anchor requires prepared clocks and a reachable O0");
    originQpc = *q;
}
std::int64_t AnchoredCameraTimeMapper::map(const FrameStamp& stamp)
{
    if (!stamp.frame || stamp.callback <= 0 || stamp.generation != preparedCamera.generation)
        throw std::runtime_error("Dubbing camera generation/stamp changed; mark camera gap");
    if (anchored)
    {
        // LiveTake keeps and remaps the newest negative preroll while waiting
        // for O0. Re-reading that exact stamp is not a capture discontinuity.
        const bool same = stamp.frame == previous.frame && stamp.pts100ns == previous.pts100ns && stamp.callback == previous.callback;
        const auto dp = difference(stamp.pts100ns, previous.pts100ns);
        const auto dq = difference(stamp.callback, previous.callback) * 10000000 / double(master.qpcFrequency);
        const auto device = deviceQpc(stamp, master.qpcFrequency), priorDevice = deviceQpc(previous, master.qpcFrequency);
        const bool deviceJump = device && priorDevice && (*device <= *priorDevice
            || std::abs(difference(*device, *priorDevice) * 10000000 / double(master.qpcFrequency) - dp) > 2500000);
        if (!same && (stamp.frame <= previous.frame || dp <= 0 || dq <= 0 || dq > 80000000
            || std::abs(dp - dq) > 2500000 || deviceJump))
            throw std::runtime_error("Dubbing camera timestamp discontinuity; mark camera gap");
    }
    else
    {
        auto q = deviceQpc(stamp, master.qpcFrequency);
        if (!q && preparedCamera.quality.source == CameraClockSource::ptsArrivalEstimated
            && std::abs(difference(stamp.pts100ns, preparedCamera.lastPts100ns)) <= 5000000)
            q = roundedOffset(preparedCamera.qpcOrigin, preparedCamera.qpcOffsetAtOrigin
                + preparedCamera.qpcTicksPer100ns * difference(stamp.pts100ns, preparedCamera.ptsOrigin100ns));
        const auto latency = rescale(preparedCamera.cameraResidualLatency100ns,
            static_cast<std::uint64_t>(master.qpcFrequency), 10000000, Rounding::nearest);
        const auto corrected = q && latency ? subtract(*q, *latency) : std::nullopt;
        const auto sample = corrected ? master.mapToSample(*corrected) : std::nullopt;
        const auto delta = sample ? subtract(*sample, origin) : std::nullopt;
        const auto time = delta ? sampleToTime100ns(*delta, rate) : std::nullopt;
        if (!time) throw std::runtime_error("Dubbing first-frame anchor unavailable; mark camera gap");
        firstPts = stamp.pts100ns; firstTime = *time; anchored = true;
    }
    const auto delta = subtract(stamp.pts100ns, firstPts);
    const auto mapped = delta ? add(firstTime, *delta) : std::nullopt;
    if (!mapped) throw std::overflow_error("Dubbing anchored PTS overflow");
    previous = stamp;
    return *mapped;
}
std::int64_t AnchoredCameraTimeMapper::now(std::int64_t qpc) const
{
    const auto delta = subtract(qpc, originQpc);
    const auto time = delta ? rescale(*delta, 10000000, static_cast<std::uint64_t>(master.qpcFrequency)) : std::nullopt;
    if (!time) throw std::overflow_error("Dubbing deadline overflow");
    return *time; // Lcam affects the image anchor only, never the output deadline.
}
std::optional<std::int64_t> nativeFrameToSample(std::int64_t frame, Rational fps, std::uint32_t rate) noexcept
{
    if (!fps.numerator || !fps.denominator || fps.numerator > 1000000 || fps.denominator > 1000000 || !rate || rate > 768000) return {};
    return rescale(frame, static_cast<std::uint64_t>(rate) * fps.denominator, fps.numerator);
}
std::optional<std::int64_t> sampleToTime100ns(std::int64_t sample, std::uint32_t rate) noexcept
{ return rate ? rescale(sample, 10000000, rate) : std::nullopt; }
}
