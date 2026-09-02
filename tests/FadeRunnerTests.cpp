#include "app/FadeRunner.h"
#include "app/CueController.h"
#include "model/ProjectSerializer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

class FadeRunnerTests : public juce::UnitTest
{
public:
    FadeRunnerTests() : juce::UnitTest ("FadeRunner", "GoCue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeSine (const juce::File& dir, const juce::String& fileName, double seconds)
    {
        const auto file = dir.getChildFile (fileName);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
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

    static float rms (const juce::AudioBuffer<float>& b, int ch) { return b.getRMSLevel (ch, 0, b.getNumSamples()); }
    static float dbToGain (double db) { return juce::Decibels::decibelsToGain ((float) db); }

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gocue_fade_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const auto tone = writeSine (dir, "tone.wav", 20.0);

        AudioEngine engine (0);
        engine.prepare (sampleRate, blockSize);
        juce::AudioBuffer<float> out (2, blockSize);

        ProjectDocument document;
        document.clock = [] { return 0.0; };
        Cue a;
        a.name = "a";
        a.file = tone;
        document.cues.add (a);

        double now = 0.0;
        FadeRunner fadeRunner (engine, document);
        fadeRunner.clock = [&now] { return now; };

        /** renders 'seconds' of audio, ticking the runner every block */
        auto run = [&] (double seconds)
        {
            const int blocks = (int) std::ceil (seconds * sampleRate / blockSize);

            for (int i = 0; i < blocks; ++i)
            {
                engine.renderBlock (out, blockSize);
                now += blockSize / sampleRate;
                engine.reapFinishedPlayers();
                fadeRunner.tick();
            }
        };

        auto makeFade = [&] (double seconds)
        {
            Cue f;
            f.type = CueType::fade;
            f.name = "fade";
            f.fade.targetId = a.id;
            f.fade.durationSeconds = seconds;
            f.fade.curve.shape = CurveShape::linear;
            f.fade.curve.domain = AudioDomain::decibel;
            f.fade.levels.resize (2, 2);
            f.fade.resizeActive (2, 2);
            return f;
        };

        beginTest ("a fade needs a running target and a target id");
        {
            auto f = makeFade (1.0);
            juce::String error;
            expect (! fadeRunner.start (f, &error));
            expect (error.isNotEmpty());
            f.fade.targetId = juce::Uuid::null();
            expect (! fadeRunner.start (f, &error));
            expectEquals (fadeRunner.getNumRunning(), 0);
        }

        beginTest ("absolute main-level fade follows the curve and finishes");
        {
            expect (engine.play (a));
            run (0.1);
            expectWithinAbsoluteError (rms (out, 0), 0.3536f, 0.01f);

            auto f = makeFade (2.0);
            f.fade.mainDb = -20.0;
            f.fade.mainActive = true;
            expect (fadeRunner.start (f));
            expect (fadeRunner.isRunning (f.id));
            const double t0 = now;
            run (1.0);
            AudioEngine::LiveState live;
            expect (engine.getLiveState (a.id, live));
            expectWithinAbsoluteError (live.gainDb, -20.0 * ((now - t0) / 2.0), 0.3);   // linear in dB
            run (1.1);
            expect (! fadeRunner.isRunning (f.id));
            expect (engine.getLiveState (a.id, live));
            expectWithinAbsoluteError (live.gainDb, -20.0, 1e-6);
            run (0.1);
            expectWithinAbsoluteError (rms (out, 0), 0.3536f * dbToGain (-20.0), 0.005f);
            expect (engine.isPlaying (a.id));   // no stop-when-done

            beginTest ("revert puts the target back to its pre-fade level");
            expect (fadeRunner.canRevert());
            expect (fadeRunner.revertLast());
            expect (engine.getLiveState (a.id, live));
            expectWithinAbsoluteError (live.gainDb, 0.0, 1e-6);
            run (0.1);
            expectWithinAbsoluteError (rms (out, 0), 0.3536f, 0.01f);
        }

        beginTest ("relative fade offsets the current level; inactive cells are untouched");
        {
            auto f = makeFade (1.0);
            f.fade.relative = true;
            f.fade.mainDb = -6.0;
            f.fade.mainActive = true;
            f.fade.levels.crosspointDb[0][0] = -30.0;   // not active: must not move
            f.fade.setCrosspointActive (0, 0, false);
            expect (fadeRunner.start (f));
            run (1.2);
            AudioEngine::LiveState live;
            expect (engine.getLiveState (a.id, live));
            expectWithinAbsoluteError (live.gainDb, -6.0, 1e-6);
            expectWithinAbsoluteError (live.levels.crosspointDb[0][0], 0.0, 1e-9);
            expect (fadeRunner.revertLast());
        }

        beginTest ("crosspoint fade on one cell only: left goes silent, right stays; two fades on different cells run together");
        {
            auto f1 = makeFade (1.0);
            f1.fade.mainActive = false;
            f1.fade.levels.crosspointDb[0][0] = LevelMatrix::silentDb;
            f1.fade.setCrosspointActive (0, 0, true);

            auto f2 = makeFade (1.0);
            f2.fade.mainActive = false;
            f2.fade.levels.outputDb[1] = -12.0;
            f2.fade.setOutputActive (1, true);

            expect (fadeRunner.start (f1));
            expect (fadeRunner.start (f2));
            expectEquals (fadeRunner.getNumRunning(), 2);
            run (1.2);
            expectEquals (fadeRunner.getNumRunning(), 0);
            run (0.1);
            expectWithinAbsoluteError (rms (out, 0), 0.0f, 1e-4f);
            expectWithinAbsoluteError (rms (out, 1), 0.3536f * dbToGain (-12.0), 0.005f);
            expect (fadeRunner.revertLast());   // f2's state (recorded after f1 started, so left is still at 0 dB there)
            expect (fadeRunner.revertLast());
            run (0.1);
            expectWithinAbsoluteError (rms (out, 0), 0.3536f, 0.01f);
            expectWithinAbsoluteError (rms (out, 1), 0.3536f, 0.01f);
        }

        beginTest ("rate fade is geometric and stop-when-done stops the target");
        {
            auto f = makeFade (1.0);
            f.fade.fadeLevels = false;
            f.fade.fadeRate = true;
            f.fade.rate = 2.0;
            f.fade.stopTargetWhenDone = true;
            expect (fadeRunner.start (f));
            run (0.5);
            AudioEngine::LiveState live;
            expect (engine.getLiveState (a.id, live));
            expectWithinAbsoluteError (live.rate, std::sqrt (2.0), 0.05);   // half way in log space
            run (0.7);
            expect (! fadeRunner.isRunning (f.id));
            run (0.1);
            expect (! engine.isPlaying (a.id));   // stopped when the fade finished
        }

        beginTest ("stopAll / stop end fades where they are; a fade cue round-trips through the project file");
        {
            expect (engine.play (a));
            run (0.1);
            auto f = makeFade (5.0);
            f.fade.mainDb = -40.0;
            expect (fadeRunner.start (f));
            run (1.0);
            fadeRunner.stop (f.id);
            expectEquals (fadeRunner.getNumRunning(), 0);
            AudioEngine::LiveState live;
            expect (engine.getLiveState (a.id, live));
            expect (live.gainDb < -4.0 && live.gainDb > -12.0);   // left where it was
            fadeRunner.stopAll();
            engine.stopAll();
            run (0.1);

            Project p;
            f.fade.curve.shape = CurveShape::custom;
            f.fade.curve.points = { { 0.0, 0.0 }, { 0.4, 0.8 }, { 1.0, 1.0 } };
            f.fade.relative = true;
            f.fade.stopTargetWhenDone = true;
            f.fade.setInputActive (1, true);
            f.fade.setCrosspointActive (1, 0, true);
            f.fade.fadeRate = true;
            f.fade.rate = 0.5;
            ParamFade pf;
            pf.slot = 1;
            pf.parameter = 3;
            pf.value = 0.25f;
            f.fade.params.push_back (pf);
            p.cues = { a, f };
            Project q;
            expect (ProjectSerializer::fromJson (ProjectSerializer::toJson (p), q, nullptr).wasOk());
            expectEquals ((int) q.cues.size(), 2);
            expect (q.cues[0].isAudio());
            expect (q.cues[1].isFade());
            expect (q.cues[1].fade.targetId == a.id);
            expectWithinAbsoluteError (q.cues[1].fade.durationSeconds, 5.0, 1e-12);
            expectWithinAbsoluteError (q.cues[1].fade.mainDb, -40.0, 1e-12);
            expect (q.cues[1].fade.relative && q.cues[1].fade.stopTargetWhenDone && q.cues[1].fade.fadeRate);
            expectWithinAbsoluteError (q.cues[1].fade.rate, 0.5, 1e-12);
            expect (q.cues[1].fade.isInputActive (1) && ! q.cues[1].fade.isInputActive (0));
            expect (q.cues[1].fade.isCrosspointActive (1, 0) && ! q.cues[1].fade.isCrosspointActive (0, 0));
            expect (q.cues[1].fade.curve.shape == CurveShape::custom);
            expectEquals ((int) q.cues[1].fade.curve.points.size(), 3);
            expectEquals ((int) q.cues[1].fade.params.size(), 1);
            expectEquals (q.cues[1].fade.params[0].parameter, 3);
            expectWithinAbsoluteError (q.cues[1].effectiveLength(), 5.0, 1e-12);
        }

        beginTest ("the controller fires a fade cue, counts it as active for auto-follow, and Esc cancels it");
        {
            ProjectDocument doc;
            doc.clock = [] { return 0.0; };
            Cue target = a;
            Cue fade = makeFade (1.0);
            fade.fade.mainDb = -20.0;
            fade.continueMode = ContinueMode::autoFollow;
            Cue after;
            after.name = "after";
            after.file = tone;
            doc.cues.add (target);
            doc.cues.add (fade);
            doc.cues.add (after);

            Scheduler scheduler ([&now] { return now; });
            CueController controller (engine, doc, scheduler);
            controller.getFadeRunner().clock = [&now] { return now; };
            juce::StringArray statuses;
            controller.onStatus = [&statuses] (const juce::String& m, bool) { statuses.add (m); };

            auto step = [&] (double seconds)
            {
                const int blocks = (int) std::ceil (seconds * sampleRate / blockSize);

                for (int i = 0; i < blocks; ++i)
                {
                    engine.renderBlock (out, blockSize);
                    now += blockSize / sampleRate;
                    engine.reapFinishedPlayers();
                    scheduler.tick();
                    controller.getFadeRunner().tick();
                }
            };

            doc.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);   // target
            controller.goKeyReleased();
            step (0.1);
            expect (controller.go() == CueController::GoResult::started);   // fade + auto-follow
            controller.goKeyReleased();
            expect (controller.isCueActive (fade.id));
            expect (! engine.isPlaying (after.id));
            step (1.2);
            expect (! controller.isCueActive (fade.id));
            expect (engine.isPlaying (after.id));                            // followed when the fade finished
            AudioEngine::LiveState live;
            expect (engine.getLiveState (target.id, live));
            expectWithinAbsoluteError (live.gainDb, -20.0, 1e-6);

            // a fade on a target that is not playing fails with a message
            engine.stopAll();
            step (0.1);
            expect (controller.trigger (fade) == CueController::GoResult::failed);
            expect (statuses[statuses.size() - 1].isNotEmpty());

            // Esc cancels a running fade
            expect (engine.play (target));
            step (0.1);
            expect (controller.trigger (fade) == CueController::GoResult::started);
            controller.panicAll();
            expect (! controller.isCueActive (fade.id));
            controller.hardStopAll();
            step (0.1);
        }

        engine.shutdown();
        expect (dir.deleteRecursively());
    }
};

static FadeRunnerTests fadeRunnerTests;

} // namespace gocue::tests
