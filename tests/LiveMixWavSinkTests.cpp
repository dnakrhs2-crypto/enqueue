#include "livemix017/r5/RecorderChecks.h"
namespace gocue::tests
{
class LiveMixWavSinkTests final : public juce::UnitTest
{
public:
    LiveMixWavSinkTests() : UnitTest("LiveMix Recorder WAV files and checkpoints", "LiveMix") {}
    void runTest() override
    {
        livemix::recording::checks::wavTests([this](const auto& result)
        {
            beginTest(juce::String::fromUTF8(result.name.c_str()));
            for (const auto& assertion : result.assertions) expect(assertion.first, juce::String::fromUTF8(assertion.second.c_str()));
        });
    }
};
static LiveMixWavSinkTests liveMixWavSinkTests;
}
