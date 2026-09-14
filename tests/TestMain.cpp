#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

namespace
{

class ConsoleRunner : public juce::UnitTestRunner
{
    void logMessage (const juce::String& message) override
    {
        std::cout << message << std::endl;
    }
};

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    ConsoleRunner runner;
    runner.setAssertOnFailure (false);
    runner.setPassesAreLogged (false);
    juce::Array<juce::UnitTest*> tests;   // both apps in one run: one result list, one summary
    const juce::String only = argc > 1 ? juce::String (argv[1]) : juce::String();   // optional: run only the tests whose name contains this

    for (auto* test : juce::UnitTest::getAllTests())
        if ((test->getCategory() == "Enqueue" || test->getCategory() == "LiveMix") && (only.isEmpty() || test->getName().containsIgnoreCase (only)))
            tests.add (test);

    if (only.isNotEmpty() && tests.isEmpty())
    {
        std::cout << "no test name contains '" << only << "'" << std::endl;
        return 2;
    }

    runner.runTests (tests);

    int passes = 0;
    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult (i);
        passes += result->passes;
        failures += result->failures;
    }

    std::cout << "\n==== Enqueue tests: " << passes << " passed, " << failures << " failed ====" << std::endl;
    return failures == 0 ? 0 : 1;
}
