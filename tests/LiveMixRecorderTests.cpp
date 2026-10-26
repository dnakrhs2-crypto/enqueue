#include "livemix017/r5/RecorderChecks.h"
namespace gocue::tests
{
class LiveMixRecorderTests final : public juce::UnitTest
{
public:
    LiveMixRecorderTests() : UnitTest("LiveMix Recorder lifecycle and layouts", "LiveMix") {}
    void runTest() override
    {
        livemix::recording::checks::recorderTests([this](const auto& result)
        {
            beginTest(juce::String::fromUTF8(result.name.c_str()));
            for (const auto& assertion : result.assertions) expect(assertion.first, juce::String::fromUTF8(assertion.second.c_str()));
        });
    }
};
static LiveMixRecorderTests liveMixRecorderTests;
}
