#include "HardeningChecks.h"
#include <iostream>
extern std::uint64_t recorderEditPropertySeed;
extern int recorderEditPropertyIterations;
int runEditPropertyTests()
{
    try
    {
        const auto report = gocue::recorder::hardening::editProperty(recorderEditPropertySeed, recorderEditPropertyIterations);
        std::cout << "EditPropertyTests: " << juce::JSON::toString(report, true) << '\n'; return 0;
    }
    catch (const std::exception&) { return 1; }
}
