#include "capture/MfCameraCapture.h"
#include "video/PreviewPresenter.h"
#include "video/PresentPacing.h"
#include "diagnostics/MfJpegRangeProbe.h"
#include "record/EncodePipeline.h"
#include "audio/AsioProbe.h"
namespace gocue::recorder { int runPlaybackProbe(int argc, wchar_t** argv); }
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <future>
#include <map>
#include <set>
#include <thread>

using namespace gocue::recorder;
namespace
{
struct Arguments
{
    std::string command;
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
    juce::Array<juce::var> original;
    bool has(const std::string& key) const { return flags.count(key) || values.count(key); }
    std::string get(const std::string& key, std::string fallback = {}) const
    {
        const auto it = values.find(key); return it == values.end() ? fallback : it->second;
    }
    std::string required(const std::string& key) const
    {
        if (!values.count(key) || get(key).empty()) throw std::invalid_argument("Missing " + key);
        return get(key);
    }
};
unsigned positiveInteger(const std::string& text)
{
    unsigned result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !result) throw std::invalid_argument("Expected positive integer: " + text);
    return result;
}
Arguments parse(int argc, wchar_t** argv)
{
    Arguments args;
    for (int i = 0; i < argc; ++i) args.original.add(juce::String(argv[i]));
    if (argc < 2) throw std::invalid_argument("RecorderProbe enumerate --select [--cam1 N --cam1-mode N] --out devices.json | capture --devices FILE --camera cam1 --seconds N --report FILE | mf-jpeg-range [--report FILE] | encode [--devices FILE --camera cam1 | --synthetic] --project-fps 30|60 --seconds N [--preset p5 --report FILE --out-dir DIR]");
    args.command = juce::String(argv[1]).toStdString();
    const std::set<std::string> allowedFlags = args.command == "mf-jpeg-range" ? std::set<std::string>{}
        : args.command == "enumerate" ? std::set<std::string>{"--select"}
        : args.command == "encode" ? std::set<std::string>{"--synthetic"} : std::set<std::string>{"--compare-decoders"};
    const std::set<std::string> allowedValues = args.command == "mf-jpeg-range" ? std::set<std::string>{"--report"}
        : args.command == "enumerate" ? std::set<std::string>{"--out", "--cam1", "--cam1-mode", "--cam2", "--cam2-mode"}
        : args.command == "encode" ? std::set<std::string>{"--devices", "--camera", "--mode", "--seconds", "--report", "--out-dir", "--project-fps", "--preset"}
        : std::set<std::string>{"--devices", "--camera", "--mode", "--seconds", "--report", "--decoder-threads", "--decoder"};
    if (args.command != "enumerate" && args.command != "capture" && args.command != "encode" && args.command != "mf-jpeg-range") throw std::invalid_argument("Unknown subcommand: " + args.command);
    for (int i = 2; i < argc; ++i)
    {
        const auto key = juce::String(argv[i]).toStdString();
        if (args.has(key)) throw std::invalid_argument("Duplicate option: " + key);
        if (allowedFlags.count(key)) args.flags.insert(key);
        else if (allowedValues.count(key) && i + 1 < argc) args.values.emplace(key, juce::String(argv[++i]).toStdString());
        else throw std::invalid_argument("Unknown/incomplete option: " + key);
    }
    return args;
}
juce::File filePath(const std::string& path)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(path.c_str()));
}
juce::var baseReport(const Arguments& args, juce::var* partial = nullptr)
{
    auto value = jsonObject(), sdk = jsonObject();
    jsonSet(value, "schemaVersion", 1); jsonSet(value, "command", args.original);
    jsonSet(value, "sourceKind", "hardware"); jsonSet(value, "startedUtc", utcNowIso8601());
    jsonSet(value, "os", juce::SystemStats::getOperatingSystemName());
    jsonSet(sdk, "expectedVersion", RECORDER_FFMPEG_VERSION); jsonSet(sdk, "loadedVersion", av_version_info());
    jsonSet(sdk, "avcodecVersion", jsonInt(avcodec_version())); jsonSet(sdk, "avutilVersion", jsonInt(avutil_version()));
    jsonSet(sdk, "swscaleVersion", jsonInt(swscale_version())); jsonSet(sdk, "configuration", avcodec_configuration());
    jsonSet(sdk, "avformatVersion", jsonInt(avformat_version())); jsonSet(sdk, "swresampleVersion", jsonInt(swresample_version()));
    jsonSet(value, "sdk", sdk);
    if (partial) *partial = value; // keep version/command evidence even if validation throws
    if (std::string(av_version_info()) != RECORDER_FFMPEG_VERSION || (avcodec_version() >> 16) != 62)
        throw std::runtime_error("Loaded FFmpeg runtime differs from the configured SDK");
    return value;
}
void status(juce::var& value, const char* verdict, const std::string& reason)
{
    jsonSet(value, "result", verdict);
    jsonSet(value, "status", std::string(verdict) == "UNAVAILABLE" ? "unavailable" : std::string(verdict) == "PASS" ? "available" : "failed");
    jsonSet(value, "reason", reason); jsonSet(value, "endedUtc", utcNowIso8601());
}
size_t readChoice(const std::string& prompt, size_t limit, bool zeroAllowed)
{
    std::cout << prompt << std::flush;
    std::string input;
    if (!std::getline(std::cin, input)) throw std::runtime_error("Selection unavailable: stdin closed; run enumerate --select in an interactive console");
    if (zeroAllowed && input == "0") return 0;
    const auto n = positiveInteger(input);
    if (n > limit) throw std::invalid_argument("Selection out of range");
    return n;
}
juce::var selectCamera(const std::vector<CameraDevice>& devices, const char* camera, bool optional, const Arguments& args)
{
    const std::string option = std::string("--") + camera;
    const bool automatic = args.has("--cam1");
    if (automatic && optional && !args.has(option)) return {};
    const auto choice = automatic ? positiveInteger(args.required(option))
        : readChoice(std::string(camera) + " device number" + (optional ? " (0=off): " : ": "), devices.size(), optional);
    if (!choice) return {};
    if (choice > devices.size()) throw std::invalid_argument("Device number out of range (1-based)");
    const auto& device = devices[choice - 1];
    if (!device.unavailableReason.empty()) throw std::runtime_error(device.unavailableReason);
    for (size_t i = 0; i < device.modes.size(); ++i) std::cout << "  " << i + 1 << ") " << device.modes[i].text() << '\n';
    const auto mode = automatic ? positiveInteger(args.required(option + "-mode")) : readChoice("Native mode number (keep its exact rational FPS): ", device.modes.size(), false);
    if (mode > device.modes.size()) throw std::invalid_argument("Native mode number out of range (1-based)");
    auto value = jsonObject();
    jsonSet(value, "symbolicLink", device.symbolicLink); jsonSet(value, "friendlyName", device.friendlyName);
    jsonSet(value, "mode", device.modes[mode - 1].text());
    return value;
}
int enumerate(const Arguments& args)
{
    const auto out = filePath(args.required("--out"));
    auto report = jsonObject();
    try
    {
        report = baseReport(args, &report);
        const bool indices = args.has("--cam1") || args.has("--cam1-mode") || args.has("--cam2") || args.has("--cam2-mode");
        if (indices && (!args.has("--select") || !args.has("--cam1") || !args.has("--cam1-mode") || args.has("--cam2") != args.has("--cam2-mode")))
            throw std::invalid_argument("Noninteractive selection requires --select --cam1 N --cam1-mode N, and paired --cam2 N --cam2-mode N");
        ComApartment com; MfRuntime mf;
        const auto devices = CameraCatalog::enumerate();
        jsonSet(report, "devices", CameraCatalog::toJson(devices));
        jsonSet(report, "asio", "UNAVAILABLE: outside round 01; implemented in round 03");
        bool any = false;
        for (size_t i = 0; i < devices.size(); ++i)
        {
            std::cout << i + 1 << ") " << devices[i].friendlyName << " [" << devices[i].modes.size() << " native modes] " << devices[i].unavailableReason << '\n';
            any |= devices[i].unavailableReason.empty() && !devices[i].modes.empty();
        }
        if (!any) throw std::runtime_error("No accessible MF camera with native NV12/YUY2/MJPEG types");
        if (args.has("--select"))
        {
            auto selections = jsonObject();
            jsonSet(selections, "cam1", selectCamera(devices, "cam1", false, args));
            if (devices.size() > 1 || args.has("--cam2"))
            {
                const auto second = selectCamera(devices, "cam2", true, args);
                if (second.isObject())
                {
                    if (second["symbolicLink"] == selections["cam1"]["symbolicLink"]) throw std::invalid_argument("cam1 and cam2 must use different symbolic links");
                    jsonSet(selections, "cam2", second);
                }
            }
            jsonSet(report, "selections", selections);
        }
        status(report, "PASS", "MF native types enumerated; no samples captured and no performance gate evaluated");
    }
    catch (const std::invalid_argument& e) { status(report, "FAIL", e.what()); }
    catch (const std::exception& e) { status(report, "UNAVAILABLE", e.what()); }
    CaptureTelemetry::writeJson(out, report);
    std::cout << report["result"].toString() << ": " << report["reason"].toString() << '\n';
    return report["result"].toString() == "PASS" ? 0 : report["result"].toString() == "UNAVAILABLE" ? 2 : 1;
}
constexpr UINT closeMessage = WM_APP + 19;
std::uint64_t processCpu100ns()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) throw std::runtime_error("GetProcessTimes failed");
    const auto ticks = [](FILETIME time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_CLOSE) { PostMessageW(window, closeMessage, 0, 0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
struct ProbeWindow
{
    HWND handle = nullptr;
    ProbeWindow()
    {
        WNDCLASSW cls{}; cls.lpfnWndProc = windowProc; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"RecorderProbePreview"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register preview window failed");
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rect{0, 0, 1280, 720}; AdjustWindowRect(&rect, style, FALSE);
        handle = CreateWindowExW(0, cls.lpszClassName, L"RecorderProbe - P0 live preview", style,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, cls.hInstance, nullptr);
        if (!handle) throw std::runtime_error("Create preview HWND failed");
        ShowWindow(handle, SW_SHOW); UpdateWindow(handle);
    }
    ~ProbeWindow() { if (handle) DestroyWindow(handle); }
    bool pump()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == closeMessage || message.message == WM_QUIT) return false;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return true;
    }
};
juce::var runCapture(const std::string& link, CameraMode mode, bool mfDecode, int threads, unsigned seconds, ProbeWindow& window)
{
    auto result = jsonObject();
    jsonSet(result, "startedUtc", utcNowIso8601());
    jsonSet(result, "path", mfDecode ? "mf-mjpeg-nv12" : "ffmpeg-native");
    jsonSet(result, "requestedNativeMode", CameraCatalog::modeJson(mode));
    jsonSet(result, "secondsRequested", jsonInt(seconds));
    jsonSet(result, "cpuDecodeDefinition", mfDecode ? "Post-MF raw NV12 binding only; range/matrix conversion is in colourNormalise. Internal MF MJPEG decode duration is UNAVAILABLE (happens before callback)."
        : "FFmpeg avcodec_send_packet/receive_frame including padded packet copy; native raw binding for NV12/YUY2. Separate from colour normalisation.");
    jsonSet(result, "callbackToPresentDefinition", mfDecode ? "Decoded NV12 SourceReader callback entry to successful Present submission; excludes internal MF decode."
        : "Native SourceReader callback entry to successful Present submission; includes worker decode and colour normalisation.");
    auto telemetry = std::make_shared<CaptureTelemetry>(mode.fps);
    VideoSurfacePool pool(mode.width, mode.height);
    PreviewPresenter presenter(window.handle, pool, telemetry, mode.width, mode.height);
    MfCameraCapture capture(telemetry, pool);
    std::promise<void> beginMeasurement;
    auto measurementStart = beginMeasurement.get_future().share();
    bool opened = false, durationComplete = false, cancelled = false;
    std::string failure;
    auto start = std::chrono::steady_clock::now();
    auto cpuStart = processCpu100ns();
    try
    {
        const auto title = juce::String("RecorderProbe - ") + (mfDecode ? "MF MJPEG -> NV12" : "FFmpeg / native") + " - " + mode.text();
        SetWindowTextW(window.handle, title.toWideCharPointer());
        // DXGI can synchronously send window messages while creating the swapchain.
        // Keep the HWND owner pumping even while the presenter prepares its device.
        auto presentReady = std::async(std::launch::async, [&] { presenter.start(measurementStart); });
        while (presentReady.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        { cancelled |= !window.pump(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT); }
        presentReady.get();
        if (cancelled) throw std::runtime_error("User closed preview during preparation");
        auto captureReady = std::async(std::launch::async, [&] { return capture.start(link, mode, mfDecode, threads, measurementStart); });
        while (captureReady.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        { cancelled |= !window.pump(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT); }
        const auto info = captureReady.get();
        if (cancelled) throw std::runtime_error("User closed preview during camera open");
        jsonSet(result, "nativeMode", CameraCatalog::modeJson(info.nativeMode));
        jsonSet(result, "outputMode", CameraCatalog::modeJson(info.outputMode));
        jsonSet(result, "decoder", info.decoderName); jsonSet(result, "effectiveDecoderThreads", info.decoderThreads);
        telemetry->reset(); // both threads are prepared but cannot yet write telemetry
        opened = true; start = std::chrono::steady_clock::now(); cpuStart = processCpu100ns();
        beginMeasurement.set_value();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds))
        {
            if (!window.pump()) { cancelled = true; failure = "User closed preview before requested duration"; break; }
            if (capture.finished() || presenter.finished()) { failure = "Capture/presenter stopped before requested duration"; break; }
            if (telemetry->samples.load() == 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
            { failure = "Camera delivered no samples within 5 seconds"; break; }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        }
        durationComplete = failure.empty();
    }
    catch (const std::exception& e) { failure = e.what(); }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto cpuElapsed = processCpu100ns() - cpuStart;
    // The HWND must remain alive and its message queue serviced while DXGI shuts down.
    std::atomic<bool> stopped{false};
    std::thread shutdown([&] { capture.stop(); presenter.stop(); stopped.store(true); });
    while (!stopped.load()) { window.pump(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT); }
    shutdown.join();
    if (!capture.error().empty()) failure += (failure.empty() ? "" : "; ") + capture.error();
    if (!presenter.error().empty()) failure += (failure.empty() ? "" : "; ") + presenter.error();
    jsonSet(result, "secondsMeasured", elapsed); jsonSet(result, "durationComplete", durationComplete); jsonSet(result, "cancelled", cancelled);
    jsonSet(result, "processCpuSeconds", static_cast<double>(cpuElapsed) / 10000000.0);
    jsonSet(result, "processCpuPercentOneCore", elapsed > 0 ? static_cast<double>(cpuElapsed) / (elapsed * 100000.0) : 0.0);
    jsonSet(result, "cpuMetricDefinition", "GetProcessTimes user+kernel over measured pass, including MF/FFmpeg/presenter threads; 100%=one fully occupied logical CPU; preparation/shutdown excluded after successful open");
    jsonSet(result, "adapter", presenter.adapterJson()); jsonSet(result, "colourDecision", capture.colourDecision());
    jsonSet(result, "normalisedOutput", VideoSurface::outputFormat); jsonSet(result, "telemetry", telemetry->toJson());
    jsonSet(result, "opticalP0", "UNAVAILABLE: optical camera/display latency not measured");
    jsonSet(result, "colourCertification", telemetry->colourAssumptions.load() ? "UNAVAILABLE: some metadata missing; assumptions explicitly recorded" : "Metadata checked; physical colour chart not measured");
    const double p95Limit = mode.fps.value() > 45 ? 35.0 : 45.0;
    jsonSet(result, "callbackToPresentP95LimitMs", p95Limit);
    const bool clean = telemetry->softwareLossFree() && telemetry->count(LossReason::timestampRegression) == 0;
    const auto& latency = telemetry->distribution(Timing::callbackToPresent);
    const bool latencyPass = latency.count() > 0 && latency.percentile(0.95) <= p95Limit;
    const int hz = presenter.adapterJson()["displayRefreshHz"];
    if (!opened || telemetry->samples.load() == 0) status(result, "UNAVAILABLE", failure.empty() ? "No delivered hardware samples" : failure);
    else if (!failure.empty() || !durationComplete || !clean || !latencyPass || std::string(displayRefreshVerdict(hz)) == "FAIL")
        status(result, "FAIL", failure.empty() ? "Software observation gate failed: inspect losses, callback-to-Present latency and display refresh (integer Hz >=59 includes 59.94)" : failure);
    else if (std::string(displayRefreshVerdict(hz)) == "UNAVAILABLE")
        status(result, "UNAVAILABLE", "Software observations retained; display refresh query unavailable");
    else status(result, "PASS", mfDecode ? "Measured post-MF-callback software budget only; MF decode/optical/source loss certification remains unavailable"
        : "Measured software callback-to-Present budget; optical/source loss certification remains unavailable");
    return result;
}
int captureCommand(const Arguments& args)
{
    const auto out = filePath(args.required("--report"));
    auto report = jsonObject();
    try
    {
        report = baseReport(args, &report);
        const auto seconds = positiveInteger(args.required("--seconds"));
        const auto threads = positiveInteger(args.get("--decoder-threads", "1"));
        if (threads > 16) throw std::invalid_argument("--decoder-threads must be 1..16");
        const auto camera = args.required("--camera");
        if (camera != "cam1" && camera != "cam2") throw std::invalid_argument("--camera must be cam1 or cam2");
        const auto configFile = filePath(args.required("--devices"));
        if (!configFile.existsAsFile()) throw std::runtime_error("Device JSON missing: " + configFile.getFullPathName().toStdString());
        juce::var config;
        const auto parsed = juce::JSON::parse(configFile.loadFileAsString(), config);
        if (parsed.failed()) throw std::invalid_argument(parsed.getErrorMessage().toStdString());
        if (static_cast<int>(config["schemaVersion"]) != 1) throw std::invalid_argument("Unsupported devices.json schemaVersion");
        const auto selected = config["selections"][juce::Identifier(camera)];
        const auto link = selected["symbolicLink"].toString().toStdString();
        if (link.empty()) throw std::runtime_error("Selected camera unavailable; run enumerate --select first");
        auto mode = CameraMode::parse(args.get("--mode", selected["mode"].toString().toStdString()));
        jsonSet(report, "camera", camera); jsonSet(report, "symbolicLink", link); jsonSet(report, "friendlyName", selected["friendlyName"]);
        ComApartment com; MfRuntime mf;
        ProbeWindow window;
        juce::Array<juce::var> runs;
        const auto addRun = [&](juce::var run) { runs.add(run); jsonSet(report, "runs", runs); };
        if (args.has("--compare-decoders"))
        {
            // Pin one actual native MJPEG mode for BOTH sequential passes. The same UVC
            // source generally cannot be opened concurrently. Never compare different modes.
            if (mode.subtype != CaptureSubtype::mjpeg)
            {
                if (args.has("--mode")) throw std::invalid_argument("--compare-decoders with explicit --mode requires MJPEG");
                const auto devices = CameraCatalog::enumerate();
                bool found = false;
                for (const auto& device : devices) if (device.symbolicLink == link)
                    for (const auto& candidate : device.modes)
                        if (candidate.subtype == CaptureSubtype::mjpeg && candidate.width == mode.width && candidate.height == mode.height && candidate.fps == mode.fps)
                        { mode = candidate; found = true; break; }
                if (!found) throw std::runtime_error("No matching native MJPEG resolution/rational FPS for decoder comparison");
            }
            if (seconds < 2) throw std::invalid_argument("Decoder comparison requires at least 2 total seconds");
            jsonSet(report, "comparisonProtocol", "Sequential A/B on the exact same native MJPEG mode; N seconds TOTAL split floor(N/2) FFmpeg and remaining MF. Keep HDMI/UVC scene/pattern running unchanged. Same-scene identity is operator-controlled, not verified.");
            jsonSet(report, "comparisonLimitation", "MF transforms before OnReadSample; internal MF decode time unavailable. Compare deviceToPresent only when DeviceTimestamp is retained and valid; callbackToPresent endpoints differ. No automatic winning decoder selection.");
            std::cout << "Compare: keep the same input scene/pattern running. FFmpeg " << seconds / 2 << "s, then MF " << seconds - seconds / 2 << "s.\n";
            addRun(runCapture(link, mode, false, static_cast<int>(threads), seconds / 2, window));
            if (!static_cast<bool>(runs.getLast()["cancelled"])) addRun(runCapture(link, mode, true, static_cast<int>(threads), seconds - seconds / 2, window));
        }
        else
        {
            // 2026-09-09 measurement (GC311G2 MJPEG 1080p60, 60 s): MF MJPEG->NV12 callback-to-Present p95 8 ms at 31% of a core;
            // the FFmpeg CPU path spent ~20 ms/frame in decode + swscale colour normalisation and dropped 9%. MJPEG therefore
            // defaults to the MF decoder; raw NV12/YUY2 modes stay on the native path. Override with --decoder mf|ffmpeg.
            const auto decoder = args.get("--decoder", mode.subtype == CaptureSubtype::mjpeg ? "mf" : "ffmpeg");
            if (decoder != "mf" && decoder != "ffmpeg") throw std::invalid_argument("--decoder must be mf or ffmpeg");
            if (decoder == "mf" && mode.subtype != CaptureSubtype::mjpeg) throw std::invalid_argument("--decoder mf requires an MJPEG native mode");
            addRun(runCapture(link, mode, decoder == "mf", static_cast<int>(threads), seconds, window));
        }
        jsonSet(report, "runs", runs);
        bool allPass = true, unavailable = false;
        for (const auto& run : runs) { allPass &= run["result"].toString() == "PASS"; unavailable |= run["result"].toString() == "UNAVAILABLE"; }
        status(report, allPass ? "PASS" : unavailable ? "UNAVAILABLE" : "FAIL", "See per-path software observations; optical P0 and independent capture-loss oracle were not measured");
    }
    catch (const std::exception& e) { status(report, "UNAVAILABLE", e.what()); }
    CaptureTelemetry::writeJson(out, report);
    std::cout << report["result"].toString() << ": " << report["reason"].toString() << "\nReport: " << out.getFullPathName() << '\n';
    return report["result"].toString() == "PASS" ? 0 : report["result"].toString() == "UNAVAILABLE" ? 2 : 1;
}
int mfJpegRangeCommand(const Arguments& args)
{
    auto report = jsonObject();
    try
    {
        report = baseReport(args, &report);
        ComApartment com; MfRuntime mf;
        const auto measured = probeMfJpegRange();
        jsonSet(report, "sourceKind", "synthetic-offline"); jsonSet(report, "measurement", measured);
        status(report, static_cast<bool>(measured["allClassified"]) ? "PASS" : "UNAVAILABLE",
            "Direct MFT synthetic colour judgement; inspect numeric patch means and model errors");
    }
    catch (const std::exception& e) { status(report, "UNAVAILABLE", e.what()); }
    if (args.has("--report")) CaptureTelemetry::writeJson(filePath(args.required("--report")), report);
    std::cout << juce::JSON::toString(report, false) << '\n';
    return report["result"].toString() == "PASS" ? 0 : 2;
}
}
namespace
{
int encodeCommand(const Arguments& args)
{
    auto report = baseReport(args);
    const bool synthetic = args.has("--synthetic");
    jsonSet(report, "sourceKind", synthetic ? "synthetic" : "hardware");
    try
    {
        NvencProfile profile;
        profile.fps = static_cast<int>(positiveInteger(args.required("--project-fps")));
        profile.preset = args.get("--preset", "p5"); profile.validate();
        const auto seconds = positiveInteger(args.required("--seconds"));
        jsonSet(report, "secondsRequested", jsonInt(seconds)); jsonSet(report, "profile", profile.toJson());
        if (synthetic)
        {
            if (args.has("--devices") || args.has("--camera") || args.has("--mode") || args.has("--out-dir"))
                throw std::invalid_argument("--synthetic measures encode-only headroom; camera and output media options do not apply");
            const auto measurement = EncodePipeline::headroom(profile, seconds);
            jsonSet(report, "headroom", measurement);
            status(report, static_cast<bool>(measurement["goalMet"]) ? "PASS" : "FAIL", "Content-specific encode-only throughput versus 156 fps; real capture/two-camera/P0 certification remains unavailable");
        }
        else
        {
            const auto reportFile = filePath(args.required("--report"));
            const auto directory = args.has("--out-dir") ? filePath(args.required("--out-dir")) : reportFile.getParentDirectory();
            const auto camera = args.required("--camera");
            if (camera != "cam1" && camera != "cam2") throw std::invalid_argument("--camera must be cam1 or cam2");
            const auto configFile = filePath(args.required("--devices"));
            if (!configFile.existsAsFile()) throw std::runtime_error("Device JSON is missing");
            juce::var config; const auto parsed = juce::JSON::parse(configFile.loadFileAsString(), config);
            if (parsed.failed() || static_cast<int>(config["schemaVersion"]) != 1) throw std::invalid_argument("Invalid devices.json schema/JSON");
            const auto selected = config["selections"][juce::Identifier(camera)];
            const auto link = selected["symbolicLink"].toString().toStdString();
            if (link.empty()) throw std::runtime_error("Camera selection missing; run enumerate --select first");
            const auto mode = CameraMode::parse(args.get("--mode", selected["mode"].toString().toStdString()));
            if (mode.width != 1920 || mode.height != 1080 || (mode.subtype != CaptureSubtype::mjpeg && mode.subtype != CaptureSubtype::nv12))
                throw std::invalid_argument("Round 02 encode input requires 1920x1080 native NV12 or MJPEG decoded by MF");
            jsonSet(report, "camera", camera); jsonSet(report, "symbolicLink", link); jsonSet(report, "friendlyName", selected["friendlyName"]);
            jsonSet(report, "requestedNativeMode", CameraCatalog::modeJson(mode));
            jsonSet(report, "callbackToPresentDefinition", mode.subtype == CaptureSubtype::mjpeg
                ? "Decoded NV12 SourceReader callback to Present submission; MF MJPEG decode precedes this interval. Includes the bounded record-pool copy."
                : "Native NV12 SourceReader callback to Present submission, including decode-worker preparation and bounded record-pool copy.");
            ComApartment com; MfRuntime mf; ProbeWindow window;
            auto telemetry = std::make_shared<CaptureTelemetry>(mode.fps);
            VideoSurfacePool previewPool(mode.width, mode.height);
            PreviewPresenter presenter(window.handle, previewPool, telemetry, mode.width, mode.height);
            EncodePipeline recording(profile, mode.fps, seconds, directory.getChildFile(juce::String(camera) + ".mp4"), telemetry);
            MfCameraCapture capture(telemetry, previewPool, [&](const VideoSurface& frame) { recording.offer(frame); });
            bool opened = false, completed = false, cancelled = false;
            std::string failure;
            auto start = std::chrono::steady_clock::now(); auto cpuStart = processCpu100ns();
            try
            {
                SetWindowTextW(window.handle, L"RecorderProbe encode - MF NV12 / CFR / NVENC / synthetic AAC");
                auto ready = std::async(std::launch::async, [&]
                {
                    presenter.start(); recording.start();
                    return capture.start(link, mode, mode.subtype == CaptureSubtype::mjpeg, 1);
                });
                while (ready.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                { cancelled |= !window.pump(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT); }
                const auto info = ready.get(); opened = true;
                jsonSet(report, "nativeMode", CameraCatalog::modeJson(info.nativeMode));
                jsonSet(report, "outputMode", CameraCatalog::modeJson(info.outputMode)); jsonSet(report, "decoder", info.decoderName);
                start = std::chrono::steady_clock::now(); cpuStart = processCpu100ns();
                while (!cancelled && recording.secondsSinceOrigin() < seconds)
                {
                    if (!window.pump()) { cancelled = true; break; }
                    if (capture.finished() || presenter.finished()) { failure = "Capture/presenter stopped before the requested duration"; break; }
                    if (recording.secondsSinceOrigin() < 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
                    { failure = "No retained camera frame within five seconds"; break; }
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
                }
                completed = !cancelled && failure.empty();
            }
            catch (const std::exception& e) { failure = e.what(); }
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const auto cpu = processCpu100ns() - cpuStart;
            std::atomic<bool> stopped{false};
            const auto stopStart = qpcNow();
            std::thread shutdown([&] { capture.stop(); recording.stop(); presenter.stop(); stopped.store(true); });
            while (!stopped.load()) { window.pump(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT); }
            shutdown.join();
            jsonSet(report, "stopDrainTotalMs", 1000.0 * (qpcNow() - stopStart) / qpcFrequency());
            if (!capture.error().empty()) failure += (failure.empty() ? "" : "; ") + capture.error();
            if (!presenter.error().empty()) failure += (failure.empty() ? "" : "; ") + presenter.error();
            const auto encoded = recording.toJson();
            jsonSet(report, "recording", encoded); jsonSet(report, "capture", telemetry->toJson());
            jsonSet(report, "adapter", presenter.adapterJson()); jsonSet(report, "colourDecision", capture.colourDecision());
            jsonSet(report, "colourCertification", "UNAVAILABLE: MF NV12 matrix/range and physical grey chart not certified; colourAssumptions preserved");
            jsonSet(report, "secondsMeasured", elapsed); jsonSet(report, "durationComplete", completed); jsonSet(report, "cancelled", cancelled);
            jsonSet(report, "processCpuPercentOneCore", elapsed > 0 ? static_cast<double>(cpu) / (elapsed * 100000.0) : 0.0);
            jsonSet(report, "cpuMetricDefinition", "GetProcessTimes user+kernel including capture, preview, record and mux workers; 100% = one logical core; preparation/shutdown excluded after successful open");
            const double limit = mode.fps.value() > 45 ? 35 : 45;
            const auto& latency = telemetry->distribution(Timing::callbackToPresent);
            const bool previewPass = latency.count() && latency.percentile(.95) <= limit && static_cast<int>(presenter.adapterJson()["displayRefreshHz"]) >= 60;
            jsonSet(report, "callbackToPresentP95LimitMs", limit); jsonSet(report, "previewBudgetMet", previewPass);
            jsonSet(report, "opticalP0", "UNAVAILABLE: no optical latency or independent source-pattern oracle; sourceId CSV records callback identity only");
            jsonSet(report, "headroom", "UNAVAILABLE in real-time mode: run encode --synthetic separately; encoder.serviceFps is not a maximum-speed throughput measurement");
            const bool clean = telemetry->softwareLossFree() && !telemetry->count(LossReason::timestampRegression);
            if (!opened || !telemetry->samples.load()) status(report, "UNAVAILABLE", failure.empty() ? "Hardware delivered no samples" : failure);
            else if (!completed || !failure.empty() || !clean || !previewPass || !static_cast<bool>(encoded["complete"]))
                status(report, "FAIL", failure.empty() ? "Inspect software loss, encode/mux errors, duration and preview budget; device cadence/stall remain separate observations" : failure);
            else status(report, "PASS", "Measured software capture/preview/record contract; full decode, physical source continuity, optical P0 and two-camera headroom require external validation");
        }
    }
    catch (const std::invalid_argument& e) { status(report, "FAIL", e.what()); }
    catch (const std::exception& e) { status(report, "UNAVAILABLE", e.what()); }
    if (args.has("--report")) CaptureTelemetry::writeJson(filePath(args.required("--report")), report);
    else std::cout << juce::JSON::toString(report, false) << '\n';
    std::cout << report["result"].toString() << ": " << report["reason"].toString() << '\n';
    return report["result"].toString() == "PASS" ? 0 : report["result"].toString() == "UNAVAILABLE" ? 2 : 1;
}
}
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
    Arguments args;
    auto report = jsonObject();
    try
    {
        if (argc >= 2 && juce::String(argv[1]) == "asio") return runAsioProbe(argc, argv);
        if (argc >= 2 && juce::String(argv[1]) == "playback") return runPlaybackProbe(argc, argv);
        args = parse(argc, argv);
        report = baseReport(args, &report); // also covers encode's early runtime check without changing its function
        return args.command == "enumerate" ? enumerate(args) : args.command == "encode" ? encodeCommand(args)
            : args.command == "mf-jpeg-range" ? mfJpegRangeCommand(args) : captureCommand(args);
    }
    catch (const std::exception& e)
    {
        status(report, "FAIL", e.what());
        try
        {
            const auto out = args.get("--report", args.get("--out"));
            if (!out.empty()) CaptureTelemetry::writeJson(filePath(out), report);
        }
        catch (const std::exception& writeError) { std::cerr << "Report write failed: " << writeError.what() << '\n'; }
        std::cerr << juce::JSON::toString(report, false) << '\n'; return 1;
    }
}
