#include "audio/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** The footer's output diagnostics: the peak of what goes to the device, the blocks over 0 dBFS, and nothing offline. */
class OutputDiagnosticsTests : public juce::UnitTest
{
public:
    OutputDiagnosticsTests() : juce::UnitTest ("OutputDiagnostics", "Enqueue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeSine (const juce::File& dir, const juce::String& fileName, double seconds, float amp)
    {
        const auto file = dir.getChildFile (fileName);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int n = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> b (2, n);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample (ch, i, amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));

        expect (writer->writeFromAudioSampleBuffer (b, 0, n));
        return file;
    }

    static void render (AudioEngine& engine, juce::AudioBuffer<float>& out, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();
        }
    }

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gocue_diag_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const auto tone = writeSine (dir, "tone.wav", 2.0, 0.9f);

        beginTest ("a cue pushed over 0 dBFS counts clipped blocks and reports the peak; silence reports nothing");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> out (2, blockSize);

            render (engine, out, 4);
            auto d = engine.takeOutputDiagnostics();
            expectWithinAbsoluteError (d.peak, 0.0f, 1.0e-6f);
            expectEquals (d.clippedBlocks, 0);
            expectEquals (d.xruns, 0);

            Cue loud;
            loud.name = "loud"; loud.file = tone; loud.gainDb = 6.0;   // 0.9 * 2 = 1.8: over 0 dBFS
            juce::String error;
            expect (engine.play (loud, &error), error);
            render (engine, out, 20);
            d = engine.takeOutputDiagnostics();
            expect (d.peak > 1.0f, "the peak must show the level that went out (" + juce::String (d.peak) + ")");
            expect (d.clippedBlocks > 0, "blocks over 0 dBFS must be counted");
            const int clippedSoFar = d.clippedBlocks;

            engine.stopAll();
            render (engine, out, 40);   // the stop fade runs out
            d = engine.takeOutputDiagnostics();
            expect (d.clippedBlocks >= clippedSoFar, "the clip count keeps counting until a device starts again");
            render (engine, out, 4);
            d = engine.takeOutputDiagnostics();
            expectWithinAbsoluteError (d.peak, 0.0f, 1.0e-6f);   // the peak is what went out since the last take: silence now
        }

        beginTest ("a cue within 0 dBFS reports its peak and no clipped block");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> out (2, blockSize);
            Cue fine;
            fine.name = "fine"; fine.file = tone;   // peak 0.9
            juce::String error;
            expect (engine.play (fine, &error), error);
            render (engine, out, 20);
            const auto d = engine.takeOutputDiagnostics();
            expect (d.peak > 0.85f && d.peak <= 1.0f, "peak " + juce::String (d.peak));
            expectEquals (d.clippedBlocks, 0);
            engine.stopAll();
            render (engine, out, 4);
        }

        dir.deleteRecursively();
    }
};

static OutputDiagnosticsTests outputDiagnosticsTests;

} // namespace gocue::tests
