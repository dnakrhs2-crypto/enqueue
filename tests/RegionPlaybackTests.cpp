#include "audio/AudioEngine.h"
#include "model/ProjectSerializer.h"

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

        beginTest ("a paused cue still obeys stop and fade requests");
        {
            Cue cue;
            cue.file = tone;
            expect (engine.play (cue));

            for (int i = 0; i < 5; ++i)
                engine.renderBlock (out, blockSize);

            engine.pause (cue.id);
            engine.renderBlock (out, blockSize);
            engine.renderBlock (out, blockSize);
            expect (engine.isPaused (cue.id));

            engine.fadeOutAndStop (cue.id);                        // F while paused
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();
            expect (! engine.isPlaying (cue.id));

            Cue other;
            other.file = tone;
            expect (engine.play (other));
            engine.renderBlock (out, blockSize);
            engine.pause (other.id);
            engine.renderBlock (out, blockSize);
            engine.renderBlock (out, blockSize);
            expect (engine.isPaused (other.id));
            engine.stopAll();                                      // hard stop while paused
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();
            expect (! engine.isPlaying (other.id));
        }

        beginTest ("a live trim keeps the audible file position and the elapsed time survives a rate change");
        {
            Cue cue;
            cue.file = tone;
            cue.audio.infiniteLoop = true;
            expect (engine.play (cue));

            for (int i = 0; i < 26; ++i)                          // 0.302 s
                engine.renderBlock (out, blockSize);

            const double filePosBefore = engine.getPlayingCues()[0].filePositionSeconds;
            expectWithinAbsoluteError (filePosBefore, 0.302, 0.02);

            engine.setLiveRegion (cue.id, 0.1, 0.9);               // start moved after the position: same file position
            engine.renderBlock (out, blockSize);
            const auto after = engine.getPlayingCues()[0];
            expectWithinAbsoluteError (after.filePositionSeconds, filePosBefore + blockSize / sampleRate, 0.02);
            expectEquals (after.passIndex, 0);

            const double elapsed = after.positionSeconds;
            engine.setLiveRate (cue.id, 2.0);
            engine.renderBlock (out, blockSize);
            expectWithinAbsoluteError (engine.getPlayingCues()[0].positionSeconds, elapsed + blockSize / sampleRate, 1e-3);   // no jump

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

        beginTest ("slices: play counts per slice, skipped slices, total length");
        {
            // three 0.2 s sections at different levels: 0.5 / 0.25 / 0.125
            const auto sliced = dir.getChildFile ("sliced.wav");
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> stream (sliced.createOutputStream());
                auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
                expect (writer != nullptr);
                const int n = (int) (0.6 * sampleRate);
                juce::AudioBuffer<float> buffer (2, n);

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                    {
                        const float amp = i < 0.2 * sampleRate ? 0.5f : i < 0.4 * sampleRate ? 0.25f : 0.125f;
                        buffer.setSample (ch, i, amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));
                    }

                expect (writer->writeFromAudioSampleBuffer (buffer, 0, n));
            }

            Cue cue;
            cue.file = sliced;
            cue.durationSeconds = 0.6;
            cue.audio.firstSliceCount = 2;                 // [0, 0.2) twice
            cue.audio.slices = { { 0.2, 0 }, { 0.4, 1 } };  // [0.2, 0.4) skipped, [0.4, 0.6) once
            cue.sanitise();
            expectWithinAbsoluteError (cue.effectiveLength(), 0.6, 1e-9);

            expect (engine.play (cue));
            const auto info = engine.getPlayingCues();
            expectEquals ((int) info.size(), 1);
            expectWithinAbsoluteError (info[0].lengthSeconds, 0.6, 0.001);

            // 0.4 s of the loud section (rms 0.354), then 0.2 s of the quiet one (0.088), then nothing
            std::vector<float> levels;
            int rendered = 0;

            for (int i = 0; i < 80; ++i)
            {
                engine.renderBlock (out, blockSize);
                ++rendered;
                levels.push_back (rms (out));
                engine.reapFinishedPlayers();

                if (! engine.isPlaying (cue.id))
                    break;
            }

            const int loudBlocks = (int) (0.4 * sampleRate / blockSize);
            expectWithinAbsoluteError (levels[(size_t) (loudBlocks / 2)], 0.3536f, 0.02f);
            expectWithinAbsoluteError (levels[(size_t) (loudBlocks + 8)], 0.0884f, 0.01f);
            expectWithinAbsoluteError (blocksToSeconds (rendered), 0.6, 0.05);
            expect (! engine.isPlaying (cue.id));

            beginTest ("slices: an endless slice loops until devamp, then the next slice follows");
            Cue endless;
            endless.file = sliced;
            endless.durationSeconds = 0.6;
            endless.audio.firstSliceCount = 1;
            endless.audio.slices = { { 0.2, -1 }, { 0.4, 1 } };
            endless.sanitise();
            expectWithinAbsoluteError (endless.effectiveLength(), -1.0, 1e-9);
            expect (engine.play (endless));
            expect (engine.getPlayingCues()[0].lengthSeconds < 0.0);

            for (int i = 0; i < (int) (1.0 * sampleRate / blockSize); ++i)   // 1 s: well inside the endless middle section
                engine.renderBlock (out, blockSize);

            expectWithinAbsoluteError (rms (out), 0.1768f, 0.02f);
            expect (engine.isPlaying (endless.id));
            engine.finishCurrentPass (endless.id);   // devamp: finish this pass of the middle slice, then the last slice

            levels.clear();
            rendered = 0;

            for (int i = 0; i < 80; ++i)
            {
                engine.renderBlock (out, blockSize);
                ++rendered;
                levels.push_back (rms (out));
                engine.reapFinishedPlayers();

                if (! engine.isPlaying (endless.id))
                    break;
            }

            expect (! engine.isPlaying (endless.id));
            expect (blocksToSeconds (rendered) < 0.45);   // at most the rest of the pass (< 0.2 s) + the last slice (0.2 s)
            bool sawQuiet = false;

            for (float l : levels)
                if (std::abs (l - 0.0884f) < 0.012f)
                    sawQuiet = true;

            expect (sawQuiet);   // the last slice was played after the devamp

            beginTest ("slices: every slice skipped is refused with a message");
            Cue none;
            none.file = sliced;
            none.audio.firstSliceCount = 0;
            none.audio.slices = { { 0.3, 0 } };
            juce::String error;
            expect (! engine.play (none, &error));
            expect (error.isNotEmpty());

            beginTest ("slice markers sanitise: sorted, min gap, clamped counts, serialised");
            Cue messy;
            messy.file = sliced;
            messy.durationSeconds = 0.6;
            messy.audio.slices = { { 0.5, 5 }, { 0.1, -3 }, { 0.12, 2 }, { 0.3, 20000 } };
            messy.audio.firstSliceCount = -7;
            messy.sanitise();
            expectEquals ((int) messy.audio.slices.size(), 3);   // 0.12 collapsed into 0.1
            expectWithinAbsoluteError (messy.audio.slices[0].seconds, 0.1, 1e-9);
            expectEquals (messy.audio.slices[0].playCount, -1);
            expectEquals (messy.audio.slices[1].playCount, Slice::maxCount);
            expectEquals (messy.audio.firstSliceCount, -1);

            Project p;
            p.cues.push_back (messy);
            Project q;
            expect (ProjectSerializer::fromJson (ProjectSerializer::toJson (p), q, nullptr).wasOk());
            expectEquals ((int) q.cues[0].audio.slices.size(), 3);
            expectEquals (q.cues[0].audio.slices[2].playCount, 5);
            expectEquals (q.cues[0].audio.firstSliceCount, -1);
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
