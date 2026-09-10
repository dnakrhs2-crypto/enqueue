// Single RecorderTests entry point. Every round registers its suite here (one entry function per file,
// never another main(), never a CMake rename of main) so parallel rounds merge without linker clashes.
//   RecorderTests                     -> every suite
//   RecorderTests --suite <name>      -> one suite (see the table below)
//   RecorderTests --list              -> suite names
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <charconv>
#include <cstdint>

int runCaptureContractTests();   // round 01: capture / decode / preview contracts (no hardware)
int runCaptureReviewTests();     // round 02b: recovery, pacing and MF JPEG colour fixtures
int runCfrSchedulerTests();      // round 02: CFR frame selection / counters
int runEncoderContractTests();   // round 02: NVENC / MP4 / AAC contracts (no GPU)
int runAsioStampTests();         // round 03: ASIO BlockStamp bridge statistics
int runNativePcmTests();         // round 03: native PCM -> PCM24 packing
int runClockMapperTests();       // round 04: clocks / epochs / calibration / CFR timing
int runJournalTests();           // round 06: recording journal / durable file
int runWavChunkTests();          // round 06: WAV chunk writer
int runProjectTests();           // round 08: project model / serializer / document / undo
int runRecorderAudioTests();
int runTakeControllerTests();
int runRecoveryTests();          // round 07: recovery and commit replay
int runLargeFileTests();         // round 07: virtual offsets and I/O failures
int runClipEditTests();
int runLinkEditTests();
int runEditHistoryTests();
int runEditPropertyTests();
std::uint64_t recorderEditPropertySeed = 909;
int recorderEditPropertyIterations = 1000;
int runAudioImportTests();       // round 16: copied originals, decoded lengths and derived PCM
int runImportedClipTests();      // round 16: independent import track/document transaction
int runPlaybackTests();          // round 11: indexed playback / audible transport (no devices)
int runDubbingPlacementTests();
int runDubbingFailureTests();
int runTakeStackTests();
int runUiWiringTests();          // round 12: UI state, settings, derived caches and shared output
int runQueueIsolationTests();    // round 05: two-camera queues, raw-audio stop and pixel oracle
int runAudioCutRenderTests();    // round 14: independent audio cuts / source masks / microfades
int runPlaybackQueueTests();     // round 14: prefetch / revision banks / RT allocation hook
int runRippleTests();
int runReorderTests();
int runMarkerTests();
int runEditJournalTests();       // round 19: transactions, worker, checkpoint generations
int runRetakeRecoveryTests();    // round 19: edit/undo/placement/finalization recovery
int runCameraSlotTests();        // round 24: product camera slots and partial failures
int runExportRangeTests();       // rounds 20+21: immutable export range and common PCM
int runWavExportTests();
int runFinalExportTests();
int runMixedFpsTests();
int runDualTakeIntegrationTests();
int runDualPlaybackTests();      // round 26: shared cursor, gaps and timeline scale
int runSeekGenerationTests();    // round 26: cancellation, file handoff and cache bounds
int runLifecycleTests();         // round 28: faults, shutdown, updater/exclusivity
int runHardeningTests();
int runMaterialExportTests();
int runExportLifecycleTests();
int runSourceReanchorTests();
int runCutEditStabilityTests();

