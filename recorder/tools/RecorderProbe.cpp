#include "capture/MfCameraCapture.h"
#include "video/PreviewPresenter.h"
#include "audio/AsioProbe.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
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
    if (argc < 2) throw std::invalid_argument("RecorderProbe enumerate --select --out devices.json | capture --devices devices.json --camera cam1 [--mode \"NV12 1920x1080 60/1\"] [--compare-decoders] [--decoder-threads 1] --seconds N --report capture.json");
    args.command = juce::String(argv[1]).toStdString();
    const std::set<std::string> allowedFlags = args.command == "enumerate" ? std::set<std::string>{"--select"} : std::set<std::string>{"--compare-decoders"};
    const std::set<std::string> allowedValues = args.command == "enumerate" ? std::set<std::string>{"--out"}
        : std::set<std::string>{"--devices", "--camera", "--mode", "--seconds", "--report", "--decoder-threads", "--decoder"};
    if (args.command != "enumerate" && args.command != "capture") throw std::invalid_argument("Unknown subcommand: " + args.command);
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
juce::var baseReport(const Arguments& args)
{
    auto value = jsonObject(), sdk = jsonObject();
    jsonSet(value, "schemaVersion", 1); jsonSet(value, "command", args.original);
    jsonSet(value, "sourceKind", "hardware"); jsonSet(value, "startedUtc", utcNowIso8601());
    jsonSet(value, "os", juce::SystemStats::getOperatingSystemName());
    jsonSet(sdk, "expectedVersion", RECORDER_FFMPEG_VERSION); jsonSet(sdk, "loadedVersion", av_version_info());
    jsonSet(sdk, "avcodecVersion", jsonInt(avcodec_version())); jsonSet(sdk, "avutilVersion", jsonInt(avutil_version()));
    jsonSet(sdk, "swscaleVersion", jsonInt(swscale_version())); jsonSet(sdk, "configuration", avcodec_configuration());
    jsonSet(value, "sdk", sdk);
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
juce::var selectCamera(const std::vector<CameraDevice>& devices, const char* camera, bool optional)
{
    const auto choice = readChoice(std::string(camera) + " device number" + (optional ? " (0=off): " : ": "), devices.size(), optional);
    if (!choice) return {};
    const auto& device = devices[choice - 1];
    if (!device.unavailableReason.empty()) throw std::runtime_error(device.unavailableReason);
    for (size_t i = 0; i < device.modes.size(); ++i) std::cout << "  " << i + 1 << ") " << device.modes[i].text() << '\n';
    const auto mode = readChoice("Native mode number (keep its exact rational FPS): ", device.modes.size(), false);
    auto value = jsonObject();
    jsonSet(value, "symbolicLink", device.symbolicLink); jsonSet(value, "friendlyName", device.friendlyName);
    jsonSet(value, "mode", device.modes[mode - 1].text());
    return value;
}
int enumerate(const Arguments& args)
{
    const auto out = filePath(args.required("--out"));
    auto report = baseReport(args);
    try
    {
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
            jsonSet(selections, "cam1", selectCamera(devices, "cam1", false));
            if (devices.size() > 1)
            {
                const auto second = selectCamera(devices, "cam2", true);
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
    catch (const std::exception& e) { status(report, "UNAVAILABLE", e.what()); }
    CaptureTelemetry::writeJson(out, report);
    std::cout << report["result"].toString() << ": " << report["reason"].toString() << '\n';
    return report["result"].toString() == "PASS" ? 0 : 2;
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
    jsonSet(result, "cpuDecodeDefinition", mfDecode ? "Post-MF NV12 pass-through only. Internal MF MJPEG decode duration is UNAVAILABLE (happens before callback)."
        : "FFmpeg avcodec_send_packet/receive_frame including padded packet copy; native raw binding for NV12/YUY2. Separate from colour normalisation.");
    jsonSet(result, "callbackToPresentDefinition", mfDecode ? "Decoded NV12 SourceReader callback entry to successful Present submission; excludes internal MF decode."
        : "Native SourceReader callback entry to successful Present submission; includes worker decode and colour normalisation.");
    auto telemetry = std::make_shared<CaptureTelemetry>(mode.fps);
    VideoSurfacePool pool(mode.width, mode.height);
    PreviewPresenter presenter(window.handle, pool, telemetry, mode.width, mode.height);
    MfCameraCapture capture(telemetry, pool);
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
        auto presentReady = std::async(std::launch::async, [&] { presenter.start(); });
        while (presentReady.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        { cancelled |= !window.pump(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT); }
        presentReady.get();
        if (cancelled) throw std::runtime_error("User closed preview during preparation");
        const auto info = capture.start(link, mode, mfDecode, threads);
        jsonSet(result, "nativeMode", CameraCatalog::modeJson(info.nativeMode));
        jsonSet(result, "outputMode", CameraCatalog::modeJson(info.outputMode));
        jsonSet(result, "decoder", info.decoderName); jsonSet(result, "effectiveDecoderThreads", info.decoderThreads);
        opened = true; start = std::chrono::steady_clock::now(); cpuStart = processCpu100ns();
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
    bool clean = true;
    for (const auto reason : {LossReason::captureDecodeOverflow, LossReason::lateQueueDiscard, LossReason::timestampRegression,
        LossReason::sourceCadenceGap, LossReason::sourceStreamTick, LossReason::sourceDiscontinuity, LossReason::sourceError, LossReason::sourceTypeChanged,
        LossReason::endOfStream, LossReason::decoderError, LossReason::surfacePoolExhausted, LossReason::uploadBusy,
        LossReason::presentFailure, LossReason::previewStall}) clean &= telemetry->count(reason) == 0;
    const auto& latency = telemetry->distribution(Timing::callbackToPresent);
    const bool latencyPass = latency.count() > 0 && latency.percentile(0.95) <= p95Limit;
    const int hz = presenter.adapterJson()["displayRefreshHz"];
    if (!opened || telemetry->samples.load() == 0) status(result, "UNAVAILABLE", failure.empty() ? "No delivered hardware samples" : failure);
    else if (!failure.empty() || !durationComplete || !clean || !latencyPass || hz < 60)
        status(result, "FAIL", failure.empty() ? "Software observation gate failed: inspect losses, callback-to-Present latency and display refresh (requires >=60Hz)" : failure);
    else status(result, "PASS", mfDecode ? "Measured post-MF-callback software budget only; MF decode/optical/source loss certification remains unavailable"
        : "Measured software callback-to-Present budget; optical/source loss certification remains unavailable");
    return result;
}
int captureCommand(const Arguments& args)
{
    const auto out = filePath(args.required("--report"));
    auto report = baseReport(args);
    try
    {
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
            runs.add(runCapture(link, mode, false, static_cast<int>(threads), seconds / 2, window));
            if (!static_cast<bool>(runs.getLast()["cancelled"])) runs.add(runCapture(link, mode, true, static_cast<int>(threads), seconds - seconds / 2, window));
        }
        else
        {
            // 2026-09-09 measurement (GC311G2 MJPEG 1080p60, 60 s): MF MJPEG->NV12 callback-to-Present p95 8 ms at 31% of a core;
            // the FFmpeg CPU path spent ~20 ms/frame in decode + swscale colour normalisation and dropped 9%. MJPEG therefore
            // defaults to the MF decoder; raw NV12/YUY2 modes stay on the native path. Override with --decoder mf|ffmpeg.
            const auto decoder = args.get("--decoder", mode.subtype == CaptureSubtype::mjpeg ? "mf" : "ffmpeg");
            if (decoder != "mf" && decoder != "ffmpeg") throw std::invalid_argument("--decoder must be mf or ffmpeg");
            if (decoder == "mf" && mode.subtype != CaptureSubtype::mjpeg) throw std::invalid_argument("--decoder mf requires an MJPEG native mode");
            runs.add(runCapture(link, mode, decoder == "mf", static_cast<int>(threads), seconds, window));
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
}
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
    try
    {
        if (argc >= 2 && juce::String(argv[1]) == "asio") return runAsioProbe(argc, argv);
        const auto args = parse(argc, argv);
        return args.command == "enumerate" ? enumerate(args) : captureCommand(args);
    }
    catch (const std::exception& e) { std::cerr << "RecorderProbe: " << e.what() << '\n'; return 1; }
}
