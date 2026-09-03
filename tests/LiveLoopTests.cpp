#include "audio/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** The infinite-loop toggle and the play count reach a running cue at once (gom, 2026-09-03). */
class LiveLoopTests : public juce::UnitTest
{
public:
    LiveLoopTests() : juce::UnitTest ("Live loop toggle", "Enqueue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeSine (const juce::File& dir, double seconds)
    {
        const auto file = dir.getChildFile ("loop.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr);

        if (stream == nullptr)
            return {};

        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int numSamples = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));

        expect (writer->writeFromAudioSampleBuffer (buffer, 0, numSamples));
        return file;
    }

    static int blocksPerPass (double seconds) { return (int) std::ceil (seconds * sampleRate / blockSize); }

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("enqueue_liveloop_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const double seconds = 0.5;
        const auto tone = writeSine (dir, seconds);
        const int pass = blocksPerPass (seconds);   // 44 blocks

        beginTest ("infinite loop switched on while a single-pass cue plays keeps it playing past the end");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> out (2, blockSize);

            Cue cue;
            cue.name = "loop";
            cue.file = tone;
            cue.audio.playCount = 1;
            cue.audio.infiniteLoop = false;

            juce::String error;
            expect (engine.play (cue, &error), error);

            for (int i = 0; i < pass / 2; ++i)   // half way through the only pass
                engine.renderBlock (out, blockSize);

            expect (engine.isPlaying (cue.id));
            const double before = engine.getPlayingCues()[0].filePositionSeconds;

            engine.setLivePlayCount (cue.id, 1, true);   // the toggle, while playing

            engine.renderBlock (out, blockSize);
            const double after = engine.getPlayingCues().empty() ? -1.0 : engine.getPlayingCues()[0].filePositionSeconds;
            expect (after >= before, "the audible place is kept: " + juce::String (before) + " -> " + juce::String (after));
            expectLessThan (after - before, 0.05);   // one block on: no jump back to the start, no skip ahead
            expectLessThan (engine.getPlayingCues()[0].lengthSeconds, 0.0);   // reported as endless now

            for (int i = 0; i < pass * 3; ++i)   // three more pass lengths: a single pass would have ended long ago
                engine.renderBlock (out, blockSize);

            expect (engine.isPlaying (cue.id), "still playing: the loop is endless now");
            expectGreaterThan (out.getRMSLevel (0, 0, blockSize), 0.2f);   // and audible (0.5 amp sine ~ 0.35 rms)

            beginTest ("switched off again: the current pass plays to its end, then the cue stops");
            engine.setLivePlayCount (cue.id, 1, false);
            int blocks = 0;

            while (engine.isPlaying (cue.id) && blocks < pass * 3)
            {
                engine.renderBlock (out, blockSize);
                engine.reapFinishedPlayers();
                ++blocks;
            }

            expect (! engine.isPlaying (cue.id), "stopped after the pass");
            expectLessThan (blocks, pass + 4);   // within one pass length (plus the read-ahead refill slack)
            expectGreaterThan (blocks, 1);       // but not at once: the pass was finished first
        }

        beginTest ("a sliced cue: infinite on and off again keeps the slice sequence and finishes the current pass");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> out (2, blockSize);

            Cue cue;
            cue.name = "sliced";
            cue.file = tone;
            cue.audio.playCount = 1;
            cue.audio.slices.push_back ({ 0.25, 2 });   // second slice plays twice: sequence = 0.25 + 2 x 0.25 s
            cue.audio.firstSliceCount = 1;

            juce::String error;
            expect (engine.play (cue, &error), error);

            for (int i = 0; i < pass / 4; ++i)   // inside the first slice
                engine.renderBlock (out, blockSize);

            engine.setLivePlayCount (cue.id, 1, true);

            for (int i = 0; i < pass * 4; ++i)   // well past one sequence (0.75 s = 65 blocks)
                engine.renderBlock (out, blockSize);

            expect (engine.isPlaying (cue.id), "endless sequence keeps going");

            engine.setLivePlayCount (cue.id, 1, false);
            int blocks = 0;

            while (engine.isPlaying (cue.id) && blocks < pass * 4)
            {
                engine.renderBlock (out, blockSize);
                engine.reapFinishedPlayers();
                ++blocks;
            }

            expect (! engine.isPlaying (cue.id));
            expectLessThan (blocks, blocksPerPass (0.75) + 4);   // at most the rest of the current sequence pass
        }

        beginTest ("a bigger play count while playing adds passes without restarting");
        {
            AudioEngine engine (0);
            engine.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> out (2, blockSize);

            Cue cue;
            cue.name = "count";
            cue.file = tone;
            cue.audio.playCount = 1;

            juce::String error;
            expect (engine.play (cue, &error), error);

            for (int i = 0; i < pass / 2; ++i)
                engine.renderBlock (out, blockSize);

            engine.setLivePlayCount (cue.id, 3, false);   // three passes now

            int blocks = 0;

            while (engine.isPlaying (cue.id) && blocks < pass * 5)
            {
                engine.renderBlock (out, blockSize);
                engine.reapFinishedPlayers();
                ++blocks;
            }

            expect (! engine.isPlaying (cue.id));
            expectGreaterThan (blocks, pass * 2);         // the remaining half pass + two more passes
            expectLessThan (blocks, pass * 3);
        }

        dir.deleteRecursively();
    }
};

static LiveLoopTests liveLoopTests;

} // namespace gocue::tests
