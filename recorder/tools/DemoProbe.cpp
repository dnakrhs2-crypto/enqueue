#include "support/Platform.h"
#include "model/SafeFileWrite.h"
#include "model/RecorderModel.h"
#include <map>
#include <charconv>
#include <iostream>
#include <algorithm>

int runDemoProbe(int argc, wchar_t** argv)
{
    using namespace gocue::recorder; juce::File reportFile; auto report = jsonObject();
    try
    {
        std::map<juce::String, juce::String> args; bool twoCameras = false;
        // Find report independently so option/capability failures leave evidence.
        for (int i = 2; i + 1 < argc; ++i) if (juce::String(argv[i]) == "--report") reportFile = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[i + 1]));
        for (int i = 2; i < argc; ++i)
        {
            const juce::String flag(argv[i]);
            if (flag == "--two-cameras" && !twoCameras) { twoCameras = true; continue; }
            if (i + 1 >= argc || (flag != "--app" && flag != "--devices" && flag != "--asio-device" && flag != "--iterations" && flag != "--report" && flag != "--timeline") || args.count(flag)) throw std::invalid_argument("Invalid demo arguments");
            args[flag] = juce::String(argv[++i]); if (flag == "--report") reportFile = juce::File::getCurrentWorkingDirectory().getChildFile(args[flag]);
        }
        if (twoCameras)
        {
            if (args["--app"].isEmpty() || reportFile == juce::File()) throw std::invalid_argument("demo requires --app and --report");
            const auto app = juce::File::getCurrentWorkingDirectory().getChildFile(args["--app"]);
            if (!app.existsAsFile()) throw std::invalid_argument("App file missing");
            const auto capabilities = juce::JSON::parse(app.getSiblingFile("Recorder.demo-capabilities.json"));
            if (int(capabilities["schemaVersion"]) != 1 || !bool(capabilities["dualCameraReceipts"]))
            {
                jsonSet(report, "schemaVersion", 1); jsonSet(report, "status", "UNAVAILABLE"); jsonSet(report, "stage", "demo-capability");
                jsonSet(report, "twoCamerasRequired", true); jsonSet(report, "app", app.getFullPathName());
                jsonSet(report, "reason", "This app's demo enables cam1 only and records only cam1 first Present. Round-27 app integration must enable both selected cameras and publish per-camera first Present receipts. Round-26 permitted paths do not include that app coordinator.");
                jsonSet(report, "iterationsCompleted", 0); jsonSet(report, "measurementPerformed", false);
                const auto saved = gocue::SafeFileWrite::writeTextVerified(reportFile, juce::JSON::toString(report, false));
                if (saved.failed()) throw std::runtime_error(saved.getErrorMessage().toStdString());
                std::cout << "Recorder dual demo UNAVAILABLE: app has no dual-camera receipt capability\n"; return 2;
            }
        }
        for (const auto* flag : {"--app", "--devices", "--asio-device", "--report"}) if (args[flag].isEmpty()) throw std::invalid_argument("demo needs --app --devices --asio-device --report");
        const auto integer = [](const juce::String& s) { const auto text = s.toStdString(); int n = -1; auto r = std::from_chars(text.data(), text.data() + text.size(), n); if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) throw std::invalid_argument("Invalid integer"); return n; };
        const auto count = args["--iterations"].isEmpty() ? 20 : integer(args["--iterations"]), asio = integer(args["--asio-device"]);
        if (count < 1 || count > 1000 || asio < 0) throw std::invalid_argument("Invalid demo count/device index");
        const auto app = juce::File::getCurrentWorkingDirectory().getChildFile(args["--app"]), devices = juce::File::getCurrentWorkingDirectory().getChildFile(args["--devices"]);
        if (!app.existsAsFile() || !devices.existsAsFile()) throw std::invalid_argument("App/devices file missing");
        const auto isolated = reportFile.getParentDirectory().getChildFile("demo-settings-" + juce::Uuid().toString());
        const auto runReport = isolated.getChildFile("measurement.json");
        juce::StringArray command {app.getFullPathName(), "--automation", "--self-test-record", juce::String(count), "--devices", devices.getFullPathName(), "--asio-device", juce::String(asio), "--report", runReport.getFullPathName(), "--test-root", isolated.getFullPathName()};
        if (args["--timeline"].isNotEmpty()) { command.add("--demo-timeline"); command.add(args["--timeline"]); } // gap | overwrite: timeline-tab takes from the second iteration
        if (twoCameras)
        {
            const auto selection = juce::JSON::parse(devices)["selections"];
            if (selection["cam2"]["symbolicLink"].toString().isEmpty()
                || selection["cam1"]["symbolicLink"].toString() == selection["cam2"]["symbolicLink"].toString())
                throw std::invalid_argument("Dual demo needs two distinct selected cameras");
            command.add("--two-cameras");
        }
        juce::ChildProcess process; if (!process.start(command, 0)) throw std::runtime_error("Could not launch Recorder demo");
        if (!process.waitForProcessToFinish((count * 60 + 90) * 1000)) { process.kill(); throw std::runtime_error("Recorder demo timed out"); }
        if (!runReport.existsAsFile()) throw std::runtime_error("Recorder did not write this run's report");
        auto result = juce::JSON::parse(runReport); auto status = result["status"].toString();
        if (twoCameras && status == "PASS")
        {
            const auto* rows = result["iterations"].getArray(); bool measured = rows && rows->size() == count;
            if (rows) for (const auto& row : *rows)
            {
                const auto* video = row["firstVideoPresentQpcByCamera"].getArray();
                const auto stop = Sample(row["stopButtonQpc"]); const auto clipMs = double(row["stopToClipMs"]);
                measured &= video && video->size() == 2 && stop > 0 && row["stopToClipMs"].isDouble() && clipMs >= 0 && clipMs <= 250;
                if (video && video->size() == 2)
                {
                    const auto first = (std::max)({Sample((*video)[0]), Sample((*video)[1]), Sample(row["firstAudibleQpc"])});
                    measured &= Sample((*video)[0]) >= stop && Sample((*video)[1]) >= stop && Sample(row["firstAudibleQpc"]) >= stop
                        && 1000.0 * (first - stop) / qpcFrequency() <= 2000;
                }
            }
            if (!measured)
            { status = "FAIL"; jsonSet(result, "status", status); jsonSet(result, "reason", "Dual demo lacks both per-camera Present receipts/complete iterations or exceeds 250ms/2s"); }
        }
        jsonSet(result, "twoCamerasRequired", twoCameras);
        const auto saved = gocue::SafeFileWrite::writeTextVerified(reportFile, juce::JSON::toString(result, false));
        if (saved.failed()) throw std::runtime_error(saved.getErrorMessage().toStdString());
        std::cout << "Recorder demo " << status << ": " << reportFile.getFullPathName() << '\n';
        return status == "PASS" && process.getExitCode() == 0 ? 0 : status == "UNAVAILABLE" ? 2 : 1;
    }
    catch (const std::exception& e)
    {
        jsonSet(report, "status", "FAIL"); jsonSet(report, "stage", "demo-launch"); jsonSet(report, "reason", e.what());
        if (reportFile != juce::File()) gocue::SafeFileWrite::writeTextVerified(reportFile, juce::JSON::toString(report, false));
        std::cerr << e.what() << '\n'; return 1;
    }
}
