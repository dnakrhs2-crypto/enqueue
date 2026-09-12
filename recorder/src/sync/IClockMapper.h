#pragma once
#include "audio/AsioTimingBridge.h"
#include <mutex>

namespace gocue::recorder
{
struct ClockMapping
{
    bool valid = false;
    std::int64_t originQpc = 0, originSample = 0;
    double samplesPerTick = 0, intercept = 0;
    double residualRmsSamples = 0;
    std::int64_t mapToSample(std::int64_t qpc) const;
    std::int64_t mapToQpc(std::int64_t sample) const;
};
// Round 04 insertion point. observe is called on a worker, snapshot on control /
// camera workers; never either method on ASIO. Snapshot is an immutable value.
// A valid snapshot's origin must belong to the latest uninterrupted ASIO epoch.
class IClockMapper
{
public:
    virtual ~IClockMapper() = default;
    virtual void observe(const AsioStampStatistics&) = 0;
    virtual ClockMapping snapshot() const = 0;
};
// Temporary OLS from round 03 statistics. No outlier rejection, smoothing,
// latency correction or exposure-time certification.
class LinearClockMapper final : public IClockMapper
{
public:
    void observe(const AsioStampStatistics&) override;
    ClockMapping snapshot() const override;
private:
    mutable std::mutex mutex;
    ClockMapping current;
};
// Interface only for round 17. Output latency is applied exactly once here;
// source playback still begins at Pstart. No dubbing renderer in rounds 09/10.
struct DubOrigin { std::int64_t Pstart = 0, O0 = 0; };
class IDubOriginPlanner
{
public:
    virtual ~IDubOriginPlanner() = default;
    virtual DubOrigin outputOrigin(std::int64_t Pstart, std::int64_t submittedSample,
                                   int reportedOutputLatency, int residualOutputLatency) const = 0;
};
}
