#include "audio/PluginHost.h"
#include "audio/PluginScan.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <windows.h>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>

namespace gocue::tests
{
class LiveMixGDenoiseTests : public juce::UnitTest
{
public:
    LiveMixGDenoiseTests() : UnitTest ("LiveMix G-Denoise voice transition", "LiveMix") {}

    void runTest() override
    {
        const auto module = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_GDENOISE_VST3", {});
        if (module.isEmpty()) return;
        beginTest ("12 seconds at 48 kHz / 256; voice_mode on at 3 seconds and off at 8 seconds");
        PluginHost host;
        PluginScanCoordinator::Options options;
        options.executable = juce::File (LM_SCAN_WORKER_EXE);
        options.timeoutMs = 20000;
        host.getKnownPlugins().setCustomScanner (std::make_unique<PluginScanCoordinator> (options));
        juce::OwnedArray<juce::PluginDescription> types;
        host.getKnownPlugins().scanAndAddFile (module, false, types, *host.getVST3Format());
        host.getKnownPlugins().scanFinished();
        expect (! types.isEmpty(), "G-Denoise did not scan");
        if (types.isEmpty()) return;
        juce::String error;
        auto plugin = host.createInstance (*types[0], 48000.0, 256, error);
        expect (plugin != nullptr, error);
        if (plugin == nullptr) return;
        juce::AudioProcessorParameter* voice = nullptr;
        // JUCE's VST3 wrapper exports string IDs as numeric VST3 ParamIDs.
        const auto voiceId = juce::String (juce::VST3ClientExtensions::convertJuceParameterId ("voice_mode"));
        for (auto* parameter : plugin->getParameters())
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (parameter))
                if (hosted->getParameterID() == voiceId) voice = parameter;
        expect (voice != nullptr, "voice_mode parameter ID was not found");
        if (voice == nullptr) return;
        voice->setValueNotifyingHost (0.0f);

        constexpr int rate = 48000, blockSize = 256, sampleCount = rate * 12;
        PluginChain chain;
        chain.prepare (rate, blockSize);
        chain.addPlugin (std::move (plugin));
        juce::AudioBuffer<float> input (2, sampleCount), output (2, sampleCount);
        juce::Random random (0x47504c5547LL);
        for (int i = 0; i < sampleCount; ++i)
        {
            const bool toneOn = i % (rate / 2) < rate / 4; // a 0.25 s burst every 0.5 s
            const float sample = (random.nextFloat() * 2.0f - 1.0f) * 0.025f
                + (toneOn ? 0.25f * std::sin ((float) (juce::MathConstants<double>::twoPi * 220.0 * i / rate)) : 0.0f);
            for (int ch = 0; ch < 2; ++ch) input.setSample (ch, i, sample);
        }
        std::atomic<int> position { 0 };
        juce::WaitableEvent finished;
        std::thread audio ([&]
        {
            const auto start = std::chrono::steady_clock::now();
            juce::AudioBuffer<float> block (2, blockSize);
            for (int offset = 0; offset < sampleCount; offset += blockSize)
            {
                for (int ch = 0; ch < 2; ++ch) block.copyFrom (ch, 0, input, ch, offset, blockSize);
                chain.process (block, blockSize);
                for (int ch = 0; ch < 2; ++ch) output.copyFrom (ch, offset, block, ch, 0, blockSize);
                position = offset + blockSize;
                std::this_thread::sleep_until (start + std::chrono::microseconds ((juce::int64) (offset + blockSize) * 1000000 / rate));
            }
            finished.signal();
        });

        int phase = 0;
        std::array<int, 3> latencies { chain.getLatencySamples(), 0, 0 };
        int previousLatency = latencies[0];
        logMessage ("initial chain latency: " + juce::String (previousLatency) + " samples");
        do
        {
            MSG message;
            while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&message);
                DispatchMessageW (&message);
            }
            const auto seconds = (double) position.load() / rate;
            if ((phase == 0 && seconds >= 3.0) || (phase == 1 && seconds >= 8.0))
            {
                latencies[(size_t) phase] = chain.getLatencySamples();
                voice->setValueNotifyingHost (phase == 0 ? 1.0f : 0.0f);
                logMessage ("voice_mode=" + juce::String (phase == 0 ? 1 : 0) + " at " + juce::String (seconds, 4) + " s");
                ++phase;
            }
            // MainComponent runs at 30 Hz; this is MixDocument::pollPluginEdits' chain maintenance.
            chain.recoverAfterStalls();
            if (chain.consumeStateChanged()) chain.refreshPluginCaches();
            const int latency = chain.getLatencySamples();
            if (latency != previousLatency)
            {
                logMessage ("chain latency: " + juce::String (latency) + " samples at " + juce::String (seconds, 4) + " s");
                previousLatency = latency;
            }
        } while (! finished.wait (33));
        audio.join();
        latencies[2] = chain.getLatencySamples();
        logMessage ("latency before/on/off: " + juce::String (latencies[0]) + "/"
                    + juce::String (latencies[1]) + "/" + juce::String (latencies[2]) + " samples");

        struct Jump { float value = 0.0f; int sample = 0; };
        std::array<Jump, 5> jumps; // before/after on, before/after off, everywhere else
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < sampleCount; ++i)
            {
                finite = std::isfinite (output.getSample (ch, i)) && finite;
                if (i == 0) continue;
                const int range = i >= rate * 2 && i < rate * 3 ? 0
                    : i >= rate * 3 && i < rate * 4 ? 1
                    : i >= rate * 7 && i < rate * 8 ? 2
                    : i >= rate * 8 && i < rate * 9 ? 3 : 4;
                const auto delta = std::abs (output.getSample (ch, i) - output.getSample (ch, i - 1));
                if (delta > jumps[(size_t) range].value) jumps[(size_t) range] = { delta, i };
            }
        expect (finite, "output contains NaN or infinity");
        const std::array<const char*, 5> labels { "on before [2,3)", "on after [3,4)", "off before [7,8)", "off after [8,9)", "outside transitions" };
        for (size_t i = 0; i < jumps.size(); ++i)
            logMessage (juce::String (labels[i]) + ": max jump " + juce::String (jumps[i].value, 8)
                        + " at " + juce::String ((double) jumps[i].sample / rate, 6) + " s");
        logMessage ("chain faulted: " + juce::String ((int) chain.getSlot (0).faulted.load()));

        const auto outputPath = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_GDENOISE_OUT", {});
        if (outputPath.isNotEmpty())
        {
            const juce::File directory (outputPath);
            directory.createDirectory();
            writeWav (directory.getChildFile ("input.wav"), input);
            writeWav (directory.getChildFile ("output.wav"), output);
        }
    }

private:
    void writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer)
    {
        file.deleteFile();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (48000.0)
            .withNumChannels (2).withBitsPerSample (32));
        expect (writer != nullptr, "cannot write " + file.getFullPathName());
        if (writer != nullptr) expect (writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()));
    }
};

static LiveMixGDenoiseTests liveMixGDenoiseTests;
} // namespace gocue::tests
