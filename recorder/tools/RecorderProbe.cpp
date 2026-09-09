#include "capture/MfCameraCapture.h"
#include "video/PreviewPresenter.h"
#include "video/PresentPacing.h"
#include "diagnostics/MfJpegRangeProbe.h"
#include "record/EncodePipeline.h"
#include "audio/AsioProbe.h"
#include "media/AudioImport.h"
#include "playback/ImportedAudioCache.h"
#include "audio/MediaFoundationAudioFormat.h"
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
#include <cmath>
#include <limits>

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
namespace
{
// Round 16 probe only: synthetic source/encoder orchestration and measured oracles.
// Import, resampling, peaks and document publication all use the production classes.
float importFixtureSignal(Sample sample, std::uint32_t rate, int channel)
{
    const double t = static_cast<double>(sample) / rate;
    return static_cast<float>(.28 * std::sin(6.283185307179586 * ((211 + channel * 157) * t + 271 * t * t))
        + .11 * std::sin(6.283185307179586 * (997 + channel * 263) * t));
}
void importProbeCheck(bool ok, const juce::String& reason)
{ if (!ok) throw std::runtime_error(reason.toStdString()); }
void importProbeCheck(const juce::Result& r) { importProbeCheck(r.wasOk(), r.getErrorMessage()); }
void writeImportFixture(const juce::File& file, std::uint32_t rate, Sample length)
{
    importProbeCheck(!file.exists(), "Fixture already exists");
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream(); juce::WavAudioFormat format;
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(32));
    importProbeCheck(writer != nullptr, "Cannot create WAV fixture"); juce::AudioBuffer<float> block(2, 4096);
    for (Sample at = 0; at < length;)
    {
        const auto n = static_cast<int>((std::min)(Sample(4096), length - at));
        for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < n; ++s) block.setSample(ch, s, importFixtureSignal(at + s, rate, ch));
        importProbeCheck(writer->writeFromAudioSampleBuffer(block, 0, n), "WAV fixture write failure"); at += n;
    }
    importProbeCheck(writer->flush(), "WAV fixture flush failure");
}
int importAudioCommand(int argc, wchar_t** argv)
{
    Arguments args; args.command = "import-audio";
    for (int i = 0; i < argc; ++i) args.original.add(juce::String(argv[i]));
    auto report = jsonObject();
    try
    {
        for (int i = 2; i < argc; ++i)
        {
            const auto key = juce::String(argv[i]).toStdString();
            importProbeCheck((key == "--fixture-set" || key == "--project-dir" || key == "--report") && i + 1 < argc && !args.has(key), "Unknown, duplicate or incomplete import-audio option");
            args.values.emplace(key, juce::String(argv[++i]).toStdString());
        }
        report = baseReport(args); jsonSet(report, "sourceKind", "synthetic");
        importProbeCheck(args.required("--fixture-set") == "wav-mp3-m4a", "Supported fixture set: wav-mp3-m4a");
        const auto projectDirectory = filePath(args.required("--project-dir"));
        args.required("--report"); importProbeCheck(projectDirectory.createDirectory());
        const auto fixturesDirectory = projectDirectory.getSiblingFile(projectDirectory.getFileName() + "-fixtures-" + newId());
        importProbeCheck(fixturesDirectory.createDirectory());
        RecorderDocument document;
        const auto checkpoint = projectDirectory.getChildFile("project.recorder");
        if (checkpoint.existsAsFile()) importProbeCheck(document.openCheckpoint(checkpoint));
        const auto projectFs = document.getProject().Fs;
        jsonSet(report, "projectDirectory", projectDirectory.getFullPathName()); jsonSet(report, "projectFs", static_cast<int>(projectFs));
        jsonSet(report, "deviceOpened", false); jsonSet(report, "originalPreservation", "SHA-256 of external original and copied original before/after cache");
        jsonSet(report, "waveformIntegration", "project-Fs min/max bins; round-09 PeakCache/MediaIndex absent in this branch");
        juce::Array<juce::var> rows; bool failed = false, unavailable = false;
        for (const auto& name : {juce::String("wav-44100"), juce::String("wav-48000"), juce::String("wav-96000"), juce::String("mp3-48000"), juce::String("m4a-48000")})
        {
            auto row = jsonObject(); jsonSet(row, "fixture", name); const auto rate = static_cast<std::uint32_t>(name.fromLastOccurrenceOf("-", false, false).getIntValue());
            const Sample length = Sample(rate) * 2 + 137; const bool compressed = !name.startsWith("wav");
            const auto wav = fixturesDirectory.getChildFile(name + "-source.wav");
            try
            {
                writeImportFixture(wav, rate, length); auto source = wav;
                if (compressed)
                {
                    source = fixturesDirectory.getChildFile(name + (name.startsWith("mp3") ? ".mp3" : ".m4a"));
                    juce::StringArray command{RECORDER_IMPORT_FFMPEG_EXE, "-hide_banner", "-loglevel", "error", "-nostdin", "-n", "-i", wav.getFullPathName(), "-map", "0:a:0", "-c:a", name.startsWith("mp3") ? "libmp3lame" : "aac", "-b:a", "192k"};
                    if (name.startsWith("m4a")) command.addArray({"-movie_timescale", juce::String(rate)});
                    command.add(source.getFullPathName()); juce::Array<juce::var> commandJson; for (const auto& s : command) commandJson.add(s);
                    jsonSet(row, "fixtureCommand", commandJson); juce::ChildProcess process;
                    bool generated = process.start(command);
                    if (generated && !process.waitForProcessToFinish(20000)) { process.kill(); generated = false; }
                    const auto log = process.readAllProcessOutput(); const auto exitCode = process.getExitCode();
                    jsonSet(row, "fixtureEncoderExitCode", static_cast<int>(exitCode)); jsonSet(row, "fixtureEncoderLog", log);
                    if (!generated || exitCode != 0 || !source.existsAsFile())
                    { status(row, "UNAVAILABLE", "Fixture encoder could not generate this format; see command/log"); unavailable = true; rows.add(row); continue; }
                }
                AudioImportRequest request; request.source = source; request.projectDirectory = projectDirectory;
                request.projectId = document.getProject().projectId; request.projectFs = projectFs; request.playhead = 12347;
                ImportedAudioCache::Worker worker(request);
                const auto started = std::chrono::steady_clock::now();
                while (!worker.finished() && std::chrono::steady_clock::now() - started < std::chrono::seconds(60)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (!worker.finished()) worker.cancel();
                std::unique_ptr<PreparedAudioImport> prepared; CachedImportedAudio cache; importProbeCheck(worker.takeResult(prepared, cache));
                const auto& info = prepared->info(); jsonSet(row, "source", source.getFullPathName()); jsonSet(row, "metadata", info.toVar());
                jsonSet(row, "expectedOriginalSamples", juce::int64(length)); jsonSet(row, "cacheSamples", juce::int64(cache.samples));
                jsonSet(row, "cachePcm", cache.pcmFile.getFullPathName()); jsonSet(row, "peakBins", static_cast<int>(cache.peaks.size()));
                jsonSet(row, "seconds", static_cast<double>(cache.samples) / projectFs);
                auto pcm = AudioImport::openReader(cache.pcmFile); juce::AudioBuffer<float> samples(2, static_cast<int>(cache.samples));
                importProbeCheck(pcm->read(&samples, 0, static_cast<int>(cache.samples), 0, true, true), "Cannot read derived PCM");
                double squared = 0; int count = 0;
                for (int ch = 0; ch < 2; ++ch) for (int at = 128; at < cache.samples - 128; at += 17)
                { squared += std::pow(samples.getSample(ch, at) - importFixtureSignal(at, projectFs, ch), 2); ++count; }
                const auto rms = std::sqrt(squared / count); jsonSet(row, "oracleRmsError", rms);
                for (const bool head : {true, false})
                {
                    double error = 0; int n = 0;
                    const int begin = head ? 64 : static_cast<int>(cache.samples - 4096);
                    const int end = head ? 4096 : static_cast<int>(cache.samples - 64);
                    for (int ch = 0; ch < 2; ++ch) for (int at = begin; at < end; at += 13)
                    { error += std::pow(samples.getSample(ch, at) - importFixtureSignal(at, projectFs, ch), 2); ++n; }
                    jsonSet(row, head ? "headRmsError" : "tailRmsError", std::sqrt(error / n));
                }
                int bestDelay = 0; double bestError = (std::numeric_limits<double>::max)();
                for (int delay = -1400; delay <= 1400; ++delay)
                {
                    double error = 0;
                    for (int centre : {4096, static_cast<int>(cache.samples / 2), static_cast<int>(cache.samples - 4096)})
                        for (int s = 0; s < 512; s += 17)
                        { const int at = centre + s; error += std::pow(samples.getSample(0, at + delay) - importFixtureSignal(at, projectFs, 0), 2); }
                    if (error < bestError) { bestError = error; bestDelay = delay; }
                }
                jsonSet(row, "measuredDelaySamples", bestDelay); jsonSet(row, "delaySearchRangeSamples", 1400);
                double seekMaxError = 0;
                // At equal Fs these compare the shared source reader directly against sequential cache output.
                if (rate == projectFs)
                {
                    auto reader = AudioImport::openReader(prepared->originalFile()); juce::AudioBuffer<float> seek(2, 257);
                    for (Sample at : {Sample(45007), Sample(2049), length - 257, Sample(17)})
                    {
                        importProbeCheck(reader->read(&seek, 0, 257, info.readerStartSample + at, true, true), "Source reader seek failed");
                        for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < 257; ++s)
                            seekMaxError = (std::max)(seekMaxError, std::abs(double(seek.getSample(ch, s) - samples.getSample(ch, static_cast<int>(at) + s))));
                    }
                    jsonSet(row, "sourceSeekMaxError", seekMaxError);
                    if (name.startsWith("m4a"))
                    {
                        gocue::MediaFoundationAudioFormat format; auto stream = prepared->originalFile().createInputStream();
                        std::unique_ptr<juce::AudioFormatReader> native(format.createReaderFor(stream.release(), true));
                        importProbeCheck(native != nullptr, "Shared MF diagnostic reader unavailable"); double nativeError = 0;
                        for (Sample at : {Sample(45007), Sample(2049), length - 257, Sample(17)})
                        {
                            importProbeCheck(native->read(&seek, 0, 257, at, true, true), "Shared MF diagnostic seek failed");
                            for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < 257; ++s)
                                nativeError = (std::max)(nativeError, std::abs(double(seek.getSample(ch, s) - samples.getSample(ch, static_cast<int>(at) + s))));
                        }
                        jsonSet(row, "sharedMfSeekMaxErrorDiagnostic", nativeError);
                        jsonSet(row, "sourceSeekPolicy", "Import-only wrapper decodes forward sequentially; backwards seek reopens. Shared MF reader unchanged. Playback seeks PCM.");
                    }
                }
                else jsonSet(row, "sourceSeekCheck", "not compared across different sample rates; rational mapping and PCM oracle checked");
                juce::Array<juce::var> mapping;
                for (Sample at : {Sample(0), Sample(1), Sample(10007), cache.samples})
                { auto m = jsonObject(); jsonSet(m, "projectSourceSample", juce::int64(at)); jsonSet(m, "originalPresentationSample", juce::int64(ImportedAudioCache::sourceSampleFor(at, info, projectFs))); mapping.add(m); }
                jsonSet(row, "sampleMapping", mapping);
                const bool unchanged = AudioImport::hashFile(source, worker.control) == info.contentHash && AudioImport::hashFile(prepared->originalFile(), worker.control) == info.contentHash;
                jsonSet(row, "sourceHashUnchanged", unchanged);
                importProbeCheck(commitImportedAudio(document, *prepared, worker.control));
                jsonSet(row, "assetId", prepared->asset().assetId); jsonSet(row, "trackKind", "importAudio"); jsonSet(row, "playhead", juce::int64(prepared->clip().timelineStartSample));
                const bool pass = unchanged && document.getProject().validate().wasOk() && info.decodedSamples == length
                    && cache.samples == rescaleRound(length, projectFs, rate) && std::abs(bestDelay) <= 1
                    && rms < (compressed ? .025 : .0001) && seekMaxError < .002
                    && static_cast<double>(row["headRmsError"]) < (compressed ? .035 : .0001)
                    && static_cast<double>(row["tailRmsError"]) < (compressed ? .035 : .0001);
                status(row, pass ? "PASS" : "FAIL", pass ? "Measured synthetic length, mapping, priming, PCM oracle and available source seek checks" : "Length/alignment/seek/original check failed; inspect measured fields");
                failed |= !pass;
            }
            catch (const std::exception& e) { status(row, "FAIL", e.what()); failed = true; }
            rows.add(row);
        }
        importProbeCheck(document.saveCheckpoint(checkpoint)); jsonSet(report, "fixtures", rows);
        jsonSet(report, "unverified", "Real DRM files, other Windows codec versions and hardware ASIO playback; MainComponent and round-09 PeakCache wiring deferred");
        status(report, failed ? "FAIL" : unavailable ? "UNAVAILABLE" : "PASS", "Synthetic import/cache verification; no audio device opened");
        CaptureTelemetry::writeJson(filePath(args.required("--report")), report);
        std::cout << report["result"].toString() << ": " << args.required("--report") << '\n';
        return failed ? 1 : unavailable ? 2 : 0;
    }
    catch (const std::exception& e)
    {
        status(report, "FAIL", e.what());
        if (args.has("--report")) try { CaptureTelemetry::writeJson(filePath(args.get("--report")), report); } catch (...) {}
        std::cerr << juce::JSON::toString(report) << '\n'; return 1;
    }
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
        if (argc >= 2 && juce::String(argv[1]) == "import-audio") return importAudioCommand(argc, argv);
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
