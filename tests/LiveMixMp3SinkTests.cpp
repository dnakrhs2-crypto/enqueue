#include "livemix017/r5/RecorderChecks.h"
namespace gocue::tests
{
class LiveMixMp3SinkTests final : public juce::UnitTest
{
public:
    LiveMixMp3SinkTests() : UnitTest("LiveMix Recorder MP3 encoding and resampling", "LiveMix") {}
    void runTest() override
    {
        livemix::recording::checks::mp3Tests([this](const auto& result)
        {
            beginTest(juce::String::fromUTF8(result.name.c_str()));
            for (const auto& assertion : result.assertions) expect(assertion.first, juce::String::fromUTF8(assertion.second.c_str()));
        });
    }
};
static LiveMixMp3SinkTests liveMixMp3SinkTests;
}
