#include "audio/AudioEngine.h"
#include "audio/ReadAheadSource.h"
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

        beginTest ("a live envelope edit is heard on the very next block");
        {
            Cue cue;
            cue.file = tone;
            cue.audio.endSeconds = 0.4;   // no envelope to begin with
            expect (engine.play (cue));

            const float full = 0.5f / std::sqrt (2.0f);

            for (int i = 0; i < 8; ++i)
                engine.renderBlock (out, blockSize);

            expectWithinAbsoluteError (rms (out), full, 0.02f);

            Envelope silent;
            silent.enabled = true;
            silent.linear = true;
            silent.lockToTrim = false;
            silent.points = { { 0.0, 0.0 }, { 0.4, 0.0 } };
            engine.setLiveEnvelope (cue.id, silent);
            engine.renderBlock (out, blockSize);   // the speed stage holds a few pre-read samples: nearly silent already
            expectLessThan (rms (out), full * 0.12f);
            engine.renderBlock (out, blockSize);
            expectLessThan (rms (out), full * 0.01f);

            Envelope off;
            off.enabled = false;
            engine.setLiveEnvelope (cue.id, off);
            engine.renderBlock (out, blockSize);
            expectWithinAbsoluteError (rms (out), full, 0.02f);

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

        beginTest ("pitch preserved at 2x: the length halves and 440 Hz stays; varispeed moves it to 880 Hz");
        {
            const auto twoSeconds = writeSine (dir, "two.wav", 2.0, 0.5f);

            auto goertzel = [] (const juce::AudioBuffer<float>& b, double hz)
            {
                const double k = 2.0 * std::cos (2.0 * juce::MathConstants<double>::pi * hz / sampleRate);
                double s0 = 0.0, s1 = 0.0, s2 = 0.0;

                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    s0 = b.getSample (0, i) + k * s1 - s2;
                    s2 = s1;
                    s1 = s0;
                }

                return s1 * s1 + s2 * s2 - k * s1 * s2;
            };

            auto measure = [&] (bool preservePitch, double& seconds, double& e440, double& e880)
            {
                Cue cue;
                cue.file = twoSeconds;
                cue.audio.rate = 2.0;
                cue.audio.preservePitch = preservePitch;
                expect (engine.play (cue));
                int rendered = 0;
                e440 = e880 = 0.0;

                for (int i = 0; i < 400; ++i)
                {
                    engine.renderBlock (out, blockSize);
                    ++rendered;

                    if (i >= 20 && i < 60)   // steady state, away from the start / end
                    {
                        e440 += goertzel (out, 440.0);
                        e880 += goertzel (out, 880.0);
                    }

                    engine.reapFinishedPlayers();

                    if (! engine.isPlaying (cue.id))
                        break;
                }

                seconds = blocksToSeconds (rendered);
            };

            double seconds = 0.0, e440 = 0.0, e880 = 0.0;
            measure (true, seconds, e440, e880);
            expectWithinAbsoluteError (seconds, 1.0, 0.12);   // 2 s file at 2x (+ a little stretcher latency)
            expect (e440 > 4.0 * e880);                       // the pitch stayed at 440

            measure (false, seconds, e440, e880);
            expectWithinAbsoluteError (seconds, 1.0, 0.05);
            expect (e880 > 4.0 * e440);                       // plain varispeed: an octave up
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
            p.cues().push_back (messy);
            Project q;
            expect (ProjectSerializer::fromJson (ProjectSerializer::toJson (p), q, nullptr).wasOk());
            expectEquals ((int) q.cues()[0].audio.slices.size(), 3);
            expectEquals (q.cues()[0].audio.slices[2].playCount, 5);
            expectEquals (q.cues()[0].audio.firstSliceCount, -1);
        }

        beginTest ("devamp with stop ends an endless loop at the pass boundary; without stop the file runs on");
        {
            // 0.5 s loop region of a 2 s file: [0, 0.5) forever
            const auto two = writeSine (dir, "two2.wav", 2.0, 0.5f);
            Cue loop;
            loop.file = two;
            loop.durationSeconds = 2.0;
            loop.audio.endSeconds = 0.5;
            loop.audio.infiniteLoop = true;
            expect (engine.play (loop));

            const int blocksPerPass = (int) (0.5 * sampleRate / blockSize);   // ~43
            for (int i = 0; i < blocksPerPass + 5; ++i)                       // into the second pass
                engine.renderBlock (out, blockSize);

            const double toEnd = engine.getSecondsToPassEnd (loop.id);
            expect (toEnd > 0.0 && toEnd < 0.5);
            engine.finishCurrentPass (loop.id, true);   // devamp + stop: the cue ends at the end of this pass
            int rendered = 0;

            for (int i = 0; i < 200; ++i)
            {
                engine.renderBlock (out, blockSize);
                ++rendered;
                engine.reapFinishedPlayers();

                if (! engine.isPlaying (loop.id))
                    break;
            }

            expect (! engine.isPlaying (loop.id));
            expectWithinAbsoluteError (blocksToSeconds (rendered), toEnd, 0.03);

            // without stop on a one-slice endless loop there is nothing after the loop, so it also ends there
            Cue loop2 = loop;
            loop2.id = juce::Uuid();
            expect (engine.play (loop2));

            for (int i = 0; i < 10; ++i)
                engine.renderBlock (out, blockSize);

            engine.finishCurrentPass (loop2.id, false);
            rendered = 0;

            for (int i = 0; i < 200; ++i)
            {
                engine.renderBlock (out, blockSize);
                ++rendered;
                engine.reapFinishedPlayers();

                if (! engine.isPlaying (loop2.id))
                    break;
            }

            expect (! engine.isPlaying (loop2.id));
            expect (blocksToSeconds (rendered) < 0.5);
        }

        beginTest ("a marker at or before the trim start owns the first slice (model and engine agree)");
        {
            // 2 s file trimmed to [0.6, 1.0); a marker at 0.3 s (before the trim) says "forever": the region loops
            const auto two = writeSine (dir, "two3.wav", 2.0, 0.5f);
            Cue cue;
            cue.file = two;
            cue.durationSeconds = 2.0;
            cue.audio.startSeconds = 0.6;
            cue.audio.endSeconds = 1.0;
            cue.audio.slices = { { 0.3, -1 } };
            cue.audio.firstSliceCount = 1;
            expect (cue.audio.firstCountFor (0.6) == -1);
            expect (cue.audio.hasEndlessSlice (0.6, 1.0));
            expect (cue.effectiveLength() < 0.0);
            expect (engine.play (cue));

            for (int i = 0; i < (int) (1.5 * sampleRate / blockSize); ++i)   // well past the 0.4 s region
            {
                engine.renderBlock (out, blockSize);
                engine.reapFinishedPlayers();
            }

            expect (engine.isPlaying (cue.id));   // still looping
            engine.stopAll();
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();

            // a marker exactly at the start with count 0 skips the region's first slice: nothing to play
            Cue skipped = cue;
            skipped.id = juce::Uuid();
            skipped.audio.slices = { { 0.6, 0 } };
            expect (skipped.audio.firstCountFor (0.6) == 0);
            expect (! engine.play (skipped));
        }

        beginTest ("devamp on finite slices with an endless sequence ends at the sequence pass boundary");
        {
            // two slices of 0.25 s each, the whole sequence forever: devamp must finish the *sequence* pass
            const auto two = writeSine (dir, "two4.wav", 2.0, 0.5f);
            Cue cue;
            cue.file = two;
            cue.durationSeconds = 2.0;
            cue.audio.endSeconds = 0.5;
            cue.audio.slices = { { 0.25, 1 } };
            cue.audio.infiniteLoop = true;
            expect (engine.play (cue));

            for (int i = 0; i < 5; ++i)   // inside the first slice of the first sequence pass
                engine.renderBlock (out, blockSize);

            const double toEnd = engine.getSecondsToPassEnd (cue.id);
            expect (toEnd > 0.3 && toEnd < 0.5);                       // the sequence pass (0.5 s), not the slice (0.25 s)
            const double reported = engine.finishCurrentPass (cue.id, true);
            expectWithinAbsoluteError (reported, toEnd, 0.02);
            int rendered = 0;

            for (int i = 0; i < 200; ++i)
            {
                engine.renderBlock (out, blockSize);
                ++rendered;
                engine.reapFinishedPlayers();

                if (! engine.isPlaying (cue.id))
                    break;
            }

            expect (! engine.isPlaying (cue.id));
            expectWithinAbsoluteError (blocksToSeconds (rendered), toEnd, 0.03);

            // nothing endless and no stop: the devamp reports failure
            Cue once = cue;
            once.id = juce::Uuid();
            once.audio.infiniteLoop = false;
            expect (engine.play (once));
            engine.renderBlock (out, blockSize);
            expect (engine.finishCurrentPass (once.id, false) < 0.0);
            engine.stopAll();
            engine.renderBlock (out, blockSize);
            engine.reapFinishedPlayers();
        }

        beginTest ("read-ahead serves cached audio and invalidate() refills from the new position at once");
        {
            struct RampSource : public juce::PositionableAudioSource
            {
                juce::int64 pos = 0;
                int epoch = 0;   // "content" version: a layout change in the real source

                void prepareToPlay (int, double) override {}
                void releaseResources() override {}
                void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
                {
                    for (int i = 0; i < info.numSamples; ++i)
                        info.buffer->setSample (0, info.startSample + i, (float) (pos + i + (juce::int64) epoch * 1000000));

                    pos += info.numSamples;
                }
                void setNextReadPosition (juce::int64 p) override { pos = p; }
                juce::int64 getNextReadPosition() const override { return pos; }
                juce::int64 getTotalLength() const override { return 1 << 30; }
                bool isLooping() const override { return false; }
            };

            juce::TimeSliceThread thread ("read-ahead test");
            thread.startThread();

            {
                RampSource ramp;
                ReadAheadSource ahead (ramp, thread, 16384, 1);
                ahead.setNextReadPosition (1000);
                ahead.prepareToPlay (512, 44100.0);
                expect (ahead.getNumSamplesReady() >= 512);

                juce::AudioBuffer<float> block (1, 512);
                juce::AudioSourceChannelInfo info (&block, 0, 512);
                ahead.getNextAudioBlock (info);
                expectWithinAbsoluteError (block.getSample (0, 0), 1000.0f, 0.01f);
                expectWithinAbsoluteError (block.getSample (0, 511), 1511.0f, 0.01f);
                ahead.getNextAudioBlock (info);
                expectWithinAbsoluteError (block.getSample (0, 0), 1512.0f, 0.01f);   // continues from the cache

                // the upstream's content changes: after invalidate() nothing old is served; the thread refills from the new position
                ramp.epoch = 1;
                ahead.invalidate (5000);

                for (int tries = 0; tries < 200 && ahead.getNumSamplesReady() < 512; ++tries)
                    juce::Thread::sleep (5);

                expect (ahead.getNumSamplesReady() >= 512);
                ahead.getNextAudioBlock (info);
                expectWithinAbsoluteError (block.getSample (0, 0), 1005000.0f, 0.5f);
                expectWithinAbsoluteError (block.getSample (0, 511), 1005511.0f, 0.5f);

                // a plain position move inside the cached range keeps serving the cache (old content is fine there)
                ahead.setNextReadPosition (5100);
                ahead.getNextAudioBlock (info);
                expectWithinAbsoluteError (block.getSample (0, 0), 1005100.0f, 0.5f);
                ahead.releaseResources();
            }

            thread.stopThread (2000);
        }

        beginTest ("a mic cue routes the device inputs through its matrix until it is stopped");
        {
            Cue mic;
            mic.type = CueType::mic;
            mic.name = "mic";
            mic.mic.firstInput = 1;   // device inputs 2-3
            mic.mic.numInputs = 2;
            mic.fadeOutMs = 0;
            expect (mic.effectiveLength() < 0.0);
            expect (engine.play (mic));
            expect (engine.isPlaying (mic.id));

            // device input block: channel 1 = 0.25, channel 2 = -0.5, channel 0 = 1.0 (not used)
            juce::AudioBuffer<float> inputs (3, blockSize);
            inputs.clear();
            juce::FloatVectorOperations::fill (inputs.getWritePointer (0), 1.0f, blockSize);
            juce::FloatVectorOperations::fill (inputs.getWritePointer (1), 0.25f, blockSize);
            juce::FloatVectorOperations::fill (inputs.getWritePointer (2), -0.5f, blockSize);

            for (int i = 0; i < 20; ++i)   // through the de-click / level ramps
                engine.renderBlock (out, blockSize, inputs.getArrayOfReadPointers(), 3);

            expectWithinAbsoluteError (out.getSample (0, blockSize - 1), 0.25f, 0.01f);   // row 1 -> output 1
            expectWithinAbsoluteError (out.getSample (1, blockSize - 1), -0.5f, 0.01f);   // row 2 -> output 2
            expect (engine.getPlayingCues().front().lengthSeconds < 0.0);                // endless

            // without an input block the mic renders silence, and it keeps running
            engine.renderBlock (out, blockSize);
            expectWithinAbsoluteError (out.getSample (0, blockSize - 1), 0.0f, 0.001f);
            expect (engine.isPlaying (mic.id));

            engine.stop (mic.id);

            for (int i = 0; i < 5; ++i)
            {
                engine.renderBlock (out, blockSize);
                engine.reapFinishedPlayers();
            }

            expect (! engine.isPlaying (mic.id));
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
