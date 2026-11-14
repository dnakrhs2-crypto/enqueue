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
    const bool midiLoad = argc > 1 && juce::String (argv[1]) == "--midi-load";
    const bool skipFile = argc > 1 && juce::String (argv[1]) == "--skip-file";
    const juce::String only = argc > 1 && ! midiLoad && ! skipFile ? juce::String (argv[1]) : juce::String();   // optional name filter
    juce::StringArray skipNames, matchedNames;
    if (skipFile)
    {
        if (argc != 3)
        {
            std::cout << "usage: EnqueueTests --skip-file <path>" << std::endl;
            return 2;
        }
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[2]));
        const auto input = file.createInputStream();
        if (input == nullptr)
        {
            std::cout << "cannot read skip file: " << file.getFullPathName() << std::endl;
            return 2;
        }
        const auto lines = juce::StringArray::fromLines (input->readEntireStreamAsString());
        if (input->getStatus().failed())
        {
            std::cout << "cannot read skip file: " << file.getFullPathName() << std::endl;
            return 2;
        }
        for (const auto& line : lines)
        {
            const auto name = line.trim();
            if (name.isNotEmpty() && ! name.startsWithChar ('#'))
                skipNames.addIfNotAlreadyThere (name);
        }
    }

    int skipped = 0;
    for (auto* test : juce::UnitTest::getAllTests())
        if ((midiLoad ? test->getCategory() == "EnqueueMidiLoad" : (test->getCategory() == "Enqueue" || test->getCategory() == "LiveMix"))
            && (only.isEmpty() || test->getName().containsIgnoreCase (only)))
        {
            if (skipNames.contains (test->getName()))
            {
                ++skipped;
                matchedNames.addIfNotAlreadyThere (test->getName());
            }
            else
                tests.add (test);
        }

    if (skipFile)
        std::cout << "SKIP_FILE: skipped " << skipped << " suites, " << skipNames.size() - matchedNames.size()
                  << " unmatched names; running " << tests.size() << " suites" << std::endl;

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