namespace
{
struct Suite
{
    const char* name;
    int (*run)();
};
int runAsioNativePcm() { const int a = runAsioStampTests(), b = runNativePcmTests(); return (a || b) ? 1 : 0; }
int runJournalDurable() { const int a = runJournalTests(), b = runWavChunkTests(); return (a || b) ? 1 : 0; }
int runCutLinkHistory() { const int a = runClipEditTests(), b = runLinkEditTests(), c = runEditHistoryTests(); return (a || b || c) ? 1 : 0; }
int runAudioImport() { const int a = runAudioImportTests(), b = runImportedClipTests(); return (a || b) ? 1 : 0; }
int runRippleReorderMarkers() { const int a = runRippleTests(), b = runReorderTests(), c = runMarkerTests(); return (a || b || c) ? 1 : 0; }
int runRecoveryIdempotence() { const int a = runRecoveryTests(), b = runRetakeRecoveryTests(); return (a || b) ? 1 : 0; }
const Suite suites[] = {
    {"cut-edit-stability", runCutEditStabilityTests},
    {"source-reanchor", runSourceReanchorTests},
    {"hardening", runHardeningTests},
    {"materials-alignment", runMaterialExportTests},
    {"export-lifecycle", runExportLifecycleTests},
    {"export-audio-range", runExportRangeTests},
    {"wav-export", runWavExportTests},
    {"final-export-sources", runFinalExportTests},
    {"audio-cut-render", runAudioCutRenderTests},
    {"audio-prefetch-underrun", runPlaybackQueueTests},
    {"capture-contract", runCaptureContractTests},
    {"capture-review", runCaptureReviewTests},
    {"cfr-scheduler", runCfrSchedulerTests},
    {"encoder-contract", runEncoderContractTests},
    {"asio-native-pcm", runAsioNativePcm},
    {"clock-mapping", runClockMapperTests},
    {"journal-durable", runJournalDurable},
    {"wav-chunks", runWavChunkTests},
    {"project-roundtrip", runProjectTests},
    {"recorder-audio", runRecorderAudioTests},
    {"take-lifecycle", runTakeControllerTests},
    {"recovery-idempotence", runRecoveryIdempotence},
    {"edit-journal-replay", runEditJournalTests},
    {"large-files", runLargeFileTests},
    {"cut-link-history", runCutLinkHistory},
    {"edit-property", runEditPropertyTests},
    {"audio-import", runAudioImport},
    {"playback-engine", runPlaybackTests},
    {"dubbing-clock-placement", runDubbingPlacementTests},
    {"dubbing-failures", runDubbingFailureTests},
    {"take-stack-edits", runTakeStackTests},
    {"ui-wiring", runUiWiringTests},
    {"queue-isolation", runQueueIsolationTests},
    {"ripple-reorder-markers", runRippleReorderMarkers},
    {"camera-slots", runCameraSlotTests},
    {"mixed-fps", runMixedFpsTests},
    {"partial-camera-take", runDualTakeIntegrationTests},
    {"dual-playback", runDualPlaybackTests},
    {"seek-generation", runSeekGenerationTests},
    {"lifecycle", runLifecycleTests},
};
int usage()
{
    std::cerr << "RecorderTests [--suite <name>|--list] [--seed <uint64> --iterations <positive int>]\n  suites:";
    for (const auto& s : suites) std::cerr << ' ' << s.name;
    std::cerr << '\n';
    return 2;
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::strcmp(argv[1], "--list") == 0)
        {
            for (const auto& s : suites) std::cout << s.name << '\n';
            return 0;
        }
        std::string selected = "all";
        bool hasSuite = false, hasSeed = false, hasIterations = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string flag = argv[i];
            if (i + 1 == argc) return usage();
            const char* value = argv[++i]; const char* end = value + std::strlen(value);
            if (flag == "--suite" && !hasSuite) { selected = value; hasSuite = true; }
            else if (flag == "--seed" && !hasSeed)
            { const auto r = std::from_chars(value, end, recorderEditPropertySeed); if (r.ec != std::errc{} || r.ptr != end) return usage(); hasSeed = true; }
            else if (flag == "--iterations" && !hasIterations)
            { const auto r = std::from_chars(value, end, recorderEditPropertyIterations); if (r.ec != std::errc{} || r.ptr != end || recorderEditPropertyIterations < 1 || recorderEditPropertyIterations > 1000000) return usage(); hasIterations = true; }
            else return usage();
        }
        if ((hasSeed || hasIterations) && selected != "all" && selected != "edit-property") return usage();
        if (selected == "all")
        {
            int failed = 0;
            for (const auto& s : suites) failed |= s.run();
            std::cout << "RecorderTests: all suites " << (failed ? "FAILED" : "passed") << '\n';
            return failed ? 1 : 0;
        }
        for (const auto& s : suites) if (selected == s.name) return s.run();
        std::cerr << "Unknown suite: " << selected << '\n';
        return usage();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Unhandled test exception: " << e.what() << '\n';
        return 1;
    }
}
