#include <exception>
#include <iostream>
#include <string>
int runCaptureContractTests();
int runCfrSchedulerTests();
int runEncoderContractTests();
int main(int argc, char** argv)
{
    if (argc != 1 && !(argc == 3 && std::string(argv[1]) == "--suite"))
    { std::cerr << "RecorderTests [--suite capture-contract|cfr-scheduler|encoder-contract]\n"; return 2; }
    try
    {
        const auto suite = argc == 1 ? std::string("all") : std::string(argv[2]);
        if (suite == "capture-contract") return runCaptureContractTests();
        if (suite == "cfr-scheduler") return runCfrSchedulerTests();
        if (suite == "encoder-contract") return runEncoderContractTests();
        if (suite != "all") return 2;
        const auto capture = runCaptureContractTests(), cfr = runCfrSchedulerTests(), encode = runEncoderContractTests();
        return capture || cfr || encode ? 1 : 0;
    }
    catch (const std::exception& e) { std::cerr << "Unhandled test exception: " << e.what() << '\n'; return 1; }
}
