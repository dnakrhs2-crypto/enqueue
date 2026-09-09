#include "support/Platform.h"
#include "model/SafeFileWrite.h"
#include <map>
#include <charconv>
#include <iostream>

int runDemoProbe(int argc, wchar_t** argv)
{
    using namespace gocue::recorder; juce::File reportFile; auto report = jsonObject();
    try
    {
        std::map<juce::String, juce::String> args;
        for (int i = 2; i < argc; i += 2)
        {
            const juce::String flag(argv[i]);
            if (i + 1 >= argc || (flag != "--app" && flag != "--devices" && flag != "--asio-device" && flag != "--iterations" && flag != "--report") || args.count(flag)) throw std::invalid_argument("Invalid demo arguments");
            args[flag] = juce::String(argv[i + 1]); if (flag == "--report") reportFile = juce::File::getCurrentWorkingDirectory().getChildFile(args[flag]);
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
        juce::ChildProcess process; if (!process.start(command, 0)) throw std::runtime_error("Could not launch Recorder demo");
        if (!process.waitForProcessToFinish((count * 60 + 90) * 1000)) { process.kill(); throw std::runtime_error("Recorder demo timed out"); }
        if (!runReport.existsAsFile()) throw std::runtime_error("Recorder did not write this run's report");
        const auto result = juce::JSON::parse(runReport); const auto status = result["status"].toString();
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
