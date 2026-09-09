#include "HardeningChecks.h"
#include "TestSupport.h"
#include "../tools/ProbeOutput.h"

using namespace gocue::recorder;
int runHardeningTests()
{
    recorder_test::Suite tests;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderHardening-"+newId());
    tests.test("10000 long clips: canonical checkpoint, durable reorder, undo and redo",[&] { hardening::scale(root.getChildFile("scale"),10000); });
    tests.test("Virtual 4GiB boundaries, RF64 header and three-hour packet index",[&] { hardening::largeFiles(root.getChildFile("large")); });
    tests.test("Repeated cancelled dual seeks join workers and release owned resources",[&] { hardening::cancelledSeeks(1000); });
    tests.test("Cancelled real WAV exports reclaim handles, workers and partials",[&] { hardening::cancelledExports(root.getChildFile("export-cancel"),20); });
    tests.test("Start epoch +1 still submits and decodes every dubbing frame",[&] { hardening::dubbingEpoch(root.getChildFile("epoch")); });
    tests.test("Probe output reruns preserve existing files and select a fresh directory",[&]
    {
        const auto first = probe::prepareOutputRoot(root.getChildFile("exports"));
        const auto emptyRetry = probe::prepareOutputRoot(first); recorder_test::require(emptyRetry != first && emptyRetry.isDirectory(),"Empty output collision");
        const auto keep = first.getChildFile("failed-run.partial"); recorder_test::require(keep.replaceWithText("preserve existing artifact"),"Fixture write");
        const auto retry = probe::prepareOutputRoot(first);
        recorder_test::require(retry != first && retry != emptyRetry && keep.loadFileAsString() == "preserve existing artifact","Probe destroyed prior artifact");
        const auto map = probe::physicalInputs("3,1"); recorder_test::require(map[0] == 2 && map[1] == 0 && map[2] == -1,"Sparse input mapping");
        recorder_test::require(probe::physicalInputs("none")[0] == -1,"No-input mapping");
        for (const char* bad : {"",",1","1,","1,,2","1,1","0","257","x"}) recorder_test::rejects([&] { probe::physicalInputs(bad); });
    });
    return tests.result("HardeningTests");
}
