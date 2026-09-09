#include "audio/AsioTimingBridge.h"
#include "capture/MfCameraCapture.h"
#include "sync/CameraClockMapper.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <thread>

using namespace gocue::recorder;
namespace
{
struct Options
{
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
    bool has(const std::string& key) const { return values.count(key) || flags.count(key); }
    std::string get(const std::string& key, const std::string& fallback = {}) const
    { const auto i = values.find(key); return i == values.end() ? fallback : i->second; }
};
int integer(const std::string& text, int minimum, int maximum)
{
    int value = 0; const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument("Integer outside allowed range: " + text);
    return value;
}
Options options(int argc, wchar_t** argv)
{
    Options o;
    const std::set<std::string> flags{"--help", "--list", "--physical-events"};
    const std::set<std::string> values{"--asio-device", "--seconds", "--report", "--devices", "--camera", "--sample-rate", "--buffer-size", "--asio-output"};
    for (int i = 1; i < argc; ++i)
    {
        const auto key = juce::String(argv[i]).toStdString();
        if (o.has(key)) throw std::invalid_argument("Duplicate option: " + key);
        if (flags.count(key)) o.flags.insert(key);
        else if (values.count(key) && i + 1 < argc) o.values.emplace(key, juce::String(argv[++i]).toStdString());
        else throw std::invalid_argument("Unknown/incomplete option: " + key);
    }
    return o;
}
juce::File file(const std::string& path) { return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(path.c_str())); }
void status(juce::var& report, const char* result, const std::string& reason)
{
    jsonSet(report, "result", result); jsonSet(report, "reason", reason);
    jsonSet(report, "status", std::string(result) == "PASS" ? "available" : std::string(result) == "FAIL" ? "failed" : "unavailable");
}
juce::var clockJson(const ClockSnapshot& s)
{
    auto v = jsonObject();
    jsonSet(v, "valid", s.valid); jsonSet(v, "epoch", std::to_string(s.epoch));
    jsonSet(v, "aSamplesPerQpcTick", s.samplesPerQpcTick);
    jsonSet(v, "bSamplesApproximate", static_cast<double>(s.sampleOrigin) + s.sampleOffsetAtOrigin - s.samplesPerQpcTick * static_cast<double>(s.qpcOrigin));
    jsonSet(v, "originQpc", std::to_string(s.qpcOrigin)); jsonSet(v, "originSample", std::to_string(s.sampleOrigin));
    jsonSet(v, "offsetSamplesAtOrigin", s.sampleOffsetAtOrigin); jsonSet(v, "observedThroughQpc", std::to_string(s.observedThroughQpc));
    jsonSet(v, "nominalSampleRate", s.nominalSampleRate); jsonSet(v, "source", clockSourceName(s.quality.source));
    jsonSet(v, "grade", clockGradeName(s.quality.grade)); jsonSet(v, "ppm", s.quality.ppm); jsonSet(v, "fittedPpm", s.quality.fittedPpm);
    jsonSet(v, "residualRmsSamples", s.quality.residualRmsSamples); jsonSet(v, "fitResidualRmsSamples", s.quality.fitResidualRmsSamples);
    jsonSet(v, "observations", s.quality.observations); jsonSet(v, "outliers", s.quality.outliers); jsonSet(v, "windowSeconds", s.quality.windowSeconds);
    return v;
}
juce::var reasons(ClockEpochReason reason)
{
    const std::pair<ClockEpochReason, const char*> names[]{
        {ClockEpochReason::initial, "initial"}, {ClockEpochReason::asioReset, "asio-reset"}, {ClockEpochReason::sampleRateChange, "sample-rate-change"},
        {ClockEpochReason::bufferChange, "buffer-change"}, {ClockEpochReason::sampleRegression, "sample-regression"}, {ClockEpochReason::sampleJump, "sample-jump"},
        {ClockEpochReason::qpcRegression, "qpc-regression"}, {ClockEpochReason::timeJump, "time-jump"}, {ClockEpochReason::observationGap, "observation-gap"},
        {ClockEpochReason::timingSourceChange, "timing-source-change"}, {ClockEpochReason::resync, "resync"}, {ClockEpochReason::xrun, "xrun"},
        {ClockEpochReason::latencyChange, "latency-change"}, {ClockEpochReason::invalidStamp, "invalid-stamp"}, {ClockEpochReason::explicitReset, "explicit-reset"}};
    juce::Array<juce::var> a; for (const auto& n : names) if (hasReason(reason, n.first)) a.add(n.second); return a;
}
struct SampleDistribution
{
    RunningMoments moments;
    std::array<double, 4096> recent{};
    void add(double v) { recent[moments.count % recent.size()] = v; moments.add(v); }
    juce::var toJson() const
    {
        auto v = jsonObject(); jsonSet(v, "count", moments.count); jsonSet(v, "mean", moments.mean);
        jsonSet(v, "minimum", moments.minimum); jsonSet(v, "maximum", moments.maximum); jsonSet(v, "stddevPopulation", moments.standardDeviation());
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(recent.size(), moments.count));
        const std::vector<double> window(recent.begin(), recent.begin() + count);
        jsonSet(v, "p50", clock_detail::quantile(window, 0.5)); jsonSet(v, "p95", clock_detail::quantile(window, 0.95));
        jsonSet(v, "percentileScope", "latest <=4096 intervals; mean/min/max cover all intervals"); jsonSet(v, "unit", "samples"); return v;
    }
};
class SilentOutput final : public juce::AudioIODeviceCallback
{
public:
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int channels, int samples,
                                         const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < channels; ++c) if (outputs[c]) std::memset(outputs[c], 0, static_cast<std::size_t>(samples) * sizeof(float));
        callbacks.fetch_add(1, std::memory_order_relaxed);
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override { stopped.store(true); }
    void audioDeviceError(const juce::String&) override { failed.store(true); }
    std::atomic<bool> stopped{false}, failed{false};
    std::atomic<std::uint64_t> callbacks{0};
};
struct Session
{
    std::unique_ptr<juce::AudioIODevice> device;
    AsioTimingBridge bridge{qpcFrequency()};
    ClockMapper mapper{qpcFrequency()};
    SilentOutput output;
    std::shared_ptr<CaptureTelemetry> telemetry;
    std::unique_ptr<VideoSurfacePool> pool;
    std::unique_ptr<CameraClockMapper> camera;
    std::unique_ptr<MfCameraCapture> capture;
    BoundedSpscQueue<FrameStamp, 256> frames;
    std::atomic<std::uint64_t> frameOverflows{0};
    SampleDistribution intervals, errors;
    std::optional<CaptureSample> previous;
    FrameStamp previousFrame;
    juce::Array<juce::var> trace, cameraEvents, history;
    std::uint64_t mapped = 0, unmapped = 0, excludedIntervals = 0, cameraEpoch = 0, historyOmitted = 0, cameraEventsOmitted = 0;
    std::atomic<bool> workerFailed{false}, clockDone{false};
    std::thread clockWorker;
    std::uint64_t mfDiscontinuities = 0, mfTypeChanges = 0;
    bool closed = false;
    double rate = 0;
    void drainClock() noexcept
    {
        bridge.drain([](void* context, const BlockStamp& s) noexcept
        {
            auto& session = *static_cast<Session*>(context);
            try { session.mapper.observe(s); } catch (...) { session.workerFailed.store(true); }
        }, this);
    }
    void startClockWorker()
    {
        clockWorker = std::thread([this]
        {
            while (!clockDone.load()) { drainClock(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            drainClock();
        });
    }
    void drain()
    {
        if (camera && telemetry)
        {
            const auto discontinuities = telemetry->count(LossReason::sourceDiscontinuity);
            const auto typeChanges = telemetry->count(LossReason::sourceTypeChanged);
            if (discontinuities != mfDiscontinuities || typeChanges != mfTypeChanges)
            { camera->reset(); previous.reset(); }
            mfDiscontinuities = discontinuities; mfTypeChanges = typeChanges;
        }
        FrameStamp frame;
        while (frames.pop(frame))
        {
            camera->observe(frame); const auto state = camera->snapshot();
            if (state && state->epoch != cameraEpoch)
            {
                cameraEpoch = state->epoch; auto event = jsonObject();
                jsonSet(event, "epoch", std::to_string(cameraEpoch)); jsonSet(event, "frame", std::to_string(frame.frame));
                jsonSet(event, "reason", cameraEpochReasonName(state->reason));
                if (cameraEvents.size() < 128) cameraEvents.add(event); else ++cameraEventsOmitted;
            }
            const auto sample = camera->captureSample(frame);
            if (!sample) { ++unmapped; previous.reset(); continue; }
            ++mapped;
            if (previous && previous->masterEpoch == sample->masterEpoch && previous->cameraEpoch == sample->cameraEpoch
                && frame.frame == previousFrame.frame + 1)
            {
                const auto interval = clock_math::difference(sample->sample, previous->sample);
                intervals.add(interval); errors.add(interval - rate / camera->nativeRate().value());
            }
            else ++excludedIntervals;
            if (trace.size() < 64)
            {
                auto item = jsonObject(); jsonSet(item, "frame", std::to_string(frame.frame)); jsonSet(item, "pts100ns", std::to_string(frame.pts100ns));
                jsonSet(item, "deviceTimestamp100ns", std::to_string(frame.deviceTimestamp100ns)); jsonSet(item, "hasDeviceTimestamp", frame.hasDeviceTimestamp);
                jsonSet(item, "deviceTimestampValid", frame.deviceTimestampValid); jsonSet(item, "callbackQpc", std::to_string(frame.callback));
                jsonSet(item, "captureQpc", std::to_string(sample->captureQpc)); jsonSet(item, "captureSample", std::to_string(sample->sample));
                jsonSet(item, "masterEpoch", std::to_string(sample->masterEpoch)); jsonSet(item, "cameraEpoch", std::to_string(sample->cameraEpoch));
                jsonSet(item, "source", cameraClockSourceName(sample->cameraSource)); trace.add(item);
            }
            previous = sample; previousFrame = frame;
        }
        if (pool) { const auto index = pool->takeLatest(); if (index != VideoSurfacePool::none) pool->release(index); }
    }
    void close()
    {
        if (closed) return;
        if (device) { device->stop(); device->close(); }
        bridge.unregisterAfterDeviceClosed();
        clockDone.store(true);
        if (clockWorker.joinable()) clockWorker.join();
        if (capture) capture->stop();
        closed = true; drain();
    }
    ~Session() { try { close(); } catch (...) {} }
};
juce::var cameraJson(const Session& s)
{
    auto v = jsonObject();
    if (!s.camera) { status(v, "UNAVAILABLE", "Camera not requested/opened"); return v; }
    const auto snapshot = s.camera->snapshot();
    if (snapshot)
    {
        const auto& q = snapshot->quality;
        jsonSet(v, "valid", snapshot->valid); jsonSet(v, "source", cameraClockSourceName(q.source)); jsonSet(v, "warmingUp", q.warmingUp);
        jsonSet(v, "epoch", snapshot->epoch); jsonSet(v, "observations", q.observations); jsonSet(v, "outliers", q.outliers);
        jsonSet(v, "aQpcTicksPer100ns", snapshot->qpcTicksPer100ns); jsonSet(v, "originPts100ns", std::to_string(snapshot->ptsOrigin100ns));
        jsonSet(v, "originQpc", std::to_string(snapshot->qpcOrigin)); jsonSet(v, "offsetQpcTicks", snapshot->qpcOffsetAtOrigin);
        jsonSet(v, "ppm", q.ppm); jsonSet(v, "residualRmsMs", q.residualRmsMs);
        jsonSet(v, "arrivalDelayP10Ms", q.arrivalDelayP10Ms); jsonSet(v, "arrivalDelayP50Ms", q.arrivalDelayP50Ms); jsonSet(v, "arrivalDelayP95Ms", q.arrivalDelayP95Ms);
    }
    jsonSet(v, "nativeFps", s.camera->nativeRate().text()); jsonSet(v, "expectedIntervalSamples", s.rate / s.camera->nativeRate().value());
    const auto expected = s.rate / s.camera->nativeRate().value();
    if (s.intervals.moments.count && expected > 0)
    {
        const auto deviation = s.intervals.moments.mean / expected - 1;
        jsonSet(v, "meanIntervalDeviationPpm", deviation * 1e6);
        jsonSet(v, "meanIntervalWithinOnePercent", std::abs(deviation) <= 0.01);
    }
    jsonSet(v, "cadenceComparison", "One-percent mean comparison is diagnostic only; not a physical sync or capture-loss gate");
    jsonSet(v, "mappedFrames", s.mapped); jsonSet(v, "unmappedFramesIncludingWarmup", s.unmapped); jsonSet(v, "excludedNonConsecutiveIntervals", s.excludedIntervals);
    jsonSet(v, "intervalSamples", s.intervals.toJson()); jsonSet(v, "intervalErrorSamples", s.errors.toJson());
    jsonSet(v, "traceFirst64", s.trace); jsonSet(v, "epochEvents", s.cameraEvents); jsonSet(v, "omittedEpochEvents", s.cameraEventsOmitted);
    jsonSet(v, "stampQueueOverflows", s.frameOverflows.load()); jsonSet(v, "captureTelemetry", s.telemetry->toJson());
    jsonSet(v, "calibration", "unmeasured; Lcam=0. Callback arrival/lower envelope is not sensor exposure. No physical sync verdict.");
    const auto failed = !s.telemetry->softwareLossFree() || s.frameOverflows.load() || !s.capture->error().empty() || s.cameraEpoch > 1;
    status(v, !s.mapped ? "UNAVAILABLE" : failed ? "FAIL" : "PASS", !s.mapped ? "No mapped camera frames" : failed ? "Inspect camera errors/epochs" : "Software timestamp mapping observed");
    return v;
}
void run(const Options& o, juce::var& report)
{
    // Placeholder must never open devices or imply LED/pulse evidence exists.
    if (o.has("--physical-events")) { status(report, "UNAVAILABLE", "Physical LED/pulse/loopback event acquisition is a studio release gate; not implemented in round 04"); return; }
    const auto seconds = integer(o.get("--seconds", "60"), 1, 10800);
    if (o.has("--devices") != o.has("--camera")) throw std::invalid_argument("--devices and --camera must be provided together");
    if (o.has("--camera") && o.get("--camera") != "cam1" && o.get("--camera") != "cam2") throw std::invalid_argument("--camera must be cam1 or cam2");
    juce::ScopedJuceInitialiser_GUI juceRuntime;
    std::unique_ptr<juce::AudioIODeviceType> type(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    if (!type) { status(report, "UNAVAILABLE", "ASIO is not compiled"); return; }
    type->scanForDevices(); const auto names = type->getDeviceNames();
    juce::Array<juce::var> devices;
    for (int i = 0; i < names.size(); ++i) { auto item = jsonObject(); jsonSet(item, "index", i); jsonSet(item, "name", names[i]); devices.add(item); }
    jsonSet(report, "asioDevices", devices);
    if (names.isEmpty()) { status(report, "UNAVAILABLE", "No ASIO devices enumerated"); return; }
    if (o.has("--list")) { status(report, "PASS", "Registry enumeration only; no device opened"); return; }
    const auto index = integer(o.get("--asio-device"), 0, names.size() - 1);
    if (!AsioTimingBridge::hookCompiled()) { status(report, "UNAVAILABLE", "Recorder ASIO timing tap is not enabled; apply shared JUCE patch and rebuild"); return; }
    // JUCE's message thread already owns STA/OLE. MfCameraCapture::run owns its
    // own MTA; do not try to change this thread to MTA (RPC_E_CHANGED_MODE).
    // MF startup/shutdown on this control thread covers the camera worker lifetime.
    std::unique_ptr<MfRuntime> mf;
    auto s = std::make_unique<Session>();
    s->device.reset(type->createDevice(names[index], names[index]));
    if (!s->device) { status(report, "UNAVAILABLE", "ASIO createDevice failed"); return; }
    const auto output = integer(o.get("--asio-output", "1"), 1, s->device->getOutputChannelNames().size()) - 1;
    const auto rate = o.has("--sample-rate") ? integer(o.get("--sample-rate"), 8000, 768000) : s->device->getCurrentSampleRate();
    const auto buffer = o.has("--buffer-size") ? integer(o.get("--buffer-size"), 1, 262144) : s->device->getDefaultBufferSize();
    if (!std::isfinite(rate) || rate <= 0 || buffer <= 0) throw std::runtime_error("Driver reported invalid rate/buffer");
    jsonSet(report, "driverName", names[index]); jsonSet(report, "asioDeviceIndex", index); jsonSet(report, "physicalOutputChannel", output + 1);
    jsonSet(report, "requestedSeconds", seconds); jsonSet(report, "requestedSampleRate", rate); jsonSet(report, "requestedBufferSamples", buffer);
    if (!s->bridge.registerTap()) throw std::runtime_error("Another ASIO tap is registered");
    s->startClockWorker(); // keeps consuming while ASIO/MF open blocks the control thread
    juce::BigInteger outputs; outputs.setBit(output);
    const auto error = s->device->open({}, outputs, rate, buffer);
    if (error.isNotEmpty()) { status(report, "UNAVAILABLE", error.toStdString()); return; }
    if (!s->device->getActiveInputChannels().isZero() || s->device->getActiveOutputChannels() != outputs)
        throw std::runtime_error("ASIO changed requested output-only routing");
    s->rate = s->device->getCurrentSampleRate();
    jsonSet(report, "actualSampleRate", s->rate); jsonSet(report, "actualBufferSamples", s->device->getCurrentBufferSizeSamples());
    jsonSet(report, "reportedInputLatencySamples", s->device->getInputLatencyInSamples()); jsonSet(report, "reportedOutputLatencySamples", s->device->getOutputLatencyInSamples());
    s->device->start(&s->output);
    std::string cameraUnavailable;
    if (o.has("--devices"))
    {
        try
        {
            const auto configFile = file(o.get("--devices")); juce::var config;
            if (!configFile.existsAsFile()) throw std::runtime_error("Device selection file missing");
            if (juce::JSON::parse(configFile.loadFileAsString(), config).failed() || static_cast<int>(config["schemaVersion"]) != 1)
                throw std::invalid_argument("Invalid devices.json schema/JSON");
            const auto selected = config["selections"][juce::Identifier(o.get("--camera"))];
            const auto link = selected["symbolicLink"].toString().toStdString();
            if (link.empty()) throw std::runtime_error("Camera selection unavailable; enumerate --select first");
            const auto mode = CameraMode::parse(selected["mode"].toString().toStdString());
            mf = std::make_unique<MfRuntime>(); s->telemetry = std::make_shared<CaptureTelemetry>(mode.fps);
            s->pool = std::make_unique<VideoSurfacePool>(mode.width, mode.height);
            s->camera = std::make_unique<CameraClockMapper>(s->mapper, qpcFrequency(), mode.fps);
            s->capture = std::make_unique<MfCameraCapture>(s->telemetry, *s->pool, [&](const VideoSurface& surface)
            { if (!s->frames.push(surface.stamp)) s->frameOverflows.fetch_add(1, std::memory_order_relaxed); });
            const auto opened = s->capture->start(link, mode, mode.subtype == CaptureSubtype::mjpeg);
            jsonSet(report, "cameraSelection", selected); jsonSet(report, "cameraActualNativeMode", CameraCatalog::modeJson(opened.nativeMode));
        }
        catch (const std::exception& e) { cameraUnavailable = e.what(); }
    }
    // Sync worker: the audio callback only enqueues POD stamps. Optional
    // camera decode worker enqueues original FrameStamp; decode completion time
    // is never substituted for callback/device timestamps.
    const auto start = std::chrono::steady_clock::now(), deadline = start + std::chrono::seconds(seconds);
    auto nextHistory = start; bool interrupted = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { if (message.message == WM_QUIT) interrupted = true; else { TranslateMessage(&message); DispatchMessageW(&message); } }
        s->drain();
        if (interrupted || s->workerFailed.load() || s->output.failed.load() || s->output.stopped.load()) break;
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextHistory)
        {
            if (const auto snapshot = s->mapper.snapshot())
            { if (s->history.size() < 128) s->history.add(clockJson(*snapshot)); else ++s->historyOmitted; }
            nextHistory = now + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto deviceFailed = s->output.failed.load() || s->output.stopped.load();
    jsonSet(report, "elapsedSeconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    s->close();
    const auto snapshot = s->mapper.snapshot(); if (snapshot) jsonSet(report, "clock", clockJson(*snapshot));
    jsonSet(report, "clockHistoryFirst128Seconds", s->history); jsonSet(report, "omittedHistoryEntries", s->historyOmitted);
    juce::Array<juce::var> events;
    for (const auto& event : s->mapper.epochEvents())
    {
        auto item = jsonObject(); jsonSet(item, "epoch", std::to_string(event.epoch)); jsonSet(item, "sequence", std::to_string(event.sequence));
        jsonSet(item, "qpc", std::to_string(event.qpc)); jsonSet(item, "samplePosition", std::to_string(event.samplePosition));
        jsonSet(item, "reasons", reasons(event.reasons)); events.add(item);
    }
    jsonSet(report, "epochEventsLatest128", events); jsonSet(report, "omittedEpochEvents", s->mapper.omittedEpochEvents());
    jsonSet(report, "blocks", s->mapper.blocksObserved()); jsonSet(report, "invalidStamps", s->mapper.invalidStamps());
    jsonSet(report, "droppedStamps", s->bridge.droppedStamps()); jsonSet(report, "stampQueueHighWater", s->bridge.stampHighWater());
    jsonSet(report, "outputCallbacks", s->output.callbacks.load()); jsonSet(report, "outputOnlyClock", true);
    const auto& stats = s->bridge.statistics();
    jsonSet(report, "timeInfoBlocks", stats.timeInfoBlocks); jsonSet(report, "systemTimeValidBlocks", stats.systemTimeBlocks);
    jsonSet(report, "samplePositionValidBlocks", stats.samplePositionBlocks);
    jsonSet(report, "firstSystemTimeRaw", std::to_string(stats.first.systemTimeRaw)); jsonSet(report, "lastSystemTimeRaw", std::to_string(stats.last.systemTimeRaw));
    AsioEventCounters final{}; const bool finalAvailable = AsioTimingBridge::readEvents(*s->device, final);
    auto finalJson = jsonObject(); jsonSet(finalJson, "available", finalAvailable); jsonSet(finalJson, "resets", final.resets);
    jsonSet(finalJson, "resyncs", final.resyncs); jsonSet(finalJson, "xruns", final.xruns); jsonSet(finalJson, "latencyChanges", final.latencyChanges);
    jsonSet(report, "finalDriverEvents", finalJson);
    const bool trailingEvent = finalAvailable && stats.blocks && (final.resets != stats.first.resets || final.xruns != stats.first.xruns
        || final.resyncs != stats.first.resyncs || final.latencyChanges != stats.first.latencyChanges);
    auto cameraReport = cameraJson(*s);
    if (!cameraUnavailable.empty()) status(cameraReport, "UNAVAILABLE", cameraUnavailable);
    jsonSet(report, "camera", cameraReport);
    const bool fail = interrupted || deviceFailed || trailingEvent || s->workerFailed.load() || s->bridge.droppedStamps() || s->mapper.invalidStamps()
        || (snapshot && snapshot->epoch > 1) || (o.has("--camera") && cameraReport["result"].toString() == "FAIL");
    if (fail) status(report, "FAIL", "Observed software timing/queue/device discontinuity; inspect epoch events and counters");
    else if (!snapshot || !snapshot->valid || snapshot->quality.grade == ClockGrade::warmingUp || !s->output.callbacks.load()
        || (o.has("--camera") && cameraReport["result"].toString() == "UNAVAILABLE"))
        status(report, "UNAVAILABLE", "No stable clock or requested camera mapping; inspect component reports/warmup");
    else status(report, "PASS", "Requested software clock observation completed; physical A/V accuracy remains unmeasured");
}
}
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    try
    {
        const auto o = options(argc, argv);
        if (o.has("--help"))
        {
            std::cout << "RecorderClockProbe --asio-device N --seconds 60 --report r04/flex-clock.json [--devices devices.json --camera cam1]\n"
                "  --list (registry only), --sample-rate Fs, --buffer-size N, --asio-output N (one-based, silent)\n"
                "  --physical-events is an UNAVAILABLE placeholder; no device is opened. Device index is zero-based.\n";
            return 0;
        }
        auto report = jsonObject(); juce::Array<juce::var> command;
        for (int i = 0; i < argc; ++i) command.add(juce::String(argv[i]));
        jsonSet(report, "schemaVersion", 1); jsonSet(report, "command", command); jsonSet(report, "sourceKind", "hardware");
        jsonSet(report, "startedUtc", utcNowIso8601()); jsonSet(report, "qpcFrequency", std::to_string(qpcFrequency()));
        jsonSet(report, "juceVersion", juce::SystemStats::getJUCEVersion()); jsonSet(report, "os", juce::SystemStats::getOperatingSystemName());
        jsonSet(report, "hookEnabled", AsioTimingBridge::hookCompiled()); jsonSet(report, "driverVersion", "unavailable through JUCE; attach installed-driver evidence");
        jsonSet(report, "clockDefinition", "8s window, >=5ms spacing, 250ms robust MAD fit; nominal slope for first 5s, then tau=5s/max20ppm/s; phase tau=2s/max0.25ms/s. Callback QPC paired with ASIO position; invalid time-info uses accumulated numSamples. systemTimeRaw units/epoch are not assumed.");
        jsonSet(report, "physicalEvents", "UNAVAILABLE; studio LED/pulse/loopback release gate");
        jsonSet(report, "realtimeScope", "Recorder ASIO tap only enqueues fixed POD; fitting on sole sync worker, JSON on control thread. Existing JUCE callbackLock/outputReady remain outside tap.");
        jsonSet(report, "intervalDefinition", "Capture sample intervals only for consecutive frame IDs in matching master/camera epochs. No resampling/input/output latency applied. Priming during ASIO open/camera setup is included in block counters.");
        try { run(o, report); }
        catch (const std::invalid_argument& e) { status(report, "FAIL", e.what()); }
        catch (const std::exception& e) { status(report, "UNAVAILABLE", e.what()); }
        jsonSet(report, "endedUtc", utcNowIso8601());
        if (o.has("--report")) CaptureTelemetry::writeJson(file(o.get("--report")), report);
        else std::cout << juce::JSON::toString(report, false) << '\n';
        std::cout << report["result"].toString() << ": " << report["reason"].toString() << '\n';
        return report["result"].toString() == "PASS" ? 0 : report["result"].toString() == "UNAVAILABLE" ? 2 : 1;
    }
    catch (const std::exception& e) { std::cerr << "RecorderClockProbe: " << e.what() << '\n'; return 1; }
}
