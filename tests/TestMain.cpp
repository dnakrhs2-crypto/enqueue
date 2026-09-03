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

int main (int, char**)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    ConsoleRunner runner;
    runner.setAssertOnFailure (false);
    runner.setPassesAreLogged (false);
    runner.runTestsInCategory ("Enqueue");

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
