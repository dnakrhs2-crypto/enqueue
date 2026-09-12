#include "../tests/HardeningChecks.h"
#include "ProbeOutput.h"
#include "diagnostics/CaptureTelemetry.h"
#include <charconv>
#include <iostream>
#include <map>
#include <set>

namespace gocue::recorder
{
int runHardeningProbe(int argc, wchar_t** argv)
{
    auto report = jsonObject(); juce::String reportName; int code = 1;
    const auto path = [](const juce::String& p) { return juce::File::getCurrentWorkingDirectory().getChildFile(p); };
    const auto write = [&] { if (reportName.isNotEmpty()) CaptureTelemetry::writeJson(path(reportName),report); };
    try
    {
        std::map<juce::String,juce::String> args;
        const std::set<juce::String> values{"--clip-count","--project-dir","--report","--seed","--iterations"};
        for (int i = 2; i < argc; ++i)
        {
            juce::String key(argv[i]); exportRequire(!args.count(key),"Duplicate hardening option");
            if (key == "--large-files") args[key] = "true";
            else { exportRequire(values.count(key) && i + 1 < argc,"Unknown/incomplete hardening option"); args[key] = juce::String(argv[++i]); }
            if (key == "--report") reportName = args[key];
        }
        exportRequire(args["--project-dir"].isNotEmpty() && reportName.isNotEmpty(),"Required: --project-dir DIR --report FILE");
        const auto number = [&](const char* key, std::uint64_t fallback, std::uint64_t minimum, std::uint64_t maximum)
        {
            if (!args.count(key)) return fallback; const auto t = args[key].toStdString(); std::uint64_t n = 0;
            const auto parsed = std::from_chars(t.data(),t.data()+t.size(),n);
            exportRequire(parsed.ec == std::errc{} && parsed.ptr == t.data()+t.size() && n >= minimum && n <= maximum,"Invalid hardening integer"); return n;
        };
        const auto count = number("--clip-count",10000,2,100000), seed = number("--seed",909,0,UINT64_MAX), iterations = number("--iterations",1000,2,1000000);
        const auto root = probe::prepareOutputRoot(path(args["--project-dir"]));
        jsonSet(report,"schemaVersion",1); jsonSet(report,"status","RUNNING"); jsonSet(report,"projectDirectory",root.getFullPathName());
        jsonSet(report,"sourceKind","synthetic CPU/file/metadata regression; no physical ASIO/MF/GPU opened"); write();
        jsonSet(report,"scale",hardening::scale(root.getChildFile("scale"),std::size_t(count))); write();
        if (args.count("--large-files")) jsonSet(report,"largeFiles",hardening::largeFiles(root.getChildFile("large-files")));
        else { auto skipped = jsonObject(); jsonSet(skipped,"status","NOT_REQUESTED"); jsonSet(report,"largeFiles",skipped); }
        write(); jsonSet(report,"editProperty",hardening::editProperty(seed,int(iterations))); write();
        jsonSet(report,"cancelledSeeks",hardening::cancelledSeeks(unsigned(iterations))); write();
        jsonSet(report,"exportCancellation",hardening::cancelledExports(root.getChildFile("export-cancel"),20)); write();
        jsonSet(report,"dubbingEpoch",hardening::dubbingEpoch(root.getChildFile("dubbing-epoch"))); write();
        jsonSet(report,"crashGuarantee","Process-crash durability only; power loss/storage firmware/clean PC/studio remain unverified");
        jsonSet(report,"diagnostics","Numeric counters, paths and synthetic test metadata only; no original media bytes or environment/credential dump");
        jsonSet(report,"status","PASS"); code = 0;
    }
    catch (const std::exception& e) { jsonSet(report,"status","FAIL"); jsonSet(report,"reason",e.what()); }
    try { write(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; code = 1; }
    std::cout << juce::JSON::toString(report,true) << '\n'; return code;
}
}
