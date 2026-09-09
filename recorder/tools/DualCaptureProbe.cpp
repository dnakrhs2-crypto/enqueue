#include "DualAudioLoad.h"
#include "FramePatternSource.h"
#include "capture/MfCameraCapture.h"
#include "record/EncodePipeline.h"
#include "record/TakeController.h"
#include "video/PreviewPresenter.h"
#include "video/PresentPacing.h"
#include "support/ThreadPriority.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <mmsystem.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <thread>

using namespace gocue::recorder;
using namespace gocue::recorder::probe;
namespace
{
struct Options
{
    bool synthetic = false, syntheticAudio = false, headroom = false, help = false;
    unsigned fps = 60, seconds = 60, stallMs = 0, contention = 0;
    int asioDevice = 0;
    bool cam2Synthetic = false;
    bool toggleCamera2 = false, headless = false;
    CaptureSubtype formats[2]{CaptureSubtype::nv12, CaptureSubtype::mjpeg};
    std::string devices, report;
};
unsigned integer(const std::string& text, unsigned minimum, unsigned maximum)
{
    unsigned n = 0; const auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || n < minimum || n > maximum) throw std::invalid_argument("Invalid integer: " + text);
    return n;
}
Options parse(int argc, wchar_t** argv)
{
    Options o; std::set<std::string> seen;
    const std::set<std::string> flags{"--synthetic", "--synthetic-audio", "--headroom", "--help", "--toggle-camera2", "--headless"};
    const std::set<std::string> values{"--devices", "--cam2", "--project-fps", "--seconds", "--stall-ms", "--cpu-contention", "--report", "--asio-device", "--cam1-format", "--cam2-format"};
    for (int i = 1; i < argc; ++i)
    {
        const auto key = juce::String(argv[i]).toStdString();
        if (!seen.insert(key).second) throw std::invalid_argument("Duplicate option: " + key);
        if (flags.count(key))
        { if (key == "--synthetic") o.synthetic = true; if (key == "--synthetic-audio") o.syntheticAudio = true; if (key == "--headroom") o.headroom = true; if (key == "--help") o.help = true;
          if (key == "--toggle-camera2") o.toggleCamera2 = true; if (key == "--headless") o.headless = true; continue; }
        if (!values.count(key) || ++i == argc) throw std::invalid_argument("Unknown/incomplete option: " + key);
        const auto value = juce::String(argv[i]).toStdString();
        if (key == "--devices") o.devices = value;
        else if (key == "--report") o.report = value;
        else if (key == "--cam2") { if (value != "synthetic") throw std::invalid_argument("--cam2 only accepts synthetic"); o.cam2Synthetic = true; }
        else if (key == "--project-fps") { o.fps = integer(value, 30, 60); if (o.fps != 30 && o.fps != 60) throw std::invalid_argument("Project FPS must be 30 or 60"); }
        else if (key == "--seconds") o.seconds = integer(value, 1, 3600);
        else if (key == "--stall-ms") o.stallMs = integer(value, 0, 30000);
        else if (key == "--cpu-contention") o.contention = integer(value, 0, 256);
        else if (key == "--asio-device") o.asioDevice = static_cast<int>(integer(value, 0, 1024));
        else
        {
            if (value != "nv12" && value != "mjpeg") throw std::invalid_argument("Synthetic format must be nv12 or mjpeg");
            o.formats[key == "--cam1-format" ? 0 : 1] = value == "nv12" ? CaptureSubtype::nv12 : CaptureSubtype::mjpeg;
        }
    }
    if (o.help) return o;
    if (o.headless && !o.toggleCamera2) throw std::invalid_argument("--headless requires --toggle-camera2 (product lifecycle diagnostics)");
    if (o.toggleCamera2 && (o.seconds < 8 || o.headroom || o.stallMs || o.contention || seen.count("--asio-device")))
        throw std::invalid_argument("--toggle-camera2 requires >=8 seconds; uses the product take path and synthetic PCM clock; use the round-05 path for ASIO/stall/contention");
    if (o.report.empty()) throw std::invalid_argument("--report FILE is required; media is saved beside the report in a unique take directory");
    if (o.headroom)
    {
        if (!o.devices.empty() || o.cam2Synthetic || o.syntheticAudio || o.stallMs || seen.count("--asio-device")
            || seen.count("--cam1-format") || seen.count("--cam2-format")) throw std::invalid_argument("--headroom is encode-only; camera/audio/stall options do not apply");
    }
    else
    {
        if (o.synthetic == !o.devices.empty()) throw std::invalid_argument("Choose --synthetic or --devices FILE");
        if (o.cam2Synthetic && o.devices.empty()) throw std::invalid_argument("--cam2 synthetic requires --devices");
        if (seen.count("--cam1-format") && !o.synthetic) throw std::invalid_argument("--cam1-format requires --synthetic");
        if (seen.count("--cam2-format") && !o.synthetic && !o.cam2Synthetic) throw std::invalid_argument("--cam2-format requires a synthetic cam2");
        if (o.stallMs && o.seconds < 4) throw std::invalid_argument("Writer stall runs require at least 4 seconds");
    }
    return o;
}
juce::File file(const std::string& path) { return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(path.c_str())); }
void status(juce::var& v, const char* result, const std::string& reason)
{
    jsonSet(v, "result", result); jsonSet(v, "reason", reason);
    jsonSet(v, "status", std::string(result) == "PASS" ? "available" : std::string(result) == "UNAVAILABLE" ? "unavailable" : "failed");
}
std::uint64_t cpuTicks()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) throw std::runtime_error("GetProcessTimes failed");
    const auto ticks = [](FILETIME f) { return (std::uint64_t{f.dwHighDateTime} << 32) | f.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
class Contention
{
public:
    explicit Contention(unsigned n)
    {
        try { for (unsigned i = 0; i < n; ++i) workers.emplace_back([this, i]
        {
            ScopedRecorderPriority priority(RecorderThreadRole::encodeWrite);
            std::uint64_t value = i + 1;
            while (!stop.load(std::memory_order_relaxed))
            { for (unsigned k = 0; k < 65536; ++k) value = value * 6364136223846793005ULL + 1442695040888963407ULL; checksum.fetch_xor(value, std::memory_order_relaxed); }
        }); }
        catch (...) { finish(); throw; }
    }
    ~Contention() { finish(); }
    void finish() { stop = true; for (auto& t : workers) if (t.joinable()) t.join(); }
private:
    std::atomic<bool> stop{false}; std::atomic<std::uint64_t> checksum{0}; std::vector<std::thread> workers;
};
constexpr UINT closeMessage = WM_APP + 51;
LRESULT CALLBACK windowProc(HWND w, UINT m, WPARAM a, LPARAM b)
{ if (m == WM_CLOSE) { PostMessageW(w, closeMessage, 0, 0); return 0; } return DefWindowProcW(w, m, a, b); }
struct DualWindow
{
    HWND handle = nullptr; std::array<HWND, 2> views{}; bool cancelled = false;
    DualWindow()
    {
        WNDCLASSW cls{}; cls.lpfnWndProc = windowProc; cls.hInstance = GetModuleHandleW(nullptr); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"RecorderDualProbePreview";
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register dual window failed");
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        RECT rect{0, 0, 1280, 360}; AdjustWindowRect(&rect, style, FALSE);
        handle = CreateWindowExW(0, cls.lpszClassName, L"RecorderDualProbe - preparing cam1 | cam2", style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, cls.hInstance, nullptr);
        if (!handle) throw std::runtime_error("UNAVAILABLE: Create dual preview window failed");
        for (unsigned i = 0; i < 2; ++i)
        {
            views[i] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, static_cast<int>(i) * 640, 0, 640, 360, handle, nullptr, cls.hInstance, nullptr);
            if (!views[i]) { DestroyWindow(handle); handle = nullptr; throw std::runtime_error("Create preview child failed"); }
        }
        ShowWindow(handle, SW_SHOW); UpdateWindow(handle);
    }
    ~DualWindow() { if (handle) DestroyWindow(handle); }
    void pump()
    {
        MSG m{}; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        { if (m.message == closeMessage || m.message == WM_QUIT) cancelled = true; else { TranslateMessage(&m); DispatchMessageW(&m); } }
    }
    void title(const std::string& text) { SetWindowTextW(handle, juce::String::fromUTF8(text.c_str()).toWideCharPointer()); }
};
template<class Work> auto pumped(DualWindow& window, Work work)
{
    auto future = std::async(std::launch::async, std::move(work));
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    { window.pump(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT); }
    return future.get();
}
class SyntheticTimeMapper final : public CameraTimeMapper
{
public:
    explicit SyntheticTimeMapper(const std::atomic<std::int64_t>& origin) : origin(origin), frequency(qpcFrequency()) {}
    std::int64_t map(const FrameStamp& stamp) override { return stamp.pts100ns; }
    std::int64_t now(std::int64_t at) const override
    {
        const auto zero = origin.load(); if (!zero || at <= zero) return 0;
        const auto elapsed = at - zero; return elapsed / frequency * 10000000 + elapsed % frequency * 10000000 / frequency;
    }
private:
    const std::atomic<std::int64_t>& origin; std::int64_t frequency;
};
struct Camera
{
    unsigned id;
    bool synthetic;
    CameraMode mode;
    std::string link;
    juce::File output;
    WriterStall stall;
    std::shared_ptr<CaptureTelemetry> telemetry;
    std::unique_ptr<VideoSurfacePool> pool;
    std::unique_ptr<EncodePipeline> encode;
    std::unique_ptr<PreviewPresenter> presenter;
    std::unique_ptr<MfCameraCapture> capture;
    std::unique_ptr<FramePatternSource> pattern;
    juce::var sourceInfo = jsonObject();
    bool failureDisplayed = false;
    Camera(unsigned id, bool synthetic, unsigned stallMs) : id(id), synthetic(synthetic), stall(stallMs) {}
    void stopCapture() { if (pattern) pattern->stop(); if (capture) capture->stop(); }
    void finish() { if (presenter) presenter->stop(); if (encode) encode->stop(); }
};
void headroom(const Options& o, juce::var& report)
{
    struct Lane { std::unique_ptr<NvencEncoder> encoder; std::unique_ptr<NvencFramePool> bank; std::array<int, 15> slots{}; std::uint64_t frames = 0, bytes = 0; juce::var stats; std::string error; };
    std::array<Lane, 2> lanes;
    std::promise<void> go; auto gate = go.get_future().share();
    std::atomic<std::int64_t> origin{0}; std::atomic<bool> abort{false};
    std::array<std::future<void>, 2> ready;
    std::array<std::thread, 2> workers;
    auto join = [&] { for (auto& worker : workers) if (worker.joinable()) worker.join(); };
    try
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            std::promise<void> prepared; ready[i] = prepared.get_future();
            workers[i] = std::thread([&, i, prepared = std::move(prepared)]() mutable
            {
                auto& lane = lanes[i]; bool announced = false;
                ScopedRecorderPriority priority(RecorderThreadRole::encodeWrite);
                try
                {
                    NvencProfile profile; profile.fps = static_cast<int>(o.fps);
                    lane.encoder = std::make_unique<NvencEncoder>(profile); lane.encoder->open();
                    lane.bank = std::make_unique<NvencFramePool>(profile.cpuSurfaces()); VideoSurface surface; surface.prepare(1920, 1080);
                    for (int frame = 0; frame < lane.bank->capacity(); ++frame)
                    { paintPattern(surface, {static_cast<std::uint32_t>(frame + 1), i + 1}); if (!lane.bank->copy(surface) || !lane.bank->pop(lane.slots[frame])) throw std::runtime_error("Headroom bank preparation failed"); }
                    prepared.set_value(); announced = true; gate.wait(); if (abort) return;
                    const auto end = origin.load() + static_cast<std::int64_t>(o.seconds) * qpcFrequency();
                    const PacketSink sink = [&](const AVPacket& packet) { lane.bytes += packet.size; };
                    while (!abort && qpcNow() < end)
                    { lane.encoder->submit(lane.bank->frame(lane.slots[lane.frames % lane.bank->capacity()]), static_cast<std::int64_t>(lane.frames), sink); ++lane.frames; }
                    lane.encoder->drain(sink); lane.stats = lane.encoder->toJson(); jsonSet(lane.stats, "priorityError", priority.error);
                }
                catch (const std::exception& e) { lane.error = e.what(); abort = true; if (!announced) prepared.set_exception(std::current_exception()); }
            });
        }
        for (auto& r : ready) r.get();
    }
    catch (const std::exception& e)
    { abort = true; go.set_value(); join(); status(report, "UNAVAILABLE", e.what()); return; }
    std::unique_ptr<Contention> contention;
    std::uint64_t cpu = 0;
    try { contention = std::make_unique<Contention>(o.contention); cpu = cpuTicks(); }
    catch (...) { abort = true; go.set_value(); join(); throw; }
    origin = qpcNow(); go.set_value(); join();
    const auto elapsed = static_cast<double>(qpcNow() - origin.load()) / qpcFrequency();
    const auto cpuEnd = cpuTicks(); contention->finish();
    juce::Array<juce::var> encoders;
    std::uint64_t frames = 0;
    for (unsigned i = 0; i < 2; ++i)
    {
        auto v = lanes[i].stats; if (!v.isObject()) v = jsonObject();
        jsonSet(v, "pipelineId", i == 0 ? "cam1" : "cam2"); jsonSet(v, "framesMeasured", lanes[i].frames);
        jsonSet(v, "fpsOverCommonWallTime", lanes[i].frames / elapsed); jsonSet(v, "bytesDiscarded", lanes[i].bytes); jsonSet(v, "error", lanes[i].error);
        encoders.add(v); frames += lanes[i].frames;
    }
    const double fps = frames / elapsed;
    jsonSet(report, "encoders", encoders); jsonSet(report, "secondsMeasured", elapsed); jsonSet(report, "combinedEncodeFps", fps);
    jsonSet(report, "goalFps", 156); jsonSet(report, "goalMet", fps >= 156 && !abort); jsonSet(report, "headroomRatioTo120Fps", fps / 120);
    jsonSet(report, "processCpuPercentOneCore", (cpuEnd - cpu) / (elapsed * 100000));
    jsonSet(report, "definition", "Two concurrent independent h264_nvenc P5 sessions, prepared changing 1080p NV12 banks. Sum of frames divided by common wall time including both drains. No capture/preview/CFR/PCM/AAC/mux/disk; content-specific, not the simultaneous-load gate.");
    status(report, !abort && fps >= 156 ? "PASS" : "FAIL", abort ? "An encoder failed; inspect both lane reports" : "Two-session synthetic encode headroom measured");
}
// Round 24 exercises the same take controller as the product. Audio is an
// explicit synthetic native PCM fixture; no ASIO/USB/optical certification.
class ProductAudioFeed
{
public:
    explicit ProductAudioFeed(RecorderAudioEngine& a) : audio(a)
    {
        worker = std::thread([this]
        {
            ScopedRecorderPriority priority(RecorderThreadRole::capturePreview);
            constexpr unsigned frames = 480;
            const auto origin = qpcNow(); const auto start = std::chrono::steady_clock::now();
            std::int64_t sample = 0; std::uint64_t sequence = 0;
            std::array<std::array<std::uint8_t, frames * 3>, 8> pcm{};
            std::array<NativeInputView, 8> views{}; std::array<float, frames> input{}, left{}, right{};
            std::array<const float*, 8> inputs{}; float* outputs[]{left.data(), right.data()};
            for (unsigned i = 0; i < 8; ++i) { views[i] = {pcm[i].data(), int(i), int(i), nativeFormatForAsio(17)}; inputs[i] = input.data(); }
            while (!stop.load())
            {
                for (unsigned c = 0; c < 8; ++c) for (unsigned i = 0; i < frames; ++i)
                    WavTrackWriter::packPcm24(int((sample + i) % 480) * (int(c) + 1) * 100, pcm[c].data() + i * 3);
                BlockStamp stamp{}; stamp.flags = samplePositionValid; stamp.sequence = sequence++; stamp.samplePosition = sample;
                stamp.sampleRate = 48000; stamp.numSamples = frames; stamp.callbackQpc = origin + sample * qpcFrequency() / 48000;
                audio.processBlock(stamp, views.data(), 8, inputs.data(), outputs, 2); sample += frames;
                std::this_thread::sleep_until(start + std::chrono::nanoseconds(sample * 1000000000LL / 48000));
            }
        });
    }
    ~ProductAudioFeed() { stop = true; if (worker.joinable()) worker.join(); }
private:
    RecorderAudioEngine& audio;
    std::atomic<bool> stop{false};
    std::thread worker;
};
void productDualLoad(const Options& o, juce::var& report)
{
    const auto check = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
    const auto ok = [](const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); };
    juce::var selected;
    if (!o.devices.empty())
    {
        if (!file(o.devices).existsAsFile()) throw std::invalid_argument("devices.json is absent");
        ok(juce::JSON::parse(file(o.devices).loadFileAsString(), selected));
        if (int(selected["schemaVersion"]) != 1) throw std::invalid_argument("Invalid devices.json schemaVersion");
    }
    UserSettings settings; settings.cameraEnabled = {true, false};
    std::array<CameraMode, 2> modes;
    std::array<bool, 2> synthetic{o.synthetic, o.synthetic || o.cam2Synthetic};
    std::vector<CameraDevice> devices;
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto name = i ? "cam2" : "cam1";
        if (synthetic[i])
        {
            settings.cameraDeviceIds[i] = juce::String("synthetic:") + name;
            modes[i] = CameraMode::parse(std::string(subtypeName(o.formats[i])) + " 1920x1080 " + (i ? "30/1" : "60/1"));
        }
        else
        {
            const auto selection = selected["selections"][juce::Identifier(name)];
            settings.cameraDeviceIds[i] = selection["symbolicLink"].toString();
            if (settings.cameraDeviceIds[i].isEmpty()) throw std::invalid_argument(std::string("devices.json has no ") + name + " symbolicLink");
            modes[i] = CameraMode::parse(selection["mode"].toString().toStdString());
        }
        settings.cameraModes[i] = modes[i].text();
        CameraDevice d; d.symbolicLink = settings.cameraDeviceIds[i].toStdString(); d.modes.push_back(modes[i]); devices.push_back(d);
    }
    CameraCatalog catalog; ok(catalog.configure(settings)); catalog.refresh(devices);
    const auto directory = file(o.report).getParentDirectory().getChildFile("product-dual-" + juce::Uuid().toString()); ok(directory.createDirectory());
    jsonSet(report, "mediaDirectory", directory.getFullPathName()); jsonSet(report, "pipeline", "product TakeController / MfCameraCapture or FramePatternSource");
    jsonSet(report, "audioSource", "synthetic native PCM24, 8 channels, 48000 Hz; product RecorderAudioEngine/WAV/AAC");
    jsonSet(report, "previewMeasurement", o.headless ? "CPU latest mailbox consumption; D3D/optical latency UNAVAILABLE" : "Independent D3D11 presenters; optical latency UNAVAILABLE");
    RecorderDocument document; document.newProject("Round 24 dual preview", 48000, {o.fps, 1});
    RecorderAudioEngine audio; std::array<int, 8> inputs{0,1,2,3,4,5,6,7}; ok(audio.setInputMap(inputs));
    ok(audio.openSynthetic(48000, 480, 8, 2)); for (unsigned i = 0; i < 8; ++i) ok(audio.arm(i, true));
    ProductAudioFeed feed(audio); TakeController take(document, audio); MfRuntime mf;
    std::unique_ptr<DualWindow> window; if (!o.headless) window = std::make_unique<DualWindow>();
    std::array<std::unique_ptr<Camera>, 2> cameras;
    std::array<std::atomic<std::int64_t>, 2> sourceOrigins{};
    std::array<unsigned, 2> sourceStarts{};
    juce::Array<juce::var> events, takes, queueTrace, sourceReports;
    bool started = false; double nextTrace = 0; const auto wallStart = qpcNow();
    auto event = [&](const char* text)
    { auto e = jsonObject(); jsonSet(e, "event", text); jsonSet(e, "sample", audio.currentSample()); jsonSet(e, "cam2Generation", jsonInt(catalog.slot(1).generation)); events.add(e); };
    auto service = [&]
    {
        if (window) { window->pump(); check(!window->cancelled, "User cancelled dual-preview experiment"); }
        else for (auto& c : cameras) if (c && c->pool) c->pool->uploadLatest([](const VideoSurface&) {});
        take.tick();
        const auto elapsed = double(qpcNow() - wallStart) / qpcFrequency();
        if (elapsed >= nextTrace)
        {
            auto row = jsonObject(); jsonSet(row, "seconds", elapsed);
            for (unsigned i = 0; i < 2; ++i)
            {
                const auto q = take.cameraQueues(i); auto v = jsonObject();
                jsonSet(v, "active", take.cameraActive(i)); jsonSet(v, "surfaceOccupancy", q.surfaces); jsonSet(v, "surfaceCapacity", q.surfaceCapacity);
                jsonSet(v, "surfaceHighWater", q.surfaceHighWater); jsonSet(v, "surfaceOverflow", jsonInt(q.surfaceOverflow));
                jsonSet(v, "videoPacketCount", jsonInt(q.videoPackets)); jsonSet(v, "videoPacketBytes", jsonInt(q.videoBytes)); jsonSet(v, "audioPacketCount", jsonInt(q.audioPackets));
                if (cameras[i])
                {
                    const auto& c = *cameras[i]; const auto preview = c.pool->snapshot();
                    jsonSet(v, "sampleQueueHighWater", jsonInt(c.telemetry->queueHighWater.load()));
                    jsonSet(v, "latestPreviewId", jsonInt(c.telemetry->latestReadyFrame.load()));
                    jsonSet(v, "previewPublished", jsonInt(preview.published)); jsonSet(v, "previewConsumed", jsonInt(preview.consumed));
                    jsonSet(v, "previewOverwritten", jsonInt(preview.overwritten)); jsonSet(v, "previewExhausted", jsonInt(preview.exhausted));
                }
                jsonSet(row, i ? "cam2" : "cam1", v);
            }
            if (queueTrace.size() == 512) queueTrace.remove(0); queueTrace.add(row); nextTrace = elapsed + .25;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    auto waitUntil = [&](auto predicate, double timeout)
    { const auto deadline = qpcNow() + std::int64_t(timeout * qpcFrequency()); while (!predicate()) { check(qpcNow() < deadline, "Product dual-load deadline exceeded"); service(); } };
    auto background = [&](auto task)
    {
        auto work = std::async(std::launch::async, task);
        while (work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) service();
        work.get();
    };
    auto closeCamera = [&](unsigned i)
    {
        if (!cameras[i]) return;
        auto& c = *cameras[i]; background([&] { c.stopCapture(); c.finish(); });
        auto v = jsonObject(); jsonSet(v, "slot", int(i + 1)); jsonSet(v, "sourceKind", c.synthetic ? "synthetic" : "hardware");
        jsonSet(v, "telemetry", c.telemetry->toJson()); jsonSet(v, "source", c.pattern ? c.pattern->toJson() : c.sourceInfo);
        if (c.presenter) jsonSet(v, "adapter", c.presenter->adapterJson());
        const auto p = c.pool->snapshot(); jsonSet(v, "previewPublished", jsonInt(p.published)); jsonSet(v, "previewConsumed", jsonInt(p.consumed));
        jsonSet(v, "previewExhausted", jsonInt(p.exhausted));
        jsonSet(v, "softwareLossFree", c.telemetry->softwareLossFree());
        jsonSet(v, "sourceFailed", c.pattern ? c.pattern->failed() : c.capture && !c.capture->error().empty());
        sourceReports.add(v); cameras[i].reset();
    };
    auto openCamera = [&](unsigned i)
    {
        const auto slot = catalog.slot(i); check(slot.ready(), "Camera slot has no selected native mode");
        auto c = std::make_unique<Camera>(i + 1, synthetic[i], 0); c->mode = *slot.mode; c->link = slot.symbolicLink;
        c->telemetry = std::make_shared<CaptureTelemetry>(c->mode.fps, i ? "cam2" : "cam1"); c->pool = std::make_unique<VideoSurfacePool>(1920, 1080);
        cameras[i] = std::move(c); auto& cam = *cameras[i];
        std::promise<void> go; auto gate = go.get_future().share();
        try
        {
            const auto sink = [&, i](const VideoSurface& f) { take.offer(i, f); };
            if (window)
            {
                cam.presenter = std::make_unique<PreviewPresenter>(window->views[i], *cam.pool, cam.telemetry, 1920, 1080);
                background([&] { cam.presenter->start(gate); });
            }
            if (cam.synthetic)
            {
                FramePatternSource::Config config{i + 1, cam.mode.fps.numerator, std::min(3600u, o.seconds + 120), cam.mode.subtype}; config.generation = slot.generation;
                cam.pattern = std::make_unique<FramePatternSource>(config, *cam.pool, cam.telemetry, sink);
                background([&] { cam.pattern->start(gate, sourceOrigins[i]); });
            }
            else
            {
                cam.capture = std::make_unique<MfCameraCapture>(cam.telemetry, *cam.pool, sink);
                background([&] { const auto opened = cam.capture->start(cam.link, cam.mode, cam.mode.subtype == CaptureSubtype::mjpeg, 1, gate, slot.generation);
                    jsonSet(cam.sourceInfo, "nativeMode", CameraCatalog::modeJson(opened.nativeMode)); });
            }
            sourceOrigins[i] = qpcNow(); go.set_value(); ++sourceStarts[i];
        }
        catch (...) { go.set_value(); throw; }
    };
    try
    {
        openCamera(0); waitUntil([&] { return audio.clockReady(); }, 10);
        const unsigned unit = o.seconds / 4;
        for (unsigned phase = 0; phase < 4; ++phase)
        {
            const bool enabled = phase == 1 || phase == 3;
            settings.cameraEnabled[1] = enabled; ok(catalog.configure(settings)); catalog.refresh(devices);
            if (enabled) openCamera(1); else closeCamera(1);
            event(enabled ? (phase == 3 ? "cam2-reconnected" : "cam2-on") : "cam2-off");
            TakeController::Config config; config.projectDirectory = directory; config.cameraMode = modes[0]; config.cameraSymbolicLink = settings.cameraDeviceIds[0].toStdString();
            config.synthetic = synthetic[0]; config.projectFps = int(o.fps); config.externalCapture = true; config.cameraGeneration = catalog.slot(0).generation;
            config.camera2.enabled = enabled; config.camera2.synthetic = synthetic[1]; config.camera2.mode = modes[1];
            config.camera2.symbolicLink = settings.cameraDeviceIds[1].toStdString(); config.camera2.generation = catalog.slot(1).generation;
            ok(take.prepare(config)); waitUntil([&] { return take.state() == TakeController::State::armed || take.state() == TakeController::State::partialFailure; }, 15);
            check(take.state() == TakeController::State::armed, "Product take preparation failed"); ok(take.start());
            waitUntil([&] { return take.state() == TakeController::State::recording; }, 5); started = true;
            const auto n0 = take.scheduledStart(), length = std::int64_t(phase == 3 ? o.seconds - unit * 3 : unit) * 48000;
            ok(take.stop(n0 + length)); bool disconnected = false, disabledRequested = false;
            std::uint64_t cam1AtDisconnect = 0, cam1BeforeToggle = cameras[0]->telemetry->latestReadyFrame.load();
            const auto deadline = qpcNow() + (length / 48000 + 20) * qpcFrequency();
            while (take.state() != TakeController::State::done && take.state() != TakeController::State::partialFailure)
            {
                check(qpcNow() < deadline, "Product take did not finish");
                if (phase == 1 && !disabledRequested && audio.currentSample() >= n0 + length / 4)
                {
                    settings.cameraEnabled[1] = false; disabledRequested = true; event("cam2-disable-requested-for-next-take");
                    check(take.cameraActive(1), "Cam2 disabled prematurely in the active take");
                }
                if (phase == 1 && !disconnected && audio.currentSample() >= n0 + length / 2)
                {
                    const auto generation = catalog.slot(1).generation;
                    take.cameraFailed(1, generation); catalog.disconnected(1);
                    disconnected = true; cam1AtDisconnect = cameras[0]->telemetry->latestReadyFrame.load(); event("cam2-disconnected");
                    background([&] { cameras[1]->stopCapture(); });
                }
                service();
            }
            auto result = take.report(); jsonSet(result, "phase", int(phase)); takes.add(result);
            check(take.logicalLength() == length, "Take logical length differs from common Nstop-N0");
            const auto& project = document.getProject(); const auto* t = project.media->findTake(config.takeId.toString()); check(t != nullptr, "Take was not placed");
            const auto* first = project.media->findAsset(t->cam1AssetId); check(first && first->gaps.empty(), "Healthy cam1 was truncated or failed");
            for (const auto& id : t->microphoneAssetIds) check(project.media->findAsset(id)->gaps.empty(), "Raw WAV was truncated");
            check(cameras[0]->telemetry->latestReadyFrame.load() > cam1BeforeToggle, "Cam1 preview stopped during cam2 toggle");
            if (phase == 1)
            {
                const auto* second = project.media->findAsset(t->cam2AssetId);
                check(disabledRequested && disconnected && second && !second->gaps.empty(), "Cam2 disconnect did not produce an explicit gap");
                check(cameras[0]->telemetry->latestReadyFrame.load() > cam1AtDisconnect, "Cam1 preview stopped after cam2 disconnect");
                check(take.cameraDisconnected(1), "Cam2 disconnect state was lost"); closeCamera(1);
            }
            else check(take.state() == TakeController::State::done, "Unexpected partial failure");
            if (!enabled)
            {
                const auto folder = directory.getChildFile("media/takes/" + config.takeId.toDashedString());
                check(t->cam2AssetId.isEmpty() && !cameras[1] && !folder.getChildFile("cam2.mp4").exists() && !folder.getChildFile("cam2.recording.mp4").exists(), "OFF cam2 created a source/file/asset");
            }
        }
        closeCamera(1); closeCamera(0);
        for (const auto& r : sourceReports)
            check(bool(r["softwareLossFree"]) && !bool(r["sourceFailed"]) && std::int64_t(r["previewPublished"]) > 0 && std::int64_t(r["previewConsumed"]) > 0 && std::int64_t(r["previewExhausted"]) == 0, "Preview/source loss or surface starvation; inspect source reports");
        status(report, "PASS", "Product OFF/ON/deferred-disable/disconnect/reconnect, common take boundaries and independent preview/queues observed");
    }
    catch (const std::exception& e)
    {
        status(report, started ? "FAIL" : "UNAVAILABLE", e.what());
        for (unsigned i = 0; i < 2; ++i) if (cameras[i])
        { cameras[i]->stopCapture(); cameras[i]->finish(); cameras[i].reset(); }
    }
    jsonSet(report, "events", events); jsonSet(report, "takes", takes); jsonSet(report, "queueTimeline", queueTrace); jsonSet(report, "sources", sourceReports);
    jsonSet(report, "cam1SourceStarts", int(sourceStarts[0])); jsonSet(report, "cam2SourceStarts", int(sourceStarts[1]));
    jsonSet(report, "secondsMeasured", double(qpcNow() - wallStart) / qpcFrequency());
    jsonSet(report, "isolationDefinition", "Real per-camera queue/pool counters sampled about every 250ms (last 512); cam1 source remains open across four takes; source+encoder+asset absent while cam2 OFF. Saturated encoder-pool isolation is separately verified by camera-slots; no injected encoder stall here.");
}
void dualLoad(const Options& o, juce::var& report)
{
    const auto directory = file(o.report).getParentDirectory().getChildFile("dual-" + juce::Uuid().toString());
    const auto created = directory.createDirectory(); if (created.failed()) throw std::runtime_error(created.getErrorMessage().toStdString());
    jsonSet(report, "mediaDirectory", directory.getFullPathName());
    juce::var devices;
    if (!o.devices.empty())
    {
        if (!file(o.devices).existsAsFile()) throw std::invalid_argument("devices.json is absent");
        const auto parsed = juce::JSON::parse(file(o.devices).loadFileAsString(), devices);
        if (parsed.failed()) throw std::invalid_argument(parsed.getErrorMessage().toStdString());
    }
    std::atomic<std::int64_t> origin{0}; TakeStopSignal stop;
    WriterStall audioStall(o.stallMs);
    DualAudioLoad audio({o.syntheticAudio, o.asioDevice, o.seconds, directory, &audioStall}, stop, origin);
    std::array<std::unique_ptr<Camera>, 2> cameras;
    for (unsigned i = 0; i < 2; ++i)
    {
        cameras[i] = std::make_unique<Camera>(i + 1, o.synthetic || (i == 1 && o.cam2Synthetic), o.stallMs);
        auto& camera = *cameras[i]; const std::string name = i == 0 ? "cam1" : "cam2";
        camera.output = directory.getChildFile(name + ".mp4");
        if (camera.synthetic)
        {
            camera.mode.width = 1920; camera.mode.height = 1080; camera.mode.fps = {i == 0 ? 60u : 30u, 1}; camera.mode.subtype = o.formats[i];
        }
        else
        {
            const auto selected = devices["selections"][juce::Identifier(name)];
            camera.link = selected["symbolicLink"].toString().toStdString();
            if (camera.link.empty()) throw std::invalid_argument("devices.json has no " + name + " symbolicLink");
            camera.mode = CameraMode::parse(selected["mode"].toString().toStdString()); camera.sourceInfo = selected;
            if (camera.mode.width != 1920 || camera.mode.height != 1080 || camera.mode.fps.value() > 60.1 || camera.mode.fps.value() < 29)
                throw std::invalid_argument("Dual probe requires native 1080p30/60 (rational modes retained)");
        }
        camera.telemetry = std::make_shared<CaptureTelemetry>(camera.mode.fps, name);
        camera.pool = std::make_unique<VideoSurfacePool>(1920, 1080);
        NvencProfile profile; profile.fps = static_cast<int>(o.fps);
        std::unique_ptr<CameraTimeMapper> mapper;
        if (camera.synthetic) mapper = std::make_unique<SyntheticTimeMapper>(origin);
        camera.encode = std::make_unique<EncodePipeline>(profile, camera.mode.fps, o.seconds, camera.output, camera.telemetry, std::move(mapper), &camera.stall);
    }
    if (!cameras[0]->synthetic && !cameras[1]->synthetic && cameras[0]->link == cameras[1]->link) throw std::invalid_argument("cam1 and cam2 must be distinct physical devices");
    MfRuntime mf; DualWindow window;
    std::promise<void> go; auto gate = go.get_future().share(); bool released = false, started = false, durationComplete = false;
    std::string failure; std::unique_ptr<Contention> contention;
    juce::Array<juce::var> queueTrace;
    std::uint64_t queueSamples = 0;
    double nextQueueSample = 0;
    std::int64_t ended = 0; std::uint64_t cpu = 0, cpuEnd = 0;
    try
    {
        audio.prepare();
        for (unsigned i = 0; i < 2; ++i)
        {
            auto& camera = *cameras[i];
            pumped(window, [&] { camera.encode->start(); });
            camera.presenter = std::make_unique<PreviewPresenter>(window.views[i], *camera.pool, camera.telemetry, 1920, 1080);
            pumped(window, [&] { camera.presenter->start(gate); });
            const auto sink = [&camera](const VideoSurface& frame) { camera.encode->offer(frame); };
            if (camera.synthetic)
            {
                camera.pattern = std::make_unique<FramePatternSource>(FramePatternSource::Config{camera.id, camera.mode.fps.numerator, o.seconds, o.formats[i]}, *camera.pool, camera.telemetry, sink);
                pumped(window, [&] { camera.pattern->start(gate, origin); });
            }
            else
            {
                camera.capture = std::make_unique<MfCameraCapture>(camera.telemetry, *camera.pool, sink);
                const auto opened = pumped(window, [&] { return camera.capture->start(camera.link, camera.mode, camera.mode.subtype == CaptureSubtype::mjpeg, 1, gate); });
                jsonSet(camera.sourceInfo, "nativeMode", CameraCatalog::modeJson(opened.nativeMode));
                jsonSet(camera.sourceInfo, "outputMode", CameraCatalog::modeJson(opened.outputMode)); jsonSet(camera.sourceInfo, "decoder", opened.decoderName);
            }
            if (window.cancelled) throw std::runtime_error("User cancelled preparation");
        }
        for (auto& camera : cameras) camera->telemetry->reset();
        contention = std::make_unique<Contention>(o.contention);
        cpu = cpuTicks(); origin = qpcNow(); started = true;
        const auto stallAt = origin.load() + static_cast<std::int64_t>(std::min(5u, o.seconds / 2)) * qpcFrequency();
        for (auto& camera : cameras) camera->stall.arm(stallAt); audioStall.arm(stallAt);
        go.set_value(); released = true;
        window.title("RecorderDualProbe - cam1 recording | cam2 recording | 8ch PCM24");
        for (;;)
        {
            window.pump(); const auto elapsed = static_cast<double>(qpcNow() - origin.load()) / qpcFrequency();
            if (elapsed >= nextQueueSample)
            {
                auto sample = jsonObject(); jsonSet(sample, "seconds", elapsed); jsonSet(sample, "wavQueueFrames", audio.queuedWavFrames());
                for (unsigned i = 0; i < 2; ++i)
                {
                    const auto queues = cameras[i]->encode->queueSnapshot(); auto lane = jsonObject();
                    jsonSet(lane, "surfaceOccupancy", queues.surfaces); jsonSet(lane, "packetCount", queues.packets); jsonSet(lane, "packetBytes", queues.packetBytes);
                    jsonSet(lane, "latestPreviewReadyId", cameras[i]->telemetry->latestReadyFrame.load());
                    jsonSet(lane, "sourceFramesDecoded", cameras[i]->telemetry->decoded.load()); jsonSet(sample, i == 0 ? "cam1" : "cam2", lane);
                }
                if (queueTrace.size() == 128) queueTrace.remove(0);
                queueTrace.add(sample); ++queueSamples; nextQueueSample = elapsed + 1;
            }
            if (window.cancelled) { failure = "User closed preview before completion"; break; }
            if (stop.requested()) { failure = "Audio original threatened; entire take capture stopped"; window.title("RecorderDualProbe - TAKE STOPPED: audio original error"); break; }
            for (auto& camera : cameras)
            {
                const bool bad = camera->encode->failed() || !camera->telemetry->softwareLossFree() || camera->presenter->finished()
                    || camera->telemetry->count(LossReason::sourceError)
                    || (camera->pattern && camera->pattern->failed())
                    || (camera->pattern && camera->pattern->finished() && elapsed < o.seconds - 0.1)
                    || (camera->capture && camera->capture->finished());
                if (bad && !camera->failureDisplayed)
                {
                    camera->failureDisplayed = true;
                    window.title("RecorderDualProbe - cam1 " + std::string(cameras[0]->failureDisplayed ? "PARTIAL FAILURE" : "recording")
                        + " | cam2 " + (cameras[1]->failureDisplayed ? "PARTIAL FAILURE" : "recording") + " | audio continues");
                }
            }
            if (elapsed >= o.seconds && audio.complete()) { durationComplete = true; break; }
            if (elapsed > o.seconds + 5) { failure = "Audio duration incomplete after five-second drain allowance"; break; }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        }
        ended = qpcNow(); cpuEnd = cpuTicks();
    }
    catch (const std::exception& e) { failure = e.what(); }
    if (!released) { go.set_value(); released = true; }
    // Close driver on its control owner. Keep HWND pumping during all GPU joins.
    audio.stopInput();
    pumped(window, [&]
    {
        for (auto& camera : cameras) camera->stopCapture();
        for (auto& camera : cameras) camera->finish();
        audio.finish();
    });
    if (contention) contention->finish();
    if (started && !ended) { ended = qpcNow(); cpuEnd = cpuTicks(); }
    const auto elapsed = started ? static_cast<double>(ended - origin.load()) / qpcFrequency() : 0;
    jsonSet(report, "measurementStartQpc", std::to_string(origin.load())); jsonSet(report, "secondsMeasured", elapsed);
    jsonSet(report, "durationComplete", durationComplete); jsonSet(report, "processCpuPercentOneCore", elapsed > 0 ? (cpuEnd - cpu) / (elapsed * 100000) : 0);
    jsonSet(report, "cpuDefinition", "GetProcessTimes user+kernel; 100%=one logical CPU. Both cameras/audio and injected contention included; preparation/drain/post-decode excluded.");
    jsonSet(report, "audio", audio.toJson()); jsonSet(report, "audioWriterStall", audioStall.toJson());
    jsonSet(report, "queueTimeline", queueTrace); jsonSet(report, "queueTimelineSamples", queueSamples);
    jsonSet(report, "queueTimelineDefinition", "Approximately once per second on control thread, last 128 samples. Atomic approximate occupancies, not a transactional snapshot. Inspect sustained growth and recovery after stall; high-water counters cover the complete run.");
    bool clean = started && durationComplete && failure.empty() && static_cast<bool>(report["audio"]["complete"]), unavailable = !started;
    juce::Array<juce::var> reports;
    for (auto& camera : cameras)
    {
        auto v = jsonObject(); auto encode = camera->encode->toJson();
        jsonSet(v, "pipelineId", camera->telemetry->pipelineId); jsonSet(v, "sourceKind", camera->synthetic ? "synthetic" : "hardware");
        jsonSet(v, "source", camera->pattern ? camera->pattern->toJson() : camera->sourceInfo);
        jsonSet(v, "nativeMode", CameraCatalog::modeJson(camera->mode)); jsonSet(v, "telemetry", camera->telemetry->toJson());
        jsonSet(v, "encode", encode); jsonSet(v, "writerStall", camera->stall.toJson()); jsonSet(v, "output", camera->output.getFullPathName());
        jsonSet(v, "partialFailureDisplayed", camera->failureDisplayed);
        const auto adapter = camera->presenter ? camera->presenter->adapterJson() : juce::var(); jsonSet(v, "previewAdapter", adapter);
        const auto& latency = camera->telemetry->distribution(Timing::callbackToPresent);
        const double limit = camera->mode.fps.value() > 45 ? 35 : 45;
        const auto refresh = std::string(displayRefreshVerdict(static_cast<int>(adapter["displayRefreshHz"])));
        const auto captureError = camera->capture ? camera->capture->error() : std::string();
        const auto presentError = camera->presenter ? camera->presenter->error() : std::string();
        jsonSet(v, "captureError", captureError); jsonSet(v, "presenterError", presentError);
        jsonSet(v, "callbackToPresentP95LimitMs", limit); jsonSet(v, "displayRefreshVerdict", refresh);
        bool laneClean = static_cast<bool>(encode["complete"]) && camera->telemetry->softwareLossFree()
            && !camera->telemetry->count(LossReason::timestampRegression) && captureError.empty() && presentError.empty()
            && latency.count() && latency.percentile(.95) <= limit && refresh != "FAIL";
        if (started && refresh == "UNAVAILABLE") unavailable = true;
        if (camera->synthetic && camera->output.existsAsFile())
        {
            juce::var oracle;
            try { oracle = pumped(window, [&] { return inspectPatternMp4(camera->output, camera->id, camera->mode.fps, {o.fps, 1}, o.seconds); }); }
            catch (const std::exception& e) { oracle = jsonObject(); status(oracle, "FAIL", e.what()); jsonSet(oracle, "captureLoss", juce::var()); }
            jsonSet(v, "pixelOracle", oracle); laneClean &= oracle["result"].toString() == "PASS";
        }
        else
        {
            auto oracle = jsonObject(); status(oracle, "UNAVAILABLE", camera->synthetic ? "No finalized MP4; preserve .recording.mp4 and encoder evidence" : "Physical source has no independent frame-number pattern; CFR counts cannot certify capture loss");
            jsonSet(oracle, "captureLoss", juce::var()); jsonSet(v, "pixelOracle", oracle);
        }
        if (camera->pattern)
        {
            const auto source = camera->pattern->toJson();
            laneClean &= source["producerError"].toString().isEmpty() && source["workerError"].toString().isEmpty()
                && static_cast<juce::int64>(source["sourceGenerationSkipped"]) == 0;
        }
        if (o.stallMs) laneClean &= static_cast<bool>(camera->stall.toJson()["injected"]);
        status(v, !started ? "UNAVAILABLE" : laneClean ? "PASS" : "FAIL", "Software budget and finalized encode; physical optical certification remains separate");
        clean &= laneClean; reports.add(v);
    }
    if (o.stallMs) clean &= static_cast<bool>(audioStall.toJson()["injected"]);
    jsonSet(report, "cameras", reports);
    jsonSet(report, "clockDecision", "Load spike: synthetic nominal PTS + common QPC origin; physical lanes retain MF PTS mapper. ASIO ClockMapper measured separately. No calibrated A/V/exposure alignment certification; ReferenceMixWriter AAC is synthetic and not the raw 8ch mix.");
    jsonSet(report, "poolIsolation", "Exactly two camera-owned capture queues, preview mailboxes, AVFrame banks, NVENC contexts, packet queues and MP4 writer owners; no cross-camera pool references.");
    if (!started) status(report, "UNAVAILABLE", failure);
    else if (!clean) status(report, "FAIL", failure.empty() ? "Inspect per-camera budgets, pixel oracle, audio queues and stall execution" : failure);
    else if (unavailable) status(report, "UNAVAILABLE", "Measured load retained; display refresh unavailable");
    else status(report, "PASS", "Requested simultaneous software load measured; physical P0/USB/source loss release gate remains unverified");
}
}
int wmain(int argc, wchar_t** argv)
{
    auto report = jsonObject(); std::string reportPath;
    // Retain --report even when strict option validation fails.
    for (int i = 1; i + 1 < argc; ++i) if (juce::String(argv[i]) == "--report") reportPath = juce::String(argv[i + 1]).toStdString();
    try
    {
        juce::Array<juce::var> command; for (int i = 0; i < argc; ++i) command.add(juce::String(argv[i]));
        jsonSet(report, "command", command); jsonSet(report, "schemaVersion", 1); jsonSet(report, "startedUtc", utcNowIso8601());
        const auto o = parse(argc, argv);
        if (o.help)
        {
            std::cout << "RecorderDualProbe (--synthetic | --devices FILE [--cam2 synthetic]) [--synthetic-audio | --asio-device 0]\n"
                "  --project-fps 30|60 --seconds 60 --report FILE [--stall-ms 2000] [--cpu-contention N]\n"
                "  [--cam1-format nv12|mjpeg --cam2-format nv12|mjpeg] (defaults: NV12 60 + MJPEG 30)\n"
                "RecorderDualProbe --headroom --seconds 60 --project-fps 60 --report FILE\n"
                "RecorderDualProbe --devices devices.json --cam2 synthetic --toggle-camera2 --seconds 60 --report r24/dual-preview.json [--headless]\n"
                "8 actual ASIO inputs required unless --synthetic-audio. MP4/WAV are stored in a unique sibling directory.\n"; return 0;
        }
        jsonSet(report, "os", juce::SystemStats::getOperatingSystemName()); jsonSet(report, "cpu", juce::SystemStats::getCpuModel());
        jsonSet(report, "logicalCpus", juce::SystemStats::getNumCpus()); jsonSet(report, "qpcFrequency", std::to_string(qpcFrequency()));
        jsonSet(report, "expectedFfmpeg", RECORDER_FFMPEG_VERSION); jsonSet(report, "loadedFfmpeg", av_version_info());
        jsonSet(report, "ffmpegConfiguration", avcodec_configuration()); jsonSet(report, "msvc", _MSC_FULL_VER);
        jsonSet(report, "sourceKind", o.headroom ? "synthetic-encode-only" : o.synthetic ? "synthetic" : o.cam2Synthetic ? "mixed" : "hardware");
        jsonSet(report, "requestedSeconds", o.seconds); jsonSet(report, "projectFps", o.fps); jsonSet(report, "cpuContentionThreads", o.contention);
        jsonSet(report, "opticalP0", "UNAVAILABLE: no glass-to-glass/USB/exposure experiment in this executable");
        if (std::string(av_version_info()) != RECORDER_FFMPEG_VERSION || (avcodec_version() >> 16) != 62) throw std::runtime_error("Loaded FFmpeg differs from pinned SDK");
        juce::ScopedJuceInitialiser_GUI juceRuntime;
        struct Timer { MMRESULT result = timeBeginPeriod(1); ~Timer() { if (result == TIMERR_NOERROR) timeEndPeriod(1); } } timer;
        jsonSet(report, "timeBeginPeriodResult", timer.result);
        if (o.headroom) headroom(o, report); else if (o.toggleCamera2) productDualLoad(o, report); else dualLoad(o, report);
    }
    catch (const std::exception& e) { status(report, "FAIL", e.what()); }
    jsonSet(report, "endedUtc", utcNowIso8601());
    try { if (!reportPath.empty()) CaptureTelemetry::writeJson(file(reportPath), report); }
    catch (const std::exception& e) { std::cerr << "Report write failed: " << e.what() << '\n'; return 1; }
    std::cout << juce::JSON::toString(report, false).toStdString() << '\n';
    return report["result"].toString() == "PASS" ? 0 : report["result"].toString() == "UNAVAILABLE" ? 2 : 1;
}
