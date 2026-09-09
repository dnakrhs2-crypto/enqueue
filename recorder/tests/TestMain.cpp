#include <exception>
#include <iostream>
#include <string>
int runCaptureContractTests();
int main(int argc, char** argv)
{
    if (argc != 1 && !(argc == 3 && std::string(argv[1]) == "--suite" && std::string(argv[2]) == "capture-contract"))
    { std::cerr << "RecorderTests [--suite capture-contract]\n"; return 2; }
    try { return runCaptureContractTests(); }
    catch (const std::exception& e) { std::cerr << "Unhandled test exception: " << e.what() << '\n'; return 1; }
}
