#include "TestSupport.h"
#include "sync/ClockMapper.h"
#include "sync/CameraClockMapper.h"
#include "sync/CalibrationProfile.h"
#include <atomic>
#include <cmath>
#include <thread>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
constexpr std::int64_t hz = 10000000, q0 = 1234567890000LL, s0 = 987654321;
BlockStamp stamp(std::uint64_t i, double ppm = 0, double jitterSeconds = 0)
{
    BlockStamp s{};
    s.flags = timeInfoPresent | samplePositionValid | systemTimeValid | sampleRateValid | latenciesValid;
    s.samplePosition = s0 + static_cast<std::int64_t>(i) * 480;
    s.callbackQpc = q0 + static_cast<std::int64_t>(std::llround((static_cast<double>(i) * 0.01 / (1 + ppm * 1e-6) + jitterSeconds) * hz));
    s.sequence = i; s.sampleRate = 48000; s.numSamples = 480; s.bufferIndex = static_cast<int>(i % 2);
    s.systemTimeRaw = 400000000000ull + i * 10000000; // intentionally NOT QPC epoch or ticks
    s.inputLatencySamples = 320; s.outputLatencySamples = 960;
    return s;
}
ClockSnapshot ready(ClockMapper& mapper, int count = 1001)
{
    for (int i = 0; i < count; ++i) mapper.observe(stamp(static_cast<std::uint64_t>(i)));
    const auto result = mapper.snapshot(); require(result && result->valid, "clock did not converge"); return *result;
}
FrameStamp frame(std::uint64_t i, bool device = true, std::int64_t baseQpc = q0, std::int64_t latency100ns = 300000)
{
    FrameStamp s;
    s.frame = i + 1; s.generation = 7;
    s.pts100ns = static_cast<std::int64_t>(i) * 10000000 / 60;
    s.callback = baseQpc + s.pts100ns + latency100ns;
    s.hasDeviceTimestamp = device; s.deviceTimestampValid = device;
    s.deviceTimestamp100ns = static_cast<std::uint64_t>(baseQpc + s.pts100ns);
    return s;
}
void requireNear(double actual, double expected, double tolerance, const char* message)
{
    if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(std::string(message) + "; actual=" + std::to_string(actual)
            + ", expected=" + std::to_string(expected) + ", tolerance=" + std::to_string(tolerance));
}
CalibrationProfile profile()
{
    CalibrationProfile p;
    p.key = {"camera-한글", "MJPEG:1920x1080", "manual:1/120", "Studio ASIO", {60000, 1001}, 48000, 256, {3, 1}};
    p.cameraResidualLatency100ns = 123456; p.inputResidualLatencySamples = -31; p.outputResidualLatencySamples = 127;
    p.measuredUtc = "2026-09-09T12:34:56Z"; p.method = "synthetic fixture";
    p.quality = CalibrationQuality::softwareEstimated; p.measurementCount = 42; p.residualRmsSamples = 0.25;
    return p;
}
}
int runClockMapperTests()
{
    Suite suite;
    suite.test("100ppm, callback jitter and MAD outliers: accuracy and inverse", []
    {
        ClockMapper m(hz); double largestPhaseUpdate = 0;
        for (std::uint64_t i = 0; i <= 12000; ++i)
        {
            const auto jitter = ((static_cast<int>(i % 11) - 5) * 0.00001) + (i % 97 == 41 ? 0.004 : 0.0);
            const auto input = stamp(i, 100, jitter);
            const auto before = m.snapshot(); m.observe(input); const auto after = m.snapshot();
            if (i > 2000 && before && before->valid && after && after->valid)
            {
                const auto a = before->mapToSample(input.callbackQpc), b = after->mapToSample(input.callbackQpc);
                if (a && b) largestPhaseUpdate = std::max(largestPhaseUpdate, std::abs(static_cast<double>(*b - *a)));
            }
        }
        const auto s = *m.snapshot();
        std::cout << "ASIO fixture: ppm=" << s.quality.ppm << ", RMS samples=" << s.quality.residualRmsSamples
            << ", MAD outliers=" << s.quality.outliers << ", max phase update samples=" << largestPhaseUpdate << '\n';
        require(s.epoch == 1 && s.valid, "jitter/outliers manufactured an epoch");
        require(s.quality.grade == ClockGrade::asioObserved && s.quality.outliers > 0, "MAD/quality absent");
        require(s.quality.windowSeconds >= 7.9 && s.quality.windowSeconds <= 8.01 && s.quality.observations > 700, "wrong regression window");
        requireNear(s.quality.ppm, 100, 4, "100ppm slope inaccurate");
        require(s.quality.residualRmsSamples < 4 && largestPhaseUpdate <= 4, "clock jitter leaked into mapping updates");
        const auto clean = stamp(12000, 100);
        const auto mapped = s.mapToSample(clean.callbackQpc); require(mapped.has_value(), "mapping absent");
        requireNear(static_cast<double>(*mapped - clean.samplePosition), 0, 5, "sample mapping error");
        const auto inverse = s.mapToQpc(clean.samplePosition); require(inverse.has_value(), "inverse absent");
        requireNear(static_cast<double>(*inverse - clean.callbackQpc), 0, hz * 5.0 / 48000, "inverse error");
        require(!s.mapToSample(clean.callbackQpc + hz), "stale clock extrapolated after stop");
    });
    suite.test("every ASIO discontinuity starts a new invalid epoch", []
    {
        const auto check = [](const std::function<void(BlockStamp&)>& modify, ClockEpochReason reason)
        {
            ClockMapper m(hz); const auto old = ready(m); auto next = stamp(1001); modify(next); m.observe(next);
            const auto s = *m.snapshot();
            require(s.epoch == old.epoch + 1 && !s.valid && hasReason(s.reason, reason), "epoch event missing or smoothed away");
            require(m.epochEvents().back().sequence == next.sequence, "epoch evidence missing");
            require(old.valid && old.epoch == 1, "published value mutated");
        };
        check([](auto& s){ ++s.resets; }, ClockEpochReason::asioReset);
        check([](auto& s){ s.sampleRate = 44100; }, ClockEpochReason::sampleRateChange);
        check([](auto& s){ s.numSamples = 256; }, ClockEpochReason::bufferChange);
        check([](auto& s){ s.samplePosition -= 1000; }, ClockEpochReason::sampleRegression);
        check([](auto& s){ s.samplePosition += 96000; }, ClockEpochReason::sampleJump);
        check([](auto& s){ s.callbackQpc -= hz; }, ClockEpochReason::qpcRegression);
        check([](auto& s){ s.callbackQpc += hz; }, ClockEpochReason::timeJump);
        check([](auto& s){ ++s.sequence; }, ClockEpochReason::observationGap);
        check([](auto& s){ ++s.resyncs; }, ClockEpochReason::resync);
        check([](auto& s){ ++s.xruns; }, ClockEpochReason::xrun);
        check([](auto& s){ ++s.latencyChanges; }, ClockEpochReason::latencyChange);
        check([](auto& s){ s.flags &= ~systemTimeValid; }, ClockEpochReason::timingSourceChange);
        check([](auto& s){ s.numSamples = 0; }, ClockEpochReason::invalidStamp);
    });
    suite.test("reset establishes fresh coordinates and bounded event history", []
    {
        ClockMapper m(hz); ready(m);
        for (std::uint64_t i = 1001; i < 1800; ++i)
        { auto s = stamp(i); s.resets = 1; s.samplePosition = static_cast<std::int64_t>(i - 1001) * 480; m.observe(s); }
        const auto s = *m.snapshot();
        require(s.valid && s.epoch == 2, "new epoch did not reconverge");
        requireNear(static_cast<double>(*s.mapToSample(stamp(1799).callbackQpc)), 798 * 480, 1, "old epoch offset hidden in new coordinates");
        for (int i = 0; i < 200; ++i) m.reset();
        require(m.epochEvents().size() == 128 && m.omittedEpochEvents() == 74, "unbounded epoch history");
    });
    suite.test("legacy fallback uses callback QPC plus accumulated numSamples", []
    {
        ClockMapper m(hz);
        for (std::uint64_t i = 0; i <= 1000; ++i)
        { auto s = stamp(i); s.flags = 0; s.samplePosition = -123; s.systemTimeRaw = UINT64_MAX; m.observe(s); }
        auto s = *m.snapshot();
        require(s.valid && s.quality.grade == ClockGrade::callbackEstimated, "fallback was not labelled low quality");
        require(s.quality.source == ClockSource::accumulatedCallbackQpc, "fallback source wrong");
        requireNear(static_cast<double>(*s.mapToSample(stamp(1000).callbackQpc)), 480000, 1, "fallback did not accumulate samples");
        auto gap = stamp(1002); gap.flags = 0; m.observe(gap);
        require(!m.snapshot()->valid && hasReason(m.snapshot()->reason, ClockEpochReason::observationGap), "fallback fabricated dropped samples");
    });
    suite.test("invalid systemTime with valid position is still low quality", []
    {
        ClockMapper m(hz);
        for (std::uint64_t i = 0; i <= 700; ++i) { auto s = stamp(i); s.flags &= ~systemTimeValid; m.observe(s); }
        const auto s = *m.snapshot();
        require(s.quality.grade == ClockGrade::callbackEstimated, "invalid systemTime upgraded quality");
        requireNear(static_cast<double>(*s.mapToSample(stamp(700).callbackQpc) - stamp(700).samplePosition), 0, 1, "fallback anchor mismatch");
    });
    suite.test("submillisecond ASIO blocks retain a full eight-second observation window", []
    {
        ClockMapper m(hz);
        for (std::uint64_t i = 0; i < 12000; ++i)
        { auto s = stamp(i); s.numSamples = 48; s.samplePosition = s0 + static_cast<std::int64_t>(i) * 48;
          s.callbackQpc = q0 + static_cast<std::int64_t>(i) * 10000; m.observe(s); }
        const auto s = *m.snapshot();
        require(s.valid && s.quality.windowSeconds >= 7.99 && s.quality.observations <= 2048, "capacity shortened window");
    });
    suite.test("int64 origins above 2^53 retain sample and QPC detail", []
    {
        ClockMapper m(hz); constexpr auto huge = (std::int64_t{1} << 54) + 13; BlockStamp last{};
        for (std::uint64_t i = 0; i <= 1000; ++i)
        { auto s = stamp(i); s.callbackQpc += huge; s.samplePosition += huge; m.observe(s); last = s; }
        const auto snapshot = *m.snapshot();
        require(*snapshot.mapToSample(last.callbackQpc) == last.samplePosition, "lost large-origin samples");
        require(*snapshot.mapToQpc(last.samplePosition) == last.callbackQpc, "lost large-origin ticks");
        require(!clock_math::add(INT64_MAX, 1) && !clock_math::subtract(INT64_MIN, 1), "overflow accepted");
    });
    suite.test("native 60000/1001 to 60: three-hour absolute rational oracle", []
    {
        constexpr std::int64_t total = 3 * 60 * 60 * 60;
        std::int64_t previousNative = -1, repeats = 0;
        for (std::int64_t k = 0; k <= total; ++k)
        {
            const auto nativeIndex = VideoCfrScheduler::nearestNativeIndex(k, {60000, 1001}, {60, 1});
            require(nativeIndex == (k * 1000 + 500) / 1001, "native selection accumulated error");
            const auto sample = nativeFrameToSample(nativeIndex, {60000, 1001}, 48000);
            require(sample && *sample == nativeIndex * 4004 / 5, "59.94 treated as 60 or accumulated rounding");
            const auto project = nativeFrameToSample(k, {60, 1}, 48000);
            require(project && *project == k * 800, "project sample grid drift");
            require(*sampleToTime100ns(*project, 48000) == VideoCfrScheduler::gridTime(k, {60, 1}), "CFR timeline differs");
            repeats += nativeIndex == previousNative; previousNative = nativeIndex;
        }
        require(*nativeFrameToSample(total, {60, 1}, 48000) == 518400000 && repeats == 647, "three-hour end/cadence wrong");
        require(*nativeFrameToSample(-1, {60000, 1001}, 48000) == -801, "preroll floor rounding wrong");
    });
    suite.test("128-bit rescale handles uptime, signed limits and overflow", []
    {
        using namespace clock_math;
        require(*rescale(INT64_MAX, 10000000, 10000000) == INT64_MAX, "intermediate overflow rejected exact result");
        require(*rescale(INT64_MIN, 10000000, 10000000) == INT64_MIN, "INT64_MIN rescale failed");
        require(!rescale(INT64_MAX, 2, 1) && !rescale(1, 1, 0), "overflow/zero divisor accepted");
        require(*rescale(-5, 1, 2) == -3 && *rescale(5, 1, 2, Rounding::nearest) == 3, "rounding policy wrong");
        require(!nativeFrameToSample(INT64_MAX, {60000, 1001}, 48000), "frame/sample overflow accepted");
    });
    suite.test("O0 / Pvideo signs, buffer offset, residual and no duplicate input correction", []
    {
        const OutputBufferStamp buffer{50000, 1000, 256, 3};
        const auto o0 = outputOriginSample(1100, 960, buffer, 40);
        require(o0 && *o0 == 51100, "output latency must move O0 later");
        require(*projectVideoSample(1100, *o0, *o0) == 1100, "Pstart anchor moved");
        require(*projectVideoSample(1100, *o0 - 480, *o0) == 620, "early video sign wrong");
        require(*projectVideoSample(1100, *o0 + 480, *o0) == 1580, "late video sign wrong");
        const auto corrected = (*o0 + 320) - 320; // input adapter already removed input latency
        require(*projectInputSample(1100, corrected, *o0) == 1100, "input latency applied twice");
        require(!outputOriginSample(1300, 960, buffer) && !outputOriginSample(1100, -1, buffer), "invalid output reference accepted");
        require(!projectVideoSample(INT64_MAX, 1, 0), "placement overflow accepted");
        ClockMapper master(hz); const auto s = ready(master);
        require(*s.mapToSample(stamp(1000).callbackQpc) == stamp(1000).samplePosition, "mapper silently applied input latency");
    });
    suite.test("DeviceTimestamp uses 100ns QPC epoch at a non-10MHz frequency", []
    {
        constexpr std::int64_t frequency = 24000000; ClockMapper master(frequency);
        for (std::uint64_t i = 0; i <= 1000; ++i)
        { auto s = stamp(i); s.callbackQpc = *clock_math::rescale(s.callbackQpc, frequency, hz); master.observe(s); }
        CameraClockMapper camera(master, frequency, {60, 1}, 50000);
        auto f = frame(600); f.callback = *clock_math::rescale(f.callback, frequency, hz);
        require(camera.observe(f), "device frame rejected");
        const auto sample = camera.captureSample(f);
        require(sample && sample->cameraSource == CameraClockSource::deviceTimestamp, "device clock path missing");
        require(sample->sample == s0 + 480000 - 240, "device units/Lcam sign wrong");
        requireNear(camera.snapshot()->quality.arrivalDelayP50Ms, 30, 0.001, "arrival/exposure distinction lost");
    });
    suite.test("PTS fallback lower arrival envelope, latency distribution and low quality", []
    {
        ClockMapper master(hz); ready(master, 3001); CameraClockMapper camera(master, hz, {60, 1}, 300000); FrameStamp last;
        for (std::uint64_t i = 0; i <= 1200; ++i)
        {
            const auto delay = 300000 + static_cast<std::int64_t>(i % 5) * 5000 + (i % 73 == 31 ? 80000 : 0);
            last = frame(i, false, q0, delay); camera.observe(last);
        }
        const auto sample = camera.captureSample(last); const auto state = *camera.snapshot();
        std::cout << "Camera fallback fixture: ppm=" << state.quality.ppm << ", RMS ms=" << state.quality.residualRmsMs
            << ", MAD outliers=" << state.quality.outliers << '\n';
        require(sample && sample->cameraSource == CameraClockSource::ptsArrivalEstimated && !state.quality.warmingUp, "fallback quality absent");
        requireNear(static_cast<double>(sample->sample - (s0 + 960000)), 0, 15, "fallback/Lcam inaccurate");
        require(state.quality.arrivalDelayP95Ms > state.quality.arrivalDelayP10Ms && state.quality.outliers > 0, "delay distribution absent");
        requireNear(state.quality.ppm, 0, 100, "fallback slope biased");
    });
    suite.test("camera timestamp and generation discontinuities expose epochs", []
    {
        ClockMapper master(hz); ready(master); CameraClockMapper camera(master, hz, {60, 1});
        auto a = frame(300); camera.observe(a); auto b = frame(301); b.generation++; camera.observe(b);
        require(camera.snapshot()->epoch == 2 && camera.snapshot()->reason == CameraEpochReason::generationChange, "camera generation hidden");
        b.frame++; b.pts100ns--; camera.observe(b);
        require(camera.snapshot()->epoch == 3 && camera.snapshot()->reason == CameraEpochReason::ptsRegression, "camera PTS regression hidden");
        auto c = frame(303, false); c.generation = b.generation; camera.observe(c);
        require(!camera.snapshot()->valid && camera.snapshot()->reason == CameraEpochReason::timingSourceChange, "camera source transition reused fit");
        auto d = frame(304, false); d.generation = c.generation; d.pts100ns += 10000000; camera.observe(d);
        require(camera.snapshot()->reason == CameraEpochReason::timestampJump, "camera timestamp jump hidden");
        camera.reset(); require(!camera.snapshot()->valid, "explicit MF discontinuity retained mapping");
    });
    suite.test("invalid DeviceTimestamp falls back and cannot masquerade as QPC", []
    {
        ClockMapper master(hz); ready(master); CameraClockMapper camera(master, hz, {60, 1});
        auto f = frame(300); f.deviceTimestamp100ns = UINT64_MAX; camera.observe(f);
        require(camera.snapshot()->quality.source == CameraClockSource::ptsArrivalEstimated && !camera.captureSample(f), "invalid device timestamp used directly");
        f = frame(301); f.deviceTimestamp100ns = static_cast<std::uint64_t>(f.callback + hz); camera.observe(f);
        require(camera.snapshot()->quality.source == CameraClockSource::ptsArrivalEstimated, "future device timestamp accepted");
    });
    suite.test("CFR adapter supplies N0/O0-relative time, unshifted deadline, rejects epochs", []
    {
        ClockMapper master(hz); const auto audio = ready(master); CameraClockMapper camera(master, hz, {60, 1}, 100000);
        auto f = frame(600); camera.observe(f);
        CameraSampleTimeMapper adapter(camera, s0 + 480000, 48000, audio.epoch, *camera.snapshot());
        require(adapter.map(f) == -100000, "negative preroll was clamped or first-frame zeroed");
        require(adapter.now(f.callback) == 300000, "Lcam incorrectly shifted CFR deadline");
        f = frame(601); camera.observe(f); require(adapter.map(f) == 66666, "mapped CFR sample time wrong");
        master.reset(); rejects([&]{ adapter.now(f.callback); }); rejects([&]{ adapter.map(f); });
    });
    suite.test("Calibration binds stereo input pair and rejects stale mono mapping", []
    {
        auto p = profile(); p.key.inputMapping = {6,2,3};
        const auto loaded = CalibrationProfile::deserialize(p.serialize()); require(loaded.key == p.key, "Stereo calibration key round trip");
        auto mono = p.key; mono.inputMapping = {6,2,-1}; rejects([&] { loaded.requireMatch(mono); });
        mono.inputMapping.clear(); rejects([&] { loaded.requireMatch(mono); });
        p.key.inputMapping = {6,2,3,7,3,-1}; rejects([&] { p.serialize(); });
    });
    suite.test("calibration JSON roundtrip preserves mode, fps, mapping order, signs", []
    {
        const auto p = profile(), restored = CalibrationProfile::deserialize(p.serialize());
        require(p.key == restored.key && restored.key.fps.denominator == 1001 && restored.key.outputMapping[0] == 3, "calibration key changed");
        require(restored.cameraResidualLatency100ns == 123456 && restored.inputResidualLatencySamples == -31
            && restored.outputResidualLatencySamples == 127 && restored.measuredUtc == p.measuredUtc, "calibration value changed");
        auto changed = p.key; changed.exposure = "auto"; require(!(changed == p.key), "exposure missing from key");
        changed = p.key; changed.outputMapping = {1, 3}; require(!(changed == p.key), "output routing missing from key");
    });
    suite.test("calibration rejects future schemas, lossy numbers and incomplete measurements", []
    {
        auto p = profile(); auto json = p.toJson(); jsonSet(json, "schemaVersion", 2); rejects([&]{ CalibrationProfile::fromJson(json); });
        json = p.toJson(); jsonSet(json, "inputResidualLatencySamples", 1.25); rejects([&]{ CalibrationProfile::fromJson(json); });
        json = p.toJson(); jsonSet(json, "outputResidualLatencySamples", "9223372036854775808"); rejects([&]{ CalibrationProfile::fromJson(json); });
        p.measuredUtc.clear(); rejects([&]{ p.serialize(); });
        p = profile(); p.key.outputMapping = {1, 1}; rejects([&]{ p.serialize(); });
        p = profile(); p.residualRmsSamples = NAN; rejects([&]{ p.serialize(); });
    });
    suite.test("immutable snapshot publication is coherent under concurrent readers", []
    {
        struct Value { std::uint64_t epoch = 0, inverse = UINT64_MAX; std::array<std::uint64_t, 16> words{}; };
        AtomicSnapshot<Value> mailbox; std::atomic<bool> done{false}, bad{false}; std::atomic<std::uint64_t> reads{0};
        auto reader = [&]
        {
            do { if (const auto value = mailbox.read())
            {
                if (value->inverse != ~value->epoch) bad.store(true);
                for (const auto word : value->words) if (word != value->epoch) bad.store(true);
                ++reads;
            } } while (!done.load());
        };
        std::thread a(reader), b(reader);
        for (std::uint64_t i = 1; i <= 20000; ++i) { Value v; v.epoch = i; v.inverse = ~i; v.words.fill(i); mailbox.publish(v); }
        done.store(true); a.join(); b.join();
        require(!bad.load() && reads.load() > 0, "torn immutable snapshot");
    });
    return suite.result("ClockMapper");
}
