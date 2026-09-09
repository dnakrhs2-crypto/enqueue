// Single RecorderTests entry point. Every round registers its suite here (one entry function per file,
// never another main(), never a CMake rename of main) so parallel rounds merge without linker clashes.
//   RecorderTests                     -> every suite
//   RecorderTests --suite <name>      -> one suite (see the table below)
//   RecorderTests --list              -> suite names
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

int runCaptureContractTests();   // round 01: capture / decode / preview contracts (no hardware)
int runCfrSchedulerTests();      // round 02: CFR frame selection / counters
int runEncoderContractTests();   // round 02: NVENC / MP4 / AAC contracts (no GPU)
int runAsioStampTests();         // round 03: ASIO BlockStamp bridge statistics
int runNativePcmTests();         // round 03: native PCM -> PCM24 packing
int runJournalTests();           // round 06: recording journal / durable file
int runWavChunkTests();          // round 06: WAV chunk writer

namespace
{
struct Suite
{
    const char* name;
    int (*run)();
};
int runAsioNativePcm() { const int a = runAsioStampTests(), b = runNativePcmTests(); return (a || b) ? 1 : 0; }
int runJournalDurable() { const int a = runJournalTests(), b = runWavChunkTests(); return (a || b) ? 1 : 0; }
const Suite suites[] = {
    {"capture-contract", runCaptureContractTests},
    {"cfr-scheduler", runCfrSchedulerTests},
    {"encoder-contract", runEncoderContractTests},
    {"asio-native-pcm", runAsioNativePcm},
    {"journal-durable", runJournalDurable},
    {"wav-chunks", runWavChunkTests},
};
int usage()
{
    std::cerr << "RecorderTests [--suite <name>|--list]\n  suites:";
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
        if (argc == 1 || (argc == 3 && std::strcmp(argv[1], "--suite") == 0 && std::strcmp(argv[2], "all") == 0))
        {
            int failed = 0;
            for (const auto& s : suites) failed |= s.run();
            std::cout << "RecorderTests: all suites " << (failed ? "FAILED" : "passed") << '\n';
            return failed ? 1 : 0;
        }
        if (argc == 3 && std::strcmp(argv[1], "--suite") == 0)
        {
            for (const auto& s : suites)
                if (std::strcmp(argv[2], s.name) == 0) return s.run();
            std::cerr << "Unknown suite: " << argv[2] << '\n';
            return usage();
        }
        return usage();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Unhandled test exception: " << e.what() << '\n';
        return 1;
    }
}
