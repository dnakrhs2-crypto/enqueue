#include "LifecycleFixtures.h"
#include <charconv>
#include <iostream>

namespace gocue::recorder
{
int runLifecycleProbe(int argc, wchar_t** argv)
{
    using namespace lifecycleFixture;
    juce::File report, directory;
    unsigned iterations = 20, maxSeconds = 60, offset = 0;
    juce::String selected = "all";
    auto result = jsonObject(); juce::Array<juce::var> rows, command;
    for (int i = 0; i < argc; ++i) command.add(juce::String(argv[i]));
    unsigned completed = 0, failed = 0;
    try
    {
        std::set<juce::String> seen;
        for (int i = 2; i < argc; ++i)
        {
            const juce::String key(argv[i]); require(seen.insert(key).second && i + 1 < argc, "Duplicate/incomplete lifecycle option");
            const juce::String value(argv[++i]);
            if (key == "--iterations" || key == "--max-seconds" || key == "--case-offset")
            {
                unsigned number = 0; const auto text = value.toStdString();
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
                require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Expected unsigned decimal option");
                if (key == "--iterations") iterations = number; else if (key == "--max-seconds") maxSeconds = number; else offset = number;
            }
            else if (key == "--project-dir") directory = juce::File::getCurrentWorkingDirectory().getChildFile(value);
            else if (key == "--report") report = juce::File::getCurrentWorkingDirectory().getChildFile(value);
            else if (key == "--fault-cases") selected = value;
            else throw std::invalid_argument("Unknown lifecycle option: " + key.toStdString());
        }
        require(iterations > 0 && iterations <= 10000 && maxSeconds > 0 && maxSeconds <= 3600 && report != juce::File() && directory != juce::File(),
            "lifecycle --iterations 1..10000 --fault-cases all --project-dir DIR --report FILE [--max-seconds 60 --case-offset N]");
        const auto chosen = selected == "all" ? cases : juce::StringArray::fromTokens(selected, ",", "");
        require(!chosen.isEmpty(), "No fault cases selected"); for (const auto& name : chosen) require(cases.contains(name), "Unknown fault case: " + name);
        const auto root = directory.getChildFile("lifecycle-" + juce::Uuid().toString()); check(root.createDirectory());
        jsonSet(result, "projectDirectory", root.getFullPathName()); jsonSet(result, "startedUtc", utcNowIso8601());
        const auto began = IoHealth::now();
        const auto total = iterations * unsigned(chosen.size());
        require(offset < total, "case-offset must be below iterations * selected cases");
        for (unsigned index = offset; index < total; ++index)
        {
            if (completed && IoHealth::now() - began >= std::int64_t(maxSeconds) * 1000) break;
            const auto fault = chosen[int(index % unsigned(chosen.size()))];
            const auto folder = root.getChildFile(juce::String(index) + "-" + fault);
            auto row = jsonObject();
            try { row = cycle(folder, fault); }
            catch (const std::exception& e) { ++failed; jsonSet(row, "status", "FAIL"); jsonSet(row, "case", fault); jsonSet(row, "project", folder.getFullPathName()); jsonSet(row, "error", e.what()); }
            jsonSet(row, "iteration", int(index / unsigned(chosen.size()) + 1)); rows.add(row); ++completed;
            std::cout << "lifecycle " << index + 1 << '/' << total << ' ' << fault << ' ' << row["status"].toString() << std::endl;
            jsonSet(result, "cases", rows); jsonSet(result, "nextCaseOffset", int(offset + completed));
            CaptureTelemetry::writeJson(report, result); // preserve each fully completed case
        }
        jsonSet(result, "status", failed ? "FAIL" : offset + completed == total ? "PASS" : "INCOMPLETE");
        jsonSet(result, "elapsedMs", double(IoHealth::now() - began));
        jsonSet(result, "iterationsRequested", int(iterations)); jsonSet(result, "casesCompleted", int(completed)); jsonSet(result, "failed", int(failed));
        jsonSet(result, "sourceKind", "synthetic native PCM + CPU H264, production take/WAV/journal/recovery/renderer");
        jsonSet(result, "scope", "Implemented synthetic lifecycle contracts only; PASS does not certify physical devices or export integration");
        jsonSet(result, "exportStatus", "UNAVAILABLE"); jsonSet(result, "physicalDeviceStatus", "UNAVAILABLE");
        jsonSet(result, "unavailable", "Physical ASIO/camera unplug/sleep/D3D device removal; export module absent; no power-loss or 100-cycle certification");
        jsonSet(result, "timeBudget", "Checked between complete cases; in-flight durable finalization is never killed at the deadline");
        jsonSet(result, "command", command); jsonSet(result, "finishedUtc", utcNowIso8601());
        CaptureTelemetry::writeJson(report, result);
        return failed ? 1 : offset + completed == total ? 0 : 2;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n'; jsonSet(result, "status", "FAIL"); jsonSet(result, "error", e.what());
        if (report != juce::File()) CaptureTelemetry::writeJson(report, result);
        return 2;
    }
}
}
