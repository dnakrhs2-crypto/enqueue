#include "audio/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** Trim / loop / envelope / rate / pause behaviour of the player, rendered offline. */
class RegionPlaybackTests : public juce::UnitTest
{
public:
    RegionPlaybackTests() : juce::UnitTest ("RegionPlayback (offline)", "GoCue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeSine (const juce::File& dir, const juce::String& fileName, double seconds, float amplitude)
    {
        const auto file = dir.getChildFile (fileName);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr);

        if (stream == nullptr)
            return {};

        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (sampleRate)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (16));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int numSamples = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));

        expect (writer->writeFromAudioSampleBuffer (buffer, 0, numSamples));
        return file;
    }

    static float rms (const juce::AudioBuffer<float>& buffer)
    {
        return buffer.getRMSLevel (0, 0, buffer.getNumSamples());
    }

    /** Renders until the cue finishes (or maxBlocks). Returns the number of blocks whose RMS was above 'threshold'. */
    static int renderUntilDone (AudioEngine& engine, const juce::Uuid& id, juce::AudioBuffer<float>& out,
                                int maxBlocks, int& blocksRendered, float threshold = 0.01f)
    {
        int loud = 0;
        blocksRendered = 0;

        for (int i = 0; i < maxBlocks; ++i)
        {
            engine.renderBlock (out, blockSize);
            ++blocksRendered;

            if (rms (out) > threshold)
                ++loud;

            engine.reapFinishedPlayers();

            if (! engine.isPlaying (id))
                break;
        }

        return loud;
    }

    static double blocksToSeconds (int blocks)
    {
        return blocks * (double) blockSize / sampleRate;
    }

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("gocue_region_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());

        const auto tone = writeSine (dir, "tone.wav", 1.0, 0.5f);

        AudioEngine engine (0);            // synchronous reads: deterministic offline rendering
        engine.prepare (sampleRate, blockSize);
        juce::AudioBuffer<float> out (2, blockSize);

        beginTest ("trim plays only the region and reports its length");
        {
            Cue cue;
            cue.file = tone;
            cue.audio.startSeconds = 0.2;
            cue.audio.endSeconds = 0.5;

            juce::String error;
            expect (engine.play (cue, &error), error);

            const auto playing = engine.getPlayingCues();
            expectEquals ((int) playing.size(), 1);
            expectWithinAbsoluteError (playing[0].lengthSeconds, 0.3, 1e-6);
            expectWithinAbsoluteError (playing[0].filePositionSeconds, 0.2, 1e-6);

            int blocks = 0;
            const int loud = renderUntilDone (engine, cue.id, out, 200, blocks);
            expectWithinAbsoluteError (blocksToSeconds (loud), 0.3, blocksToSeconds (2));
            expect (! engine.isPlaying (cue.id));
        }

        beginTest ("play count repeats the region seamlessly");
        {
            Cue cue;
            cue.file = tone;
            cue.audio.startSeconds = 0.2;
            cue.audio.endSeconds = 0.5;
            cue.audio.playCount = 3;
            expect (engine.play (cue));
            expectWithinAbsoluteError (engine.getPlayingCues()[0].lengthSeconds, 0.9, 1e-6);

            for (int i = 0; i < 35; ++i)      // 0.406 s: into the second pass
                engine.renderBlock (out, blockSize);

            const auto mid = engine.getPlayingCues()[0];
            expectEquals (mid.passIndex, 1);
            expectWithinAbsoluteError (mid.positionSeconds, blocksToSeconds (35), 1e-3);
            expectWithinAbsoluteError (mid.filePositionSeconds, 0.2 + (blocksToSeconds (35) - 0.3), 1e-3);
            expectGreaterThan (rms (out), 0.3f);                         // no gap at the pass boundary

            int blocks = 0;
            const int loud = renderUntilDone (engine, cue.id, out, 200, blocks);
            expectWithinAbsoluteError (blocksToSeconds (loud + 35), 0.9, blocksToSeconds (2));
        }

        beginTest ("infinite loops keep going until the current pass is finished on request");
        {
            Cue cue;
            cue.file = tone;
            cue.audio.startSeconds = 0.0;
            cue.audio.endSeconds = 0.3;
            cue.audio.infiniteLoop = true;
            expect (engine.play (cue));
            expectWithinAbsoluteError (engine.getPlayingCues()[0].lengthSeconds, -1.0, 1e-12);

            for (int i = 0; i < 300; ++i)     // 3.5 s: far beyond one pass
                engine.renderBlock (out, blockSize);

            engine.reapFinishedPlayers();
            expect (engine.isPlaying (cue.id));
            expectGreaterThan (rms (out), 0.3f);
            const int passBefore = engine.getPlayingCues()[0].passIndex;
            expectGreaterThan (passBefore, 5);

            engine.finishCurrentPass (cue.id);
            expectWithinAbsoluteError (engine.getPlayingCues()[0].lengthSeconds, 0.3 * (passBefore + 1), 1e-6);

            int blocks = 0;
            renderUntilDone (engine, cue.id, out, 200, blocks);
            expect (! engine.isPlaying (cue.id));
            const double expectedRemaining = 0.3 * (passBefore + 1) - blocksToSeconds (300);
            expectWithinAbsoluteError (blocksToSeconds (blocks), expectedRemaining, blocksToSeconds (2));
        }

        beginTest ("the integrated envelope shapes the level per pass");
        {
            Cue cue;
            cue.file = tone;
            cue.audio.endSeconds = 0.4;
            cue.audio.playCount = 2;
            cue.audio.envelope.enabled = true;
            cue.audio.envelope.linear = true;
            cue.audio.envelope.lockToTrim = false;
            cue.audio.envelope.points = { { 0.0, 0.0 }, { 0.1, 1.0 }, { 0.3, 1.0 }, { 0.4, 0.0 } };
            expect (engine.play (cue));

            const float full = 0.5f / std::sqrt (2.0f);          // sine amplitude 0.5

            engine.renderBlock (out, blockSize);                   // 0 .. 11.6 ms: ramping up from silence
            expectLessThan (rms (out), full * 0.2f);

            for (int i = 0; i < 16; ++i)                          // -> 0.197 s: flat top
                engine.renderBlock (out, blockSize);

            expectWithinAbsoluteError (rms (out), full, 0.02f);

            for (int i = 0; i < 16; ++i)                          // -> 0.383 s: near the end of the fade-out
                engine.renderBlock (out, blockSize);

            expectLessThan (rms (out), full * 0.3f);

            for (int i = 0; i < 20; ++i)                          // -> 0.615 s: second pass, flat top again
                engine.renderBlock (out, blockSize);

            expectWithinAbsoluteError (rms (out), full, 0.03f);
            engine.stopAll();
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();
        }

        beginTest ("rate scales the length and can change while playing");
        {
            Cue cue;
            cue.file = tone;
            cue.audio.endSeconds = 0.4;
            cue.audio.rate = 2.0;
            expect (engine.play (cue));
            expectWithinAbsoluteError (engine.getPlayingCues()[0].lengthSeconds, 0.2, 1e-6);

            int blocks = 0;
            const int loud = renderUntilDone (engine, cue.id, out, 200, blocks);
            expectWithinAbsoluteError (blocksToSeconds (loud), 0.2, blocksToSeconds (2));

            Cue slow;
            slow.file = tone;
            slow.audio.endSeconds = 0.4;
            expect (engine.play (slow));
            engine.setLiveRate (slow.id, 0.5);
            expectWithinAbsoluteError (engine.getPlayingCues()[0].lengthSeconds, 0.8, 1e-6);
            const int loudSlow = renderUntilDone (engine, slow.id, out, 200, blocks);
            expectWithinAbsoluteError (blocksToSeconds (loudSlow), 0.8, blocksToSeconds (2));
        }

        beginTest ("pause freezes the position and resume continues");
        {
            Cue cue;
            cue.file = tone;
            expect (engine.play (cue));

            for (int i = 0; i < 10; ++i)
                engine.renderBlock (out, blockSize);

            engine.pause (cue.id);
            engine.renderBlock (out, blockSize);                   // de-click ramp block
            engine.renderBlock (out, blockSize);
            expect (engine.isPaused (cue.id));
            expectWithinAbsoluteError (rms (out), 0.0f, 1e-4f);
            const double frozen = engine.getPlayingCues()[0].positionSeconds;
            expect (engine.getPlayingCues()[0].paused);

            for (int i = 0; i < 20; ++i)
                engine.renderBlock (out, blockSize);

            expectWithinAbsoluteError (engine.getPlayingCues()[0].positionSeconds, frozen, 1e-9);
            expect (engine.isPlaying (cue.id));

            engine.resume (cue.id);
            engine.renderBlock (out, blockSize);
            engine.renderBlock (out, blockSize);
            expect (! engine.isPaused (cue.id));
            expectGreaterThan (rms (out), 0.3f);
            expectGreaterThan (engine.getPlayingCues()[0].positionSeconds, frozen + 0.01);

            engine.pauseAll();
            engine.renderBlock (out, blockSize);
            engine.renderBlock (out, blockSize);
            expect (engine.isPaused (cue.id));
            engine.resumeAll();
            engine.renderBlock (out, blockSize);
            expect (! engine.isPaused (cue.id));

            engine.stopAll();
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();
        }

        beginTest ("gain changes apply to a running cue");
        {
            Cue cue;
            cue.file = tone;
            cue.gainDb = 0.0;
            expect (engine.play (cue));

            const float full = 0.5f / std::sqrt (2.0f);
            for (int i = 0; i < 3; ++i)
                engine.renderBlock (out, blockSize);

            expectWithinAbsoluteError (rms (out), full, 0.02f);

            engine.setLiveGainDb (cue.id, -6.0206);                // half
            engine.renderBlock (out, blockSize);                   // ramp block
            engine.renderBlock (out, blockSize);
            expectWithinAbsoluteError (rms (out), full * 0.5f, 0.02f);

            engine.setLiveGainDb (cue.id, Cue::minGainDb);         // silence
            engine.renderBlock (out, blockSize);
            engine.renderBlock (out, blockSize);
            expectWithinAbsoluteError (rms (out), 0.0f, 1e-4f);
            expect (engine.isPlaying (cue.id));                    // still running, just silent

            engine.setLiveGainDb (cue.id, 0.0);
            engine.renderBlock (out, blockSize);
            engine.renderBlock (out, blockSize);
            expectWithinAbsoluteError (rms (out), full, 0.02f);

            engine.stopAll();
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();
        }

        beginTest ("a live trim that ends before the playhead stops the cue at once");
        {
            Cue cue;
            cue.file = tone;
            expect (engine.play (cue));

            for (int i = 0; i < 20; ++i)                          // 0.232 s
                engine.renderBlock (out, blockSize);

            engine.setLiveRegion (cue.id, 0.0, 0.1);
            expectWithinAbsoluteError (engine.getPlayingCues()[0].lengthSeconds, 0.1, 1e-6);

            int blocks = 0;
            renderUntilDone (engine, cue.id, out, 50, blocks);
            expect (! engine.isPlaying (cue.id));
            expectLessOrEqual (blocks, 2);

            // extending the end while playing keeps it going
            Cue longer;
            longer.file = tone;
            longer.audio.endSeconds = 0.2;
            expect (engine.play (longer));
            engine.setLiveRegion (longer.id, 0.0, 0.6);
            expectWithinAbsoluteError (engine.getPlayingCues()[0].lengthSeconds, 0.6, 1e-6);
            const int loud = renderUntilDone (engine, longer.id, out, 200, blocks);
            expectWithinAbsoluteError (blocksToSeconds (loud), 0.6, blocksToSeconds (2));
        }

        beginTest ("a start offset begins later in the region");
        {
            Cue cue;
            cue.file = tone;
            cue.audio.startSeconds = 0.1;
            cue.audio.endSeconds = 0.6;

            AudioEngine::PlayOptions options;
            options.startSeconds = 0.3;
            expect (engine.play (cue, options));
            expectWithinAbsoluteError (engine.getPlayingCues()[0].positionSeconds, 0.3, 1e-6);
            expectWithinAbsoluteError (engine.getPlayingCues()[0].filePositionSeconds, 0.4, 1e-6);

            int blocks = 0;
            const int loud = renderUntilDone (engine, cue.id, out, 200, blocks);
            expectWithinAbsoluteError (blocksToSeconds (loud), 0.2, blocksToSeconds (2));
        }

        beginTest ("the disk read-ahead path plays the same region and loops");
        {
            AudioEngine buffered (65536);          // real background read-ahead thread, as in the app
            buffered.prepare (sampleRate, blockSize);

            Cue cue;
            cue.file = tone;
            cue.audio.startSeconds = 0.2;
            cue.audio.endSeconds = 0.5;
            cue.audio.playCount = 2;

            juce::String error;
            expect (buffered.play (cue, &error), error);
            expectWithinAbsoluteError (buffered.getPlayingCues()[0].lengthSeconds, 0.6, 1e-6);

            int loud = 0, blocks = 0;

            for (int i = 0; i < 200; ++i)
            {
                buffered.renderBlock (out, blockSize);
                ++blocks;

                if (rms (out) > 0.01f)
                    ++loud;

                buffered.reapFinishedPlayers();

                if (! buffered.isPlaying (cue.id))
                    break;

                juce::Thread::sleep (1);          // let the read-ahead thread keep up
            }

            expect (! buffered.isPlaying (cue.id));
            expectWithinAbsoluteError (blocksToSeconds (loud), 0.6, blocksToSeconds (3));
            buffered.shutdown();
        }

        beginTest ("an empty region is refused with a message");
        {
            Cue cue;
            cue.file = tone;
            cue.durationSeconds = 1.0;
            cue.audio.startSeconds = 0.999999;
            cue.audio.endSeconds = 1.0;
            cue.audio.endSeconds = 0.9999995;   // sanitise keeps it (end > start) but it rounds to < 1 sample

            juce::String error;
            expect (! engine.play (cue, &error));
            expect (error.isNotEmpty());
        }

        expect (dir.deleteRecursively());
    }
};

static RegionPlaybackTests regionPlaybackTests;

} // namespace gocue::tests
