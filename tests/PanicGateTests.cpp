#include "audio/AudioEngine.h"
#include "TestGainPlugin.h"

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

        beginTest ("a self-generating master plugin: exact silence once the ramp is over, closed stays closed, the next cue reopens");
        {
            struct Generator : public TestGainPlugin
            {
                Generator() : TestGainPlugin (1.0f) {}

                void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
                {
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.5f, buffer.getNumSamples());
                }
            };

            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            engine.getMasterChain().addPlugin (std::make_unique<Generator>());
            juce::AudioBuffer<float> out (2, blockSize);

            engine.renderBlock (out, blockSize);
            expectWithinAbsoluteError (out.getSample (0, blockSize - 1), 0.5f, 0.001f);   // the generator reaches the output

            engine.stopAll();   // hard panic: a 5 ms (220 sample) ramp, then exact zero whatever still generates behind the gate
            engine.renderBlock (out, blockSize);
            expectGreaterThan (out.getSample (0, 0), 0.4f);              // the ramp starts from the open gate
            expectEquals (out.getSample (0, 300), 0.0f);                 // and is over well inside the block
            expectEquals (out.getSample (1, blockSize - 1), 0.0f);
            engine.renderBlock (out, blockSize);
            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);

            engine.fadeOutAndStopAll (100);   // a second panic on a closed gate: it stays closed

            for (int i = 0; i < 20; ++i)
                engine.renderBlock (out, blockSize);

            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);

            Cue cue;
            cue.name = "tone";
            cue.file = tone;
            juce::String error;
            expect (engine.play (cue, &error), error);
            engine.renderBlock (out, blockSize);
            expectGreaterThan (out.getMagnitude (0, 0, blockSize), 0.4f);   // reopened at once for the new cue

            // a soft panic: the fade time, then the 200 ms close ramp, then exact zero
            engine.fadeOutAndStopAll (100);
            const int blocks = (int) std::ceil (0.3 * sampleRate / blockSize) + 2;

            for (int i = 0; i < blocks; ++i)
                engine.renderBlock (out, blockSize);

            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);
            expectEquals (out.getMagnitude (1, 0, blockSize), 0.0f);

            // a refused start (no such file) leaves the gate shut
            Cue missing;
            missing.name = "missing";
            missing.file = dir.getChildFile ("nope.wav");
            expect (! engine.play (missing, &error));
            engine.renderBlock (out, blockSize);
            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);
        }

        dir.deleteRecursively();
    }
};

static PanicGateTests panicGateTests;

} // namespace gocue::tests
