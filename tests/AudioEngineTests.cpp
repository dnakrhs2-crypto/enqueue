#include "audio/AudioEngine.h"
#include "TestGainPlugin.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** Drives the engine without an audio device: prepare() + renderBlock() in a loop. */
class AudioEngineTests : public juce::UnitTest
{
public:
    AudioEngineTests() : juce::UnitTest ("AudioEngine (offline)", "Enqueue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeSine (const juce::File& dir, const juce::String& fileName, double seconds, float amplitude, int channels)
    {
        const auto file = dir.getChildFile (fileName);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr);

        if (stream == nullptr)
            return {};

        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (sampleRate)
                                                       .withNumChannels (channels)
                                                       .withBitsPerSample (16));
        expect (writer != nullptr);

        if (writer == nullptr)
            return {};

        const int numSamples = (int) (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (channels, numSamples);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));

        expect (writer->writeFromAudioSampleBuffer (buffer, 0, numSamples));
        return file;
    }

    static void render (AudioEngine& engine, juce::AudioBuffer<float>& out, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            engine.renderBlock (out, blockSize);
    }

    static float rms (const juce::AudioBuffer<float>& buffer, int channel = 0)
    {
        return buffer.getRMSLevel (channel, 0, buffer.getNumSamples());
    }

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("gocue_engine_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());

        const auto tone = writeSine (dir, "tone.wav", 1.0, 0.5f, 2);
        const auto mono = writeSine (dir, "mono.wav", 1.0, 0.5f, 1);
        const auto quad = writeSine (dir, "quad.wav", 1.0, 0.5f, 4);

        AudioEngine engine (0);            // synchronous reads: deterministic offline rendering
        engine.prepare (sampleRate, blockSize);
        juce::AudioBuffer<float> out (2, blockSize);

        beginTest ("fade-in and cue gain are applied");
        {
            Cue cue;
            cue.name = "tone";
            cue.file = tone;
            cue.audio.envelope = Envelope::fromFadeIn (0.1);
            cue.fadeOutMs = 50;
            cue.gainDb = -6.0206;           // x0.5

            juce::String error;
            expect (engine.play (cue, &error), error);
            expect (engine.isPlaying (cue.id));
            expectEquals (engine.getNumPlaying(), 1);

            engine.renderBlock (out, blockSize);
            expectLessThan (rms (out), 0.06f);                       // still ramping up

            render (engine, out, 15);                                // 8192 samples: past the 4410-sample fade
            expectWithinAbsoluteError (rms (out, 0), 0.1768f, 0.01f); // 0.5 amp * 0.5 gain / sqrt(2)
            expectWithinAbsoluteError (rms (out, 1), 0.1768f, 0.01f);

            const auto playing = engine.getPlayingCues();
            expectEquals ((int) playing.size(), 1);
            expect (playing[0].id == cue.id);
            expectWithinAbsoluteError (playing[0].lengthSeconds, 1.0, 1e-3);
            expectGreaterThan (playing[0].positionSeconds, 0.15);
            expect (! playing[0].fadingOut);

            beginTest ("fade-out stop reaches silence and the player is reaped");
            engine.fadeOutAndStop (cue.id);
            engine.renderBlock (out, blockSize);
            expect (engine.getPlayingCues()[0].fadingOut);

            render (engine, out, 6);                                 // 50 ms == 2205 samples < 7 blocks
            expectWithinAbsoluteError (rms (out), 0.0f, 1e-4f);
            engine.reapFinishedPlayers();
            expect (! engine.isPlaying (cue.id));
            expectEquals (engine.getNumPlaying(), 0);
        }

        beginTest ("a cue finishes by itself at the end of the file");
        {
            Cue plain;
            plain.file = tone;
            expect (engine.play (plain));

            render (engine, out, 50);                                // 25600 samples
            engine.reapFinishedPlayers();
            expect (engine.isPlaying (plain.id));

            render (engine, out, 40);                                // 46080 samples > 44100
            engine.reapFinishedPlayers();
            expect (! engine.isPlaying (plain.id));
            expectEquals (engine.getNumPlaying(), 0);
        }

        beginTest ("mono files are played on both output channels");
        {
            Cue m;
            m.file = mono;
            expect (engine.play (m));
            render (engine, out, 5);
            expectWithinAbsoluteError (rms (out, 0), 0.3536f, 0.01f);
            expectWithinAbsoluteError (rms (out, 1), 0.3536f, 0.01f);

            engine.stopAll();
            render (engine, out, 3);
            engine.reapFinishedPlayers();
            expectEquals (engine.getNumPlaying(), 0);
        }

        beginTest ("level matrix: crosspoints, input / output levels and trim shape the output");
        {
            const float full = 0.3536f;   // rms of the 0.5 sine
            Cue c;
            c.file = tone;
            c.levels.resize (2, 2);
            c.levels.crosspointDb[0][0] = -6.0;                     // L -> out 1 at -6 dB
            c.levels.crosspointDb[1][1] = LevelMatrix::silentDb;   // R -> out 2 silent
            expect (engine.play (c));
            render (engine, out, 5);
            expectWithinAbsoluteError (rms (out, 0), full * 0.501f, 0.01f);
            expectWithinAbsoluteError (rms (out, 1), 0.0f, 1e-4f);
            engine.stopAll();
            render (engine, out, 3);
            engine.reapFinishedPlayers();

            Cue d;
            d.file = tone;
            d.levels.resize (2, 2);
            d.levels.inputDb[1] = LevelMatrix::silentDb;   // silent input row
            d.levels.outputDb[0] = 6.0;                    // hot output column
            d.trim.resize (2);
            d.trim.mainDb = -6.0;                          // trim cancels the output boost
            expect (engine.play (d));
            render (engine, out, 5);
            expectWithinAbsoluteError (rms (out, 0), full, 0.01f);
            expectWithinAbsoluteError (rms (out, 1), 0.0f, 1e-4f);
            engine.stopAll();
            render (engine, out, 3);
            engine.reapFinishedPlayers();
        }

        beginTest ("live level changes ramp to the new matrix while the cue plays");
        {
            Cue c;
            c.file = tone;
            expect (engine.play (c));
            render (engine, out, 3);
            expectWithinAbsoluteError (rms (out, 0), 0.3536f, 0.01f);

            LevelMatrix m;
            m.resize (2, 2);
            m.crosspointDb[0][0] = LevelMatrix::silentDb;
            m.outputDb[1] = -6.0;
            engine.setLiveLevels (c.id, m, TrimLevels());
            render (engine, out, 3);    // ~35 ms: past the 10 ms ramp
            expectWithinAbsoluteError (rms (out, 0), 0.0f, 1e-4f);
            expectWithinAbsoluteError (rms (out, 1), 0.3536f * 0.501f, 0.01f);
            engine.stopAll();
            render (engine, out, 3);
            engine.reapFinishedPlayers();
        }

        beginTest ("a four-channel file: channels 3-4 are read and routed by the matrix");
        {
            Cue c;
            c.file = quad;
            expect (engine.play (c));                  // default: ch1 -> out1, ch2 -> out2, ch3/4 silent
            render (engine, out, 5);
            expectWithinAbsoluteError (rms (out, 0), 0.3536f, 0.01f);
            expectWithinAbsoluteError (rms (out, 1), 0.3536f, 0.01f);

            LevelMatrix m;
            m.resize (4, 2);
            m.crosspointDb[2][0] = 0.0;                // ch3 also to out1: the identical sines add up
            m.crosspointDb[3][1] = 0.0;                // ch4 also to out2
            engine.setLiveLevels (c.id, m, TrimLevels());
            render (engine, out, 3);
            expectWithinAbsoluteError (rms (out, 0), 0.7071f, 0.01f);
            expectWithinAbsoluteError (rms (out, 1), 0.7071f, 0.01f);
            engine.stopAll();
            render (engine, out, 3);
            engine.reapFinishedPlayers();
            expectEquals (engine.getNumPlaying(), 0);
        }

        beginTest ("patch: cue outputs route to device outputs 3-4 with the patch main level");
        {
            AudioEngine engine4 (0);
            engine4.prepare (sampleRate, blockSize, 4);
            juce::AudioBuffer<float> out4 (4, blockSize);

            auto patch = AudioPatch::makeDefault ("Main");
            patch.numCueOutputs = 2;
            patch.sanitise();
            patch.setRouting (0, 0, LevelMatrix::silentDb);
            patch.setRouting (1, 1, LevelMatrix::silentDb);
            patch.setRouting (0, 2, 0.0);
            patch.setRouting (1, 3, 0.0);
            patch.mainDb = -6.0;
            expect (engine4.setPatches ({ patch }).isEmpty());
            expect (engine4.findPatch (patch.id) != nullptr);

            Cue c;
            c.file = tone;
            expect (engine4.play (c));
            render (engine4, out4, 5);
            expectWithinAbsoluteError (rms (out4, 0), 0.0f, 1e-4f);
            expectWithinAbsoluteError (rms (out4, 1), 0.0f, 1e-4f);
            expectWithinAbsoluteError (rms (out4, 2), 0.3536f * 0.501f, 0.01f);
            expectWithinAbsoluteError (rms (out4, 3), 0.3536f * 0.501f, 0.01f);

            // live routing change: cue output 1 back to device output 1
            patch.setRouting (0, 2, LevelMatrix::silentDb);
            patch.setRouting (0, 0, 0.0);
            engine4.updatePatchLevels (patch);
            render (engine4, out4, 3);
            expectWithinAbsoluteError (rms (out4, 0), 0.3536f * 0.501f, 0.01f);
            expectWithinAbsoluteError (rms (out4, 2), 0.0f, 1e-4f);

            // a stereo-pair cue output insert halves outputs 1-2; a mono device output insert quarters device output 4
            patch.cueOutputStereoWithNext[0] = 1;
            engine4.updatePatchLevels (patch);
            engine4.getPatchCueOutputChain (patch.id, 0).addPlugin (std::make_unique<TestGainPlugin> (0.5f));
            engine4.getPatchDeviceOutputChain (patch.id, 3).addPlugin (std::make_unique<TestGainPlugin> (0.25f));
            render (engine4, out4, 3);
            expectWithinAbsoluteError (rms (out4, 0), 0.3536f * 0.501f * 0.5f, 0.01f);
            expectWithinAbsoluteError (rms (out4, 3), 0.3536f * 0.501f * 0.5f * 0.25f, 0.005f);

            AudioPatch captured = patch;
            engine4.capturePatchInsertStates (captured);
            expectEquals ((int) captured.cueOutputInserts[0].size(), 1);
            expectEquals ((int) captured.deviceOutputInserts.size(), 4);
            expectEquals ((int) captured.deviceOutputInserts[3].size(), 1);

            engine4.stopAll();
            render (engine4, out4, 3);
            engine4.reapFinishedPlayers();

            // an unknown patch id plays through the default (first) patch
            Cue d;
            d.file = tone;
            d.patchId = juce::Uuid();
            expect (engine4.play (d));
            render (engine4, out4, 5);
            expectWithinAbsoluteError (rms (out4, 0), 0.3536f * 0.501f * 0.5f, 0.01f);

            // replacing the patch list moves the running player to the new default patch (diagonal, unity)
            auto other = AudioPatch::makeDefault ("Other");
            other.numCueOutputs = 2;
            other.sanitise();
            expect (engine4.setPatches ({ other }).isEmpty());
            render (engine4, out4, 3);
            expectWithinAbsoluteError (rms (out4, 0), 0.3536f, 0.01f);
            expectWithinAbsoluteError (rms (out4, 1), 0.3536f, 0.01f);
            expectWithinAbsoluteError (rms (out4, 3), 0.0f, 1e-4f);
            expect (engine4.findPatch (patch.id) == nullptr);

            engine4.stopAll();
            render (engine4, out4, 3);
            engine4.reapFinishedPlayers();
            expectEquals (engine4.getNumPlaying(), 0);
            engine4.shutdown();
        }

        beginTest ("cues mix together and stopAll silences everything");
        {
            Cue a, b;
            a.file = tone;
            b.file = tone;
            expect (engine.play (a));
            expect (engine.play (b));
            expectEquals (engine.getNumPlaying(), 2);

            render (engine, out, 5);
            expectWithinAbsoluteError (rms (out), 0.7071f, 0.02f);   // two in-phase 0.5 sines

            engine.stopAll();
            render (engine, out, 3);                                 // 5 ms de-click < 1 block
            expectWithinAbsoluteError (rms (out), 0.0f, 1e-4f);
            engine.reapFinishedPlayers();
            expectEquals (engine.getNumPlaying(), 0);

            beginTest ("re-firing a running cue restarts it as a single instance");
            expect (engine.play (a));
            render (engine, out, 10);
            expect (engine.play (a));
            render (engine, out, 3);
            engine.reapFinishedPlayers();

            const auto playing = engine.getPlayingCues();
            expectEquals ((int) playing.size(), 1);
            expect (playing[0].id == a.id);
            expectLessThan (playing[0].positionSeconds, 0.1);

            beginTest ("the most recently started cue is reported, optionally skipping fading cues");
            b.fadeOutMs = 500;                                       // long enough to observe the fade
            expect (engine.play (b));
            expect (engine.getMostRecentlyStartedCue (false) == b.id);
            engine.fadeOutAndStop (b.id);
            engine.renderBlock (out, blockSize);
            expect (engine.getMostRecentlyStartedCue (false) == b.id);
            expect (engine.getMostRecentlyStartedCue (true) == a.id);

            engine.stopAll();
            render (engine, out, 3);
            engine.reapFinishedPlayers();
            expectEquals (engine.getNumPlaying(), 0);
            expect (engine.getMostRecentlyStartedCue (false).isNull());
        }

        beginTest ("a duck ramp is linear and lands on its target after the requested time");
        {
            Cue c;
            c.file = tone;
            expect (engine.play (c));
            render (engine, out, 3);
            engine.setDuckDb (c.id, -20.0, 0.4);                     // to 0.1 over 0.4 s (the file is 1 s long)
            const int blocksPerSecond = (int) (sampleRate / blockSize);
            render (engine, out, blocksPerSecond / 5);               // ~0.2 s: half way
            expectWithinAbsoluteError (rms (out, 0), 0.3536f * 0.55f, 0.03f);   // linear in gain: 1.0 -> 0.1 is 0.55 at the middle
            render (engine, out, blocksPerSecond / 5 + 2);           // ~0.42 s: landed
            expectWithinAbsoluteError (rms (out, 0), 0.3536f * 0.1f, 0.005f);
            engine.stopAll();
            render (engine, out, 3);
            engine.reapFinishedPlayers();
        }

        beginTest ("missing or unassigned files are rejected with a message");
        {
            Cue missing;
            missing.name = "missing";
            missing.file = dir.getChildFile ("nope.wav");
            juce::String message;
            expect (! engine.play (missing, &message));
            expect (message.contains ("nope.wav"));

            Cue unassigned;
            unassigned.name = "empty";
            message.clear();
            expect (! engine.play (unassigned, &message));
            expect (message.isNotEmpty());
            expectEquals (engine.getNumPlaying(), 0);
        }

        engine.shutdown();
        expect (dir.deleteRecursively());
    }
};

static AudioEngineTests audioEngineTests;

} // namespace gocue::tests
