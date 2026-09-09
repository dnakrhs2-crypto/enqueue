#include "TestSupport.h"
void runDubbingFailureScenario(int);
int runDubbingFailureTests()
{
    recorder_test::Suite suite;
    suite.test("Playback underrun aborts and preserves partial completion", [] { runDubbingFailureScenario(0); });
    suite.test("ASIO reset aborts at accepted end and persists partial take", [] { runDubbingFailureScenario(1); });
    suite.test("Edit, seek, output and arm locks cover both document APIs", [] { runDubbingFailureScenario(2); });
    suite.test("Invalid preparation does not leave locks or a take", [] { runDubbingFailureScenario(3); });
    suite.test("Abort during asynchronous preparation drains the later-created session", [] { runDubbingFailureScenario(4); });
    suite.test("One camera failure preserves the other stream and leaves an explicit gap", [] { runDubbingFailureScenario(5); });
    return suite.result("dubbing-failures");
}
