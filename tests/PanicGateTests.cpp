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

        beginTest ("the panic fade outlasts a shorter declared tail and reaches zero; a source that ends mid-panic keeps fading after the chain");
        {
            struct ShortTailGenerator : public TestGainPlugin
            {
                ShortTailGenerator() : TestGainPlugin (1.0f, 0.1) {}   // 100 ms of declared tail, and it keeps generating

                void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
                {
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.5f, buffer.getNumSamples());
                }
            };

            const auto shortTone = writeShortTone (dir);
            const int fadeBlocks = (int) std::ceil (0.2 * sampleRate / blockSize);   // 200 ms

            // (a) in the tail, with only 100 ms of declared tail left: the 200 ms panic fade still runs to zero
            {
                AudioEngine engine (0);
                engine.prepare (sampleRate, blockSize);
                juce::AudioBuffer<float> out (2, blockSize);
                Cue cue;
                cue.name = "short";
                cue.file = shortTone;
                engine.getCueChain (cue.id).addPlugin (std::make_unique<ShortTailGenerator>());
                juce::String error;
                expect (engine.play (cue, &error), error);

                for (int i = 0; i < 60; ++i)   // until the stream is over (the tail then has ~100 ms left)
                {
                    engine.renderBlock (out, blockSize);
                    const auto playing = engine.getPlayingCues();

                    if (playing.size() == 1 && playing[0].progress >= 0.999)
                        break;
                }

                expectEquals (engine.getNumPlaying(), 1);
                engine.fadeOutAndStopAll (200);
                float lastLevel = 1.0f;

                for (int i = 0; i < fadeBlocks - 1; ++i)
                {
                    engine.renderBlock (out, blockSize);
                    engine.reapIfNeeded();
                    lastLevel = out.getSample (0, blockSize - 1);
                }

                expectEquals (engine.getNumPlaying(), 1);        // alive past the declared 100 ms tail: the fade is in charge
                expectGreaterThan (lastLevel, 0.001f);            // and still on its way down, not cut
                expectLessThan (lastLevel, 0.1f);

                for (int i = 0; i < 3; ++i)
                {
                    engine.renderBlock (out, blockSize);
                    engine.reapIfNeeded();
                }

                expectEquals (engine.getNumPlaying(), 0);
            }

            // (b) the panic arrives while the source still streams and the source ends during the fade: the chain keeps
            //     ringing through the rest of the panic time, fading after the chain, instead of cutting at the file's end
            {
                AudioEngine engine (0);
                engine.prepare (sampleRate, blockSize);
                juce::AudioBuffer<float> out (2, blockSize);
                Cue cue;
                cue.name = "short";
                cue.file = shortTone;
                engine.getCueChain (cue.id).addPlugin (std::make_unique<ShortTailGenerator>());
                juce::String error;
                expect (engine.play (cue, &error), error);
                engine.renderBlock (out, blockSize);   // 11 ms in: the 50 ms file is still streaming

                engine.fadeOutAndStopAll (200);

                for (int i = 0; i < 8; ++i)   // 93 ms: the file ended (at ~50 ms) inside the fade
                {
                    engine.renderBlock (out, blockSize);
                    engine.reapIfNeeded();
                }

                expectEquals (engine.getNumPlaying(), 1);                          // not cut when the file ended
                expectGreaterThan (out.getMagnitude (0, 0, blockSize), 0.15f);     // the chain still rings, fading (about half way)
                expectLessThan (out.getMagnitude (0, 0, blockSize), 0.45f);

                for (int i = 0; i < fadeBlocks; ++i)
                {
                    engine.renderBlock (out, blockSize);
                    engine.reapIfNeeded();
                }

                expectEquals (engine.getNumPlaying(), 0);   // gone once the 200 ms are over
            }
        }

        beginTest ("a soft panic lets the insert of a streaming cue ring out after the envelope fade, and a paused cue's insert too");
        {
            struct Generator : public TestGainPlugin
            {
                Generator() : TestGainPlugin (1.0f, 3.0) {}

                void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
                {
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.5f, buffer.getNumSamples());
                }
            };

            const int fadeBlocks = (int) std::ceil (0.2 * sampleRate / blockSize);   // 200 ms

            // streaming: the envelope fade takes the panic time, then the insert fades after itself over the gate's close
            {
                AudioEngine engine (0);
                engine.prepare (sampleRate, blockSize);
                juce::AudioBuffer<float> out (2, blockSize);
                Cue cue;
                cue.name = "tone";
                cue.file = tone;
                engine.getCueChain (cue.id).addPlugin (std::make_unique<Generator>());
                juce::String error;
                expect (engine.play (cue, &error), error);

                for (int i = 0; i < 5; ++i)
                    engine.renderBlock (out, blockSize);

                engine.fadeOutAndStopAll (200);

                for (int i = 0; i < fadeBlocks + 1; ++i)   // the envelope fade is over
                {
                    engine.renderBlock (out, blockSize);
                    engine.reapIfNeeded();
                }

                expectEquals (engine.getNumPlaying(), 1);                        // not cut: the insert rings on, fading
                expectGreaterThan (out.getMagnitude (0, 0, blockSize), 0.05f);

                for (int i = 0; i < fadeBlocks + 2; ++i)
                {
                    engine.renderBlock (out, blockSize);
                    engine.reapIfNeeded();
                }

                expectEquals (engine.getNumPlaying(), 0);   // gone with the gate
            }

            // paused: the insert of a paused cue fades after the chain over the panic time instead of ending at once
            {
                AudioEngine engine (0);
                engine.prepare (sampleRate, blockSize);
                juce::AudioBuffer<float> out (2, blockSize);
                Cue cue;
                cue.name = "tone";
                cue.file = tone;
                engine.getCueChain (cue.id).addPlugin (std::make_unique<Generator>());
                juce::String error;
                expect (engine.play (cue, &error), error);

                for (int i = 0; i < 5; ++i)
                    engine.renderBlock (out, blockSize);

                engine.pause (cue.id);

                for (int i = 0; i < 3; ++i)   // the pause de-click
                    engine.renderBlock (out, blockSize);

                expect (engine.isPaused (cue.id));
                engine.fadeOutAndStopAll (200);

                for (int i = 0; i < 3; ++i)
                {
                    engine.renderBlock (out, blockSize);
                    engine.reapIfNeeded();
                }

                expectEquals (engine.getNumPlaying(), 1);                        // alive: fading after the chain
                expectGreaterThan (out.getMagnitude (0, 0, blockSize), 0.2f);

                for (int i = 0; i < fadeBlocks + 2; ++i)
                {
                    engine.renderBlock (out, blockSize);
                    engine.reapIfNeeded();
                }

                expectEquals (engine.getNumPlaying(), 0);
            }
        }

        dir.deleteRecursively();
    }
};

static PanicGateTests panicGateTests;

} // namespace gocue::tests
