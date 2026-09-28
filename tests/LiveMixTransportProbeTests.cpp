#include "MixEngine.h"

namespace gocue::tests
{
using namespace gocue::livemix;

class LiveMixTransportProbeTests : public juce::UnitTest
{
public:
    LiveMixTransportProbeTests() : UnitTest ("LiveMix real transport probe (opt-in)", "LiveMix") {}

    void runTest() override
    {
        beginTest ("VST3 and VST2 report playing positions and stop on the first OFF block");
        const auto directory = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_TRANSPORT_PROBE_DIR", "");
        if (directory.isEmpty())
        {
            logMessage ("skipped: LIVEMIX_TRANSPORT_PROBE_DIR is not set");
            expect (true);
            return;
        }

        expect (juce::MessageManager::getInstance()->isThisTheMessageThread());
        for (const bool vst2 : { false, true })
        {
            const juce::String formatName = vst2 ? "VST" : "VST3";
            const auto path = juce::File (directory).getChildFile (vst2 ? "VST/Transport Probe.dll" : "VST3/Transport Probe.vst3");
            const auto log = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile (vst2 ? "transport_probe_vst2.txt" : "transport_probe_vst3.txt");
            expect (log.deleteFile());
            MixEngine engine;
            constexpr int blockSize = 256;
            engine.prepare (48000.0, blockSize);
            auto& host = engine.getPluginHost();
            host.setVst2Enabled (vst2);
            auto* format = host.getFormat (formatName);
            expect (format != nullptr, formatName + " host is unavailable");
            if (format == nullptr) continue;
            juce::OwnedArray<juce::PluginDescription> descriptions;
            format->findAllTypesForFile (descriptions, path.getFullPathName());
            expectEquals (descriptions.size(), 1);
            if (descriptions.isEmpty()) continue;
            if (const auto xml = descriptions[0]->createXml())   // what a session slot needs to name this plugin (manual end-to-end checks)
                logMessage ("probe description: " + xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));
            juce::String error;
            auto plugin = host.createInstance (*descriptions[0], 48000.0, blockSize, error);
            expect (plugin != nullptr, error);
            if (plugin == nullptr) continue;
            MixSession session;
            session.addChannel();
            const auto id = session.channels[0].id;
            engine.applySession (session);
            engine.getChannelChain (id)->addPlugin (std::move (plugin));
            juce::AudioBuffer<float> output (2, blockSize);
            const auto render = [&] (int blocks)
            {
                for (int i = 0; i < blocks; ++i)
                    engine.renderBlock (nullptr, 0, output.getArrayOfWritePointers(), 2, blockSize);
            };
            const auto check = [&] (bool playing, juce::int64 loggedBlock)
            {
                const auto line = log.loadFileAsString().trim();
                logMessage (log.getFileName() + (playing ? " ON: " : " OFF: ") + line);
                juce::StringPairArray values;
                for (const auto& token : juce::StringArray::fromTokens (line, " ", ""))
                    values.set (token.upToFirstOccurrenceOf ("=", false, false), token.fromFirstOccurrenceOf ("=", false, false));
                auto number = [&] (const char* key) { return values[key].getLargeIntValue(); };
                expect (line.isNotEmpty());
                expectEquals (number ("blocks"), loggedBlock);
                expectEquals (number ("playhead"), (juce::int64) 1);
                expectEquals (number ("position"), (juce::int64) 1);
                expectEquals (number ("playing"), (juce::int64) (playing ? 1 : 0));
                expectEquals (number ("samplesValid"), (juce::int64) 1);
                expectEquals (number ("samples"), playing ? (number ("blocks") - 1) * blockSize : (juce::int64) 0);
                expectEquals (number ("bpmValid"), (juce::int64) (playing ? 1 : 0));
                expectEquals (number ("ppqValid"), (juce::int64) (playing ? 1 : 0));
                expectEquals (number ("sigValid"), (juce::int64) (playing ? 1 : 0));
                if (playing)
                {
                    expectEquals (values["bpm"].getDoubleValue(), 120.0);
                    expectEquals (values["sig"], juce::String ("4/4"));
                    expectWithinAbsoluteError (values["ppq"].getDoubleValue(), (double) number ("samples") / 24000.0, 0.0001);
                }
            };
            engine.setSendTransport (true);
            render (75);
            check (true, 51);
            engine.setSendTransport (false);
            render (1);
            check (false, 76);
            render (59);
            check (false, 126);
        }
    }
};
static LiveMixTransportProbeTests liveMixTransportProbeTests;
}
