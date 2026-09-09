#include "record/DubbingController.h"
#include "playback/ImportedAudioCache.h"
#include "record/Mp4TakeWriter.h"
#include <juce_events/juce_events.h>
#include <charconv>
#include <iostream>
#include <map>
#include <set>

namespace gocue::recorder
{
namespace
{
void check(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
Sample integer(const juce::String& text)
{
    const auto str = text.toStdString(); Sample n = -1; const auto parsed = std::from_chars(str.data(), str.data() + str.size(), n);
    if (parsed.ec != std::errc{} || parsed.ptr != str.data() + str.size()) throw std::invalid_argument("Invalid integer argument"); return n;
}
juce::File path(const juce::String& p) { return juce::File::isAbsolutePath(p) ? juce::File(p) : juce::File::getCurrentWorkingDirectory().getChildFile(p); }
void pump()
{
    MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Sleep(1);
}

}
int runDubbingProbe(int argc, wchar_t** argv)
{
    auto report = jsonObject(); juce::String reportPath; int exitCode = 1; juce::Array<juce::var> runs;
    try
    {
        std::map<juce::String, juce::String> args;
        const std::set<juce::String> options{"--devices", "--asio-device", "--audio-file", "--pstart", "--mic-modes", "--seconds", "--project-dir", "--report", "--inputs", "--outputs", "--buffer-size", "--test-offsets"};
        for (int i = 2; i < argc; ++i)
        {
            const juce::String key(argv[i]);
            if (!options.count(key) || args.count(key)) throw std::invalid_argument("Unknown or duplicate dubbing option");
            if (key == "--test-offsets") args[key] = "true";
            else { if (++i >= argc) throw std::invalid_argument("Missing dubbing option value"); args[key] = juce::String(argv[i]); }
            if (key == "--report") reportPath = args[key];
        }
        for (const char* key : {"--devices", "--asio-device", "--audio-file", "--pstart", "--project-dir", "--report"})
            if (!args.count(key) || args[key].isEmpty()) throw std::invalid_argument(std::string("Required option: ") + key);
        const auto pstart = integer(args["--pstart"]), deviceIndex = integer(args["--asio-device"]);
        const auto seconds = integer(args.count("--seconds") ? args["--seconds"] : "60");
        if (pstart < 0 || deviceIndex < 0 || seconds < 1 || seconds > 3600) throw std::invalid_argument("pstart/device must be nonnegative; seconds must be 1..3600");
        const auto modes = juce::StringArray::fromTokens(args.count("--mic-modes") ? args["--mic-modes"] : "off", ",", "");
        if (modes.isEmpty() || modes.size() > 2) throw std::invalid_argument("mic-modes must be off, on or off,on");
        std::set<juce::String> unique; for (const auto& m : modes) if ((m != "off" && m != "on") || !unique.insert(m).second) throw std::invalid_argument("Invalid mic mode");
        juce::var devices; check(juce::JSON::parse(path(args["--devices"]).loadFileAsString(), devices));
        if (int(devices["schemaVersion"]) != 1) throw std::invalid_argument("devices.json must use schemaVersion 1");
        juce::ScopedJuceInitialiser_GUI runtime;
        const auto names = RecorderAudioEngine::deviceNames();
        if (deviceIndex >= names.size()) { exitCode = 2; throw std::runtime_error("Selected ASIO device is unavailable"); }
        jsonSet(report, "schemaVersion", 1); jsonSet(report, "sourceKind", "hardware-ASIO-and-MF-cameras"); jsonSet(report, "asioDevice", names[int(deviceIndex)]);
        jsonSet(report, "secondsRequested", seconds); jsonSet(report, "physicalSync", "UNVERIFIED: no optical/loopback oracle in this probe");
        for (const auto& mode : modes)
        {
            RecorderAudioEngine audio; std::array<int, 8> mapping{-1,-1,-1,-1,-1,-1,-1,-1};
            if (mode == "on")
            {
                const auto inputs = juce::StringArray::fromTokens(args.count("--inputs") ? args["--inputs"] : "1", ",", "");
                if (inputs.isEmpty() || inputs.size() > 8) throw std::invalid_argument("Select 1..8 microphone inputs");
                for (int i = 0; i < inputs.size(); ++i) { const auto n = integer(inputs[i]); if (n < 1 || n > 256) throw std::invalid_argument("Input index outside range"); mapping[size_t(i)] = int(n - 1); }
            }
            check(audio.setInputMap(mapping)); OutputMapping output;
            const auto channels = juce::StringArray::fromTokens(args.count("--outputs") ? args["--outputs"] : "1:2", ":", "");
            for (const auto& channel : channels) if (integer(channel) < 1 || integer(channel) > 256) throw std::invalid_argument("Output index must be 1..256");
            if (channels.size() == 1) { output.mono = true; output.monoChannel = int(integer(channels[0]) - 1); }
            else if (channels.size() == 2) { output.left = int(integer(channels[0]) - 1); output.right = int(integer(channels[1]) - 1); }
            else throw std::invalid_argument("outputs must be L:R or one mono output");
            check(audio.setOutputMap(output));
            const auto buffer = args.count("--buffer-size") ? integer(args["--buffer-size"]) : 0;
            if (buffer < 0 || buffer > 16384) throw std::invalid_argument("Buffer size must be 0..16384");
            const auto opened = audio.openDevice(names[int(deviceIndex)], 48000, int(buffer));
            if (opened.failed()) { exitCode = 2; check(opened); }
            for (unsigned i = 0; i < 8; ++i) if (mapping[i] >= 0) check(audio.arm(i, true));
            RecorderDocument document; document.newProject("Dubbing probe " + mode, audio.deviceInfo().sampleRate, {60,1});
            const auto directory = path(args["--project-dir"]).getChildFile("mic-" + mode);
            if (directory.getChildFile("project.recorder").existsAsFile()) throw std::invalid_argument("Use a fresh probe project directory");
            AudioImportControl control; std::unique_ptr<PreparedAudioImport> imported;
            check(AudioImport::prepare({path(args["--audio-file"]), directory, document.getProject().projectId, document.getProject().Fs, 0}, control, imported));
            const auto trackId = imported->track().trackId; check(commitImportedAudio(document, *imported, control));
            DubbingController controller(document, audio); DubbingController::Config c;
            c.projectDirectory = directory; c.audioTrackId = trackId; c.Pstart = pstart; c.spanSamples = seconds * document.getProject().Fs; c.recordMicrophones = mode == "on";
            for (const char* name : {"cam1", "cam2"})
            {
                const auto selected = devices["selections"][name];
                if (selected["symbolicLink"].toString().isEmpty()) { if (juce::String(name) == "cam1") throw std::invalid_argument("Select cam1 in devices.json"); continue; }
                DubbingController::Camera cam; cam.symbolicLink = selected["symbolicLink"].toString().toStdString(); cam.mode = CameraMode::parse(selected["mode"].toString().toStdString()); c.cameras.push_back(cam);
            }
            check(controller.prepare(c)); const auto deadline = qpcNow() + qpcFrequency() * (seconds + 90); bool started = false, timeout = false;
            while (controller.locked())
            {
                controller.tick(); if (!started && controller.state() == DubbingController::State::armed) { check(controller.start()); started = true; }
                if (!timeout && qpcNow() > deadline) { controller.abort(); timeout = true; }
                pump();
            }
            auto run = controller.report(); jsonSet(run, "micMode", mode); jsonSet(run, "projectDirectory", directory.getFullPathName());
            bool passed = controller.state() == DubbingController::State::done && controller.placement().Pstart == pstart
                && controller.placement().recordedSamples == c.spanSamples;
            if (const auto* videos = run["videos"].getArray())
                for (const auto& video : *videos)
                    passed = passed && Sample(video["mux"]["videoPackets"]) == Sample(run["expectedVideoFrames"])
                        && bool(video["inspection"]["presentationStartsAtZero"]);
            else passed = false;
            if (mode == "on") passed = passed && Sample(run["audio"]["wav"]["writtenSamplesPerMic"]) == c.spanSamples;
            if (args.count("--test-offsets")) { const auto checkOffsets = controller.calibrationOffsetReport(); jsonSet(run, "offsetCheck", checkOffsets); passed = passed && checkOffsets["status"].toString() == "PASS"; }
            jsonSet(run, "status", passed ? "PASS" : "FAIL"); runs.add(run);
            if (!passed) throw std::runtime_error("Dubbing partially failed; captured prefix and report preserved");
        }
        exitCode = 0; jsonSet(report, "status", "PASS");
    }
    catch (const std::exception& e) { jsonSet(report, "status", exitCode == 2 ? "UNAVAILABLE" : "FAIL"); jsonSet(report, "reason", e.what()); }
    jsonSet(report, "runs", runs);
    try { if (reportPath.isNotEmpty()) CaptureTelemetry::writeJson(path(reportPath), report); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; exitCode = 1; }
    std::cout << juce::JSON::toString(report, true) << '\n'; return exitCode;
}
}
