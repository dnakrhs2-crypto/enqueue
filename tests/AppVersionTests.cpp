#include "app/AppVersion.h"

namespace gocue::tests
{

class AppVersionTests : public juce::UnitTest
{
public:
    AppVersionTests() : juce::UnitTest ("Application version", "Enqueue") {}

    void runTest() override
    {
        beginTest ("the generated target version is nonempty and matches PROJECT_VERSION");
        const juce::File expectedFile (GOCUE_EXPECTED_APP_VERSION_FILE);
        expect (expectedFile.existsAsFile(), "Missing configured PROJECT_VERSION fixture");
        const auto version = appVersionString();
        expect (version.isNotEmpty());
        expectEquals (version, expectedFile.loadFileAsString().trim());

        beginTest ("the executable version resource matches the generated target version");
        const auto executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        expectEquals (executable.getVersion(), version + ".0");
    }
};

static AppVersionTests appVersionTests;

} // namespace gocue::tests
