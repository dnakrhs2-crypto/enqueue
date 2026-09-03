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

    /** A 50 ms tone: its stream ends within a few blocks, which puts a player with a tailed chain into its tail. */
    juce::File writeShortTone (const juce::File& dir)
    {
        const auto file = dir.getChildFile ("short.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr);

        if (stream == nullptr)
            return {};

        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int numSamples = (int) (0.05 * sampleRate);
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

        beginTest ("a soft panic ends every instance without waiting for a declared plugin tail, and the chains are reset once the gate is closed");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            auto* master = new TestGainPlugin (1.0f, 5.0);   // five seconds of declared tail
            engine.getMasterChain().addPlugin (std::unique_ptr<juce::AudioPluginInstance> (master));
            juce::AudioBuffer<float> out (2, blockSize);

            Cue cue;
            cue.name = "tone";
            cue.file = tone;
            juce::String error;
            expect (engine.play (cue, &error), error);

            for (int i = 0; i < 10; ++i)
                engine.renderBlock (out, blockSize);

            engine.fadeOutAndStopAll (100);
            const int blocks = (int) std::ceil (0.5 * sampleRate / blockSize);   // fade 100 ms + gate close 200 ms, with margin

            for (int i = 0; i < blocks; ++i)
            {
                engine.renderBlock (out, blockSize);
                engine.reapIfNeeded();   // the timer's job: reaping and the polled chain reset
            }

            expectEquals (engine.getNumPlaying(), 0);   // not held for five seconds by the tail
            expect (! engine.mayBePlaying());           // the conservative flag follows the reap
            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);
            expectGreaterThan (master->resetCount, 0);   // the chain was reset behind the closed gate
        }

        beginTest ("a start during a soft panic is refused until the gate closed and the chains were reset; then it plays");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            auto* master = new TestGainPlugin (1.0f, 5.0);
            engine.getMasterChain().addPlugin (std::unique_ptr<juce::AudioPluginInstance> (master));
            juce::AudioBuffer<float> out (2, blockSize);

            Cue cue;
            cue.name = "tone";
            cue.file = tone;
            juce::String error;
            expect (engine.play (cue, &error), error);

            for (int i = 0; i < 10; ++i)
                engine.renderBlock (out, blockSize);

            engine.fadeOutAndStopAll (500);   // the controller's wall-clock latch can run out before the audio clock gets here
            expect (engine.isResetOutstanding());

            for (int i = 0; i < 4; ++i)
            {
                engine.renderBlock (out, blockSize);
                engine.reapIfNeeded();
            }

            Cue again;
            again.name = "again";
            again.file = tone;
            expect (! engine.play (again, &error));   // the reset has not run: refused, and the gate does not reopen
            expect (error.isNotEmpty());
            expectEquals (master->resetCount, 0);
            expect (engine.isResetOutstanding());

            const int blocks = (int) std::ceil (0.8 * sampleRate / blockSize);   // fade 500 ms + close 200 ms, with margin

            for (int i = 0; i < blocks; ++i)
            {
                engine.renderBlock (out, blockSize);
                engine.reapIfNeeded();
            }

            expectEquals (out.getMagnitude (0, 0, blockSize), 0.0f);
            expectGreaterThan (master->resetCount, 0);
            expect (! engine.isResetOutstanding());
            expect (engine.play (again, &error), error);

            for (int i = 0; i < 4; ++i)
                engine.renderBlock (out, blockSize);

            expectGreaterThan (out.getRMSLevel (0, 0, blockSize), 0.2f);
        }

        beginTest ("a hard stop's chain reset cannot be skipped by an immediate restart");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            auto* master = new TestGainPlugin (1.0f, 5.0);
            engine.getMasterChain().addPlugin (std::unique_ptr<juce::AudioPluginInstance> (master));
            juce::AudioBuffer<float> out (2, blockSize);

            Cue cue;
            cue.name = "tone";
            cue.file = tone;
            juce::String error;
            expect (engine.play (cue, &error), error);

            for (int i = 0; i < 5; ++i)
                engine.renderBlock (out, blockSize);

            engine.stopAll();
            Cue again;
            again.name = "again";
            again.file = tone;
            expect (! engine.play (again, &error));   // the gate has not even closed: the reset is still owed
            expectEquals (master->resetCount, 0);

            for (int i = 0; i < 3; ++i)   // the 5 ms close and the stopping instance's de-click are over well inside
            {
                engine.renderBlock (out, blockSize);
                engine.reapIfNeeded();
            }

            expectGreaterThan (master->resetCount, 0);
            expect (engine.play (again, &error), error);
            engine.renderBlock (out, blockSize);
            expectGreaterThan (out.getMagnitude (0, 0, blockSize), 0.2f);
        }

        beginTest ("a soft panic fades a ringing insert tail over the panic time instead of cutting it");
        {
            struct TailGenerator : public TestGainPlugin
            {
                TailGenerator() : TestGainPlugin (1.0f, 3.0) {}   // three seconds of declared tail, and it keeps generating

                void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
                {
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.5f, buffer.getNumSamples());
                }
            };

            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> out (2, blockSize);
            const auto shortTone = writeShortTone (dir);

            Cue cue;
            cue.name = "short";
            cue.file = shortTone;
            engine.getCueChain (cue.id).addPlugin (std::make_unique<TailGenerator>());
            juce::String error;
            expect (engine.play (cue, &error), error);

            // render until the 50 ms stream is over (progress stays at 1 through the tail), then a little more: the
            // instance sits in its 3 s tail
            int blocksToTail = 0;

            for (; blocksToTail < 60; ++blocksToTail)
            {
                engine.renderBlock (out, blockSize);
                const auto playing = engine.getPlayingCues();

                if (playing.size() == 1 && playing[0].progress >= 0.999)
                    break;
            }

            expectLessThan (blocksToTail, 12);   // 50 ms of audio, 11.6 ms per block

            for (int i = 0; i < 3; ++i)
                engine.renderBlock (out, blockSize);

            expectEquals (engine.getNumPlaying(), 1);
            expectWithinAbsoluteError (out.getSample (0, blockSize - 1), 0.5f, 0.01f);   // the tail is audible

            engine.fadeOutAndStopAll (200);   // the tail ramps down over 200 ms; the gate closes after that
            engine.renderBlock (out, blockSize);
            expectGreaterThan (out.getSample (0, 0), 0.45f);                             // the fade starts from the ringing level
            expectLessThan (out.getSample (0, blockSize - 1), out.getSample (0, 0));     // and goes down inside the block
            expectGreaterThan (out.getSample (0, blockSize - 1), 0.3f);                  // not cut

            const int fadeBlocks = (int) std::ceil (0.2 * sampleRate / blockSize);

            for (int i = 0; i < fadeBlocks / 2; ++i)
                engine.renderBlock (out, blockSize);

            expectEquals (engine.getNumPlaying(), 1);   // still fading halfway through
            expectGreaterThan (out.getMagnitude (0, 0, blockSize), 0.1f);

            for (int i = 0; i < fadeBlocks; ++i)
            {
                engine.renderBlock (out, blockSize);
                engine.reapIfNeeded();
            }

            expectEquals (engine.getNumPlaying(), 0);   // the tail fade ended the instance
        }

        dir.deleteRecursively();
    }
};

static PanicGateTests panicGateTests;

} // namespace gocue::tests
