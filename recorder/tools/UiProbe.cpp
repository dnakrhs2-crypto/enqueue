#include "ui/TimelineView.automation.h"
#include "model/SafeFileWrite.h"
#include <iostream>
#include <map>

int runUiProbe(int argc, wchar_t** argv)
{
    using namespace gocue::recorder; juce::File reportFile;
    auto* root = new juce::DynamicObject(); juce::var report(root);
    try
    {
        std::map<juce::String, juce::String> args; bool headless = false;
        for (int i = 2; i < argc; ++i)
        {
            const juce::String key(argv[i]);
            if (key == "--headless" && !headless) { headless = true; continue; }
            if ((key != "--app" && key != "--scenario" && key != "--report") || i + 1 == argc || args.count(key)) throw std::invalid_argument("Invalid ui arguments");
            args[key] = juce::String(argv[++i]); if (key == "--report") reportFile = juce::File::getCurrentWorkingDirectory().getChildFile(args[key]);
        }
        if (args["--scenario"] != "independent-audio-cuts" || args["--report"].isEmpty() || (headless ? args.count("--app") != 0 : args["--app"].isEmpty()))
            throw std::invalid_argument("ui needs (--headless | --app Recorder.exe) --scenario independent-audio-cuts --report file.json");
        const auto runId = juce::Uuid().toString();
        if (headless)
        {
            RecorderDocument document; TimelineEditController edits(document); IndependentAudioCutsScenario scenario(edits);
            while (!scenario.advance()) {}
            report = scenario.report(); report.getDynamicObject()->setProperty("mode", "headless"); report.getDynamicObject()->setProperty("runId", runId);
            report.getDynamicObject()->setProperty("visibleWindowPainted", false);
        }
        else
        {
            const auto app = juce::File::getCurrentWorkingDirectory().getChildFile(args["--app"]);
            if (!app.existsAsFile()) throw std::invalid_argument("Recorder app missing");
            const auto isolated = reportFile.getParentDirectory().getChildFile("ui-" + runId); const auto resultFile = isolated.getChildFile("result.json");
            const auto scenarioFile = isolated.getChildFile("scenario.json"); auto* c = new juce::DynamicObject(); juce::var config(c);
            c->setProperty("schemaVersion", 1); c->setProperty("scenario", args["--scenario"]); c->setProperty("runId", runId); c->setProperty("report", resultFile.getFullPathName());
            const auto saved = gocue::SafeFileWrite::writeTextVerified(scenarioFile, juce::JSON::toString(config, false));
            if (saved.failed()) throw std::runtime_error(saved.getErrorMessage().toStdString());
            juce::ChildProcess process;
            if (!process.start(juce::StringArray{app.getFullPathName(), "--automation", scenarioFile.getFullPathName()}, 0)) throw std::runtime_error("Cannot launch Recorder UI scenario");
            if (!process.waitForProcessToFinish(45000)) { process.kill(); throw std::runtime_error("Recorder UI scenario timed out"); }
            if (!resultFile.existsAsFile()) throw std::runtime_error("Recorder did not write this run's report");
            report = juce::JSON::parse(resultFile);
            if (!report.isObject() || report["runId"].toString() != runId || report["scenario"].toString() != args["--scenario"]
                || report["mode"].toString() != "real-window" || !bool(report["visibleWindowPainted"]) || process.getExitCode() != 0) throw std::runtime_error("Recorder UI report/exit validation failed");
            report.getDynamicObject()->setProperty("app", app.getFullPathName()); report.getDynamicObject()->setProperty("scenarioFile", scenarioFile.getFullPathName());
        }
        const auto written = gocue::SafeFileWrite::writeTextVerified(reportFile, juce::JSON::toString(report, false));
        if (written.failed()) throw std::runtime_error(written.getErrorMessage().toStdString());
        std::cout << "Recorder ui " << report["status"].toString() << ": " << reportFile.getFullPathName() << '\n'; return report["status"].toString() == "PASS" ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        if (!report.isObject()) report = juce::var(new juce::DynamicObject());
        report.getDynamicObject()->setProperty("status", "FAIL"); report.getDynamicObject()->setProperty("reason", juce::String::fromUTF8(e.what()));
        if (reportFile != juce::File()) gocue::SafeFileWrite::writeTextVerified(reportFile, juce::JSON::toString(report, false));
        std::cerr << e.what() << '\n'; return 1;
    }
}
