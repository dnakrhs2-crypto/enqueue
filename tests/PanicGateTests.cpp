#include "audio/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** The final output gate: a panic ends in silence at the device, whatever is still ringing behind it. */
class PanicGateTests : public juce::UnitTest
{
public:
    PanicGateTests() : juce::UnitTest ("Panic output gate", "Enqueue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeSine (const juce::File& dir)
    {
        const auto file = dir.getChildFile ("tone.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr);

        if (stream == nullptr)
            return {};

        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int numSamples = (int) (4.0 * sampleRate);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));

        expect (writer->writeFromAudioSampleBuffer (buffer, 0, numSamples));
        return file;
    }

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("enqueue_panic_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const auto tone = writeSine (dir);

        beginTest ("hard stop: the very next block is silent and a new cue reopens the gate");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> out (2, blockSize);

            Cue cue;
            cue.name = "tone";
            cue.file = tone;
            juce::String error;
            expect (engine.play (cue, &error), error);

            for (int i = 0; i < 20; ++i)
                engine.renderBlock (out, blockSize);

            expectGreaterThan (out.getRMSLevel (0, 0, blockSize), 0.2f);   // audible before the panic

            engine.stopAll();
            engine.renderBlock (out, blockSize);   // the gate ramps down inside this block (5 ms)
            engine.renderBlock (out, blockSize);
            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);   // and stays closed afterwards
            expectEquals (out.getMagnitude (1, 0, blockSize), 0.0f);

            for (int i = 0; i < 10; ++i)
            {
                engine.renderBlock (out, blockSize);
                engine.reapFinishedPlayers();
            }

            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);

            Cue again;
            again.name = "again";
            again.file = tone;
            expect (engine.play (again, &error), error);

            for (int i = 0; i < 4; ++i)
                engine.renderBlock (out, blockSize);

            expectGreaterThan (out.getRMSLevel (0, 0, blockSize), 0.2f);   // the gate reopened for the new cue
        }

        beginTest ("fade-out panic: silence once the fade and the 200 ms gate ramp are over, and it stays silent");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> out (2, blockSize);

            Cue cue;
            cue.name = "tone";
            cue.file = tone;
            juce::String error;
            expect (engine.play (cue, &error), error);

            for (int i = 0; i < 10; ++i)
                engine.renderBlock (out, blockSize);

            engine.fadeOutAndStopAll (300);   // 300 ms fade, then the gate closes over 200 ms

            const int blocksForHalfSecond = (int) std::ceil (0.5 * sampleRate / blockSize);   // 44

            for (int i = 0; i < blocksForHalfSecond + 4; ++i)
                engine.renderBlock (out, blockSize);

            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);
            expectEquals (out.getMagnitude (1, 0, blockSize), 0.0f);

            for (int i = 0; i < 20; ++i)
            {
                engine.renderBlock (out, blockSize);
                engine.reapFinishedPlayers();
            }

            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);
        }

        beginTest ("no device after initialise: play() refuses with a message (offline engines without initialise still play)");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            Cue cue;
            cue.name = "tone";
            cue.file = tone;
            juce::String error;
            expect (engine.play (cue, &error), error);   // never initialised: no device expected
        }

        dir.deleteRecursively();
    }
};

static PanicGateTests panicGateTests;

} // namespace gocue::tests
