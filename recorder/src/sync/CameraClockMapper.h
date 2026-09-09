#pragma once
#include "ClockMapper.h"
#include "record/VideoCfrScheduler.h"

namespace gocue::recorder
{
enum class CameraClockSource { unavailable, deviceTimestamp, ptsArrivalEstimated };
const char* cameraClockSourceName(CameraClockSource) noexcept;
enum class CameraEpochReason
{
    none, initial, generationChange, ptsRegression, qpcRegression, timestampJump,
    timingSourceChange, explicitDiscontinuity, calibrationChange, invalidStamp
};
const char* cameraEpochReasonName(CameraEpochReason) noexcept;
struct CameraClockQuality
{
    CameraClockSource source = CameraClockSource::unavailable;
    bool warmingUp = true;
    std::uint64_t observations = 0, outliers = 0;
    double windowSeconds = 0, ppm = 0, residualRmsMs = 0;
    // Delay from the ESTIMATED timestamp reference to callback; not a physical
    // exposure measurement. In fallback, the reference is the 10th percentile.
    double arrivalDelayP10Ms = 0, arrivalDelayP50Ms = 0, arrivalDelayP95Ms = 0;
};
struct CameraClockSnapshot
{
    bool valid = false;
    std::uint64_t epoch = 0, generation = 0;
    CameraEpochReason reason = CameraEpochReason::none;
    std::int64_t qpcFrequency = 0, ptsOrigin100ns = 0, qpcOrigin = 0;
    std::int64_t firstPts100ns = 0, lastPts100ns = 0;
    std::int64_t cameraResidualLatency100ns = 0; // positive => timestamp is late
    double qpcTicksPer100ns = 0, qpcOffsetAtOrigin = 0;
    CameraClockQuality quality;
    std::optional<std::int64_t> timestampQpc(const FrameStamp&) const noexcept;
};
struct CaptureSample
{
    std::int64_t sample = 0, captureQpc = 0;
    std::uint64_t masterEpoch = 0, cameraEpoch = 0;
    ClockGrade masterGrade = ClockGrade::unavailable;
    CameraClockSource cameraSource = CameraClockSource::unavailable;
};
class CameraClockMapper
{
public:
    CameraClockMapper(const ClockMapper& master, std::int64_t qpcFrequency, Rational nativeFps,
                      std::int64_t cameraResidualLatency100ns = 0);
    // Sole camera/sync worker. FrameStamp has no discontinuity flag; the capture
    // owner must call reset() for MF discontinuity/type-change notifications that
    // are not represented by generation/PTS. All consumers must check BOTH epochs.
    bool observe(const FrameStamp&);
    void reset(CameraEpochReason = CameraEpochReason::explicitDiscontinuity);
    void setResidualLatency(std::int64_t latency100ns); // worker; new epoch
    std::optional<CameraClockSnapshot> snapshot() const noexcept { return published.read(); }
    std::optional<CaptureSample> captureSample(const FrameStamp&) const noexcept;
    std::optional<CaptureSample> captureSample(const FrameStamp&, const ClockSnapshot&) const noexcept;
    const ClockMapper& masterClock() const noexcept { return master; }
    Rational nativeRate() const noexcept { return fps; }
private:
    void beginEpoch(CameraEpochReason, const FrameStamp&);
    void fit();
    const ClockMapper& master;
    Rational fps;
    CameraClockSnapshot state;
    AtomicSnapshot<CameraClockSnapshot> published;
    FrameStamp previous;
    bool havePrevious = false;
    std::int64_t lastFitQpc = 0;
    std::vector<clock_detail::Point> window;
    std::vector<double> directDelays;
};
// Production CFR adapter. N0 (recording) or O0 (dubbing) is explicitly reserved
// after clock warmup. It is NEVER the first callback/first retained frame. The
// camera worker calls observe(stamp) before submitting that frame to this adapter;
// map() only reads snapshots, so already observed preroll can be mapped again.
// Epoch changes throw so the caller stops the take/marks the camera gap. Negative
// preroll times remain negative; the owner retains/selects preroll before pushing
// nonnegative CfrInput into VideoCfrScheduler. now() never applies Lcam.
class CameraSampleTimeMapper final : public CameraTimeMapper
{
public:
    CameraSampleTimeMapper(CameraClockMapper&, std::int64_t originSample, std::uint32_t sampleRate,
                          std::uint64_t masterEpoch, std::uint64_t cameraEpoch);
    std::int64_t map(const FrameStamp&) override;
    std::int64_t now(std::int64_t qpc) const override;
private:
    CameraClockMapper& camera;
    std::int64_t origin;
    std::uint32_t rate;
    std::uint64_t audioEpoch, videoEpoch;
};
// Absolute rational rescale, floor rounding; never accumulate rounded periods.
std::optional<std::int64_t> nativeFrameToSample(std::int64_t frame, Rational fps, std::uint32_t sampleRate) noexcept;
std::optional<std::int64_t> sampleToTime100ns(std::int64_t sample, std::uint32_t sampleRate) noexcept;
}
