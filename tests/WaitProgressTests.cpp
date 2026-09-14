#include "app/CueController.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** CueController::getRunningWaits(): what the UI counts down while pre-waits, post-waits and wait cues run. */
class WaitProgressTests : public juce::UnitTest
{
public:
    WaitProgressTests() : juce::UnitTest ("WaitProgress", "Enqueue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;

    juce::File writeSine (const juce::File& dir, const juce::String& fileName, double seconds)
    {
        const auto file = dir.getChildFile (fileName);
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

    /** Renders 'blocks' blocks (11.6 ms each), advancing the fake clock and ticking the scheduler after each. */
    static void render (AudioEngine& engine, Scheduler& scheduler, double& now, juce::AudioBuffer<float>& out, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            engine.renderBlock (out, blockSize);
            now += blockSize / sampleRate;
            engine.reapFinishedPlayers();
            scheduler.tick();
        }
    }

    static const WaitProgress* find (const std::vector<WaitProgress>& waits, const juce::Uuid& id, WaitProgress::Kind kind)
    {
        for (const auto& w : waits)
            if (w.cueId == id && w.kind == kind)
                return &w;

        return nullptr;
    }

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gocue_waits_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const auto tone = writeSine (dir, "tone.wav", 2.0);

        AudioEngine engine (0);
        engine.prepare (sampleRate, blockSize);
        juce::AudioBuffer<float> out (2, blockSize);

        ProjectDocument document;
        document.clock = [] { return 0.0; };
        Cue a, b, c;
        a.name = "a"; a.file = tone;
        b.name = "b"; b.file = tone;
        c.name = "c"; c.file = tone;
        document.cues.add (a);
        document.cues.add (b);
        document.cues.add (c);

        double now = 10.0;   // not 0: the waits must carry the clock's absolute times
        Scheduler scheduler ([&now] { return now; });
        CueController controller (engine, document, scheduler);

        auto stopEverything = [&]
        {
            controller.hardStopAll();   // no panic latch: the next test starts at once
            render (engine, scheduler, now, out, 2);
        };

        beginTest ("a pre-wait is reported from the GO until the start fires");
        {
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.5; });
            document.cues.setPlayheadIndex (0);
            const double t0 = now;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (! engine.isPlaying (a.id));

            auto waits = controller.getRunningWaits();
            expectEquals ((int) waits.size(), 1);
            const auto* w = find (waits, a.id, WaitProgress::Kind::preWait);
            expect (w != nullptr, "the pre-wait is not reported");

            if (w != nullptr)
            {
                expectWithinAbsoluteError (w->startedAt, t0, 1.0e-9);
                expectWithinAbsoluteError (w->endsAt, t0 + 0.5, 1.0e-9);
                expectWithinAbsoluteError (w->total(), 0.5, 1.0e-9);
                expectWithinAbsoluteError (w->remaining (t0 + 0.2), 0.3, 1.0e-9);
                expectWithinAbsoluteError (w->fraction (t0 + 0.25), 0.5, 1.0e-9);
                expectWithinAbsoluteError (w->fraction (t0 + 9.0), 1.0, 1.0e-9);   // clamped
                expectWithinAbsoluteError (w->remaining (t0 + 9.0), 0.0, 1.0e-9);
                expect (! w->audition);
            }

            render (engine, scheduler, now, out, 20);   // 0.23 s in: still waiting
            expect (find (controller.getRunningWaits(), a.id, WaitProgress::Kind::preWait) != nullptr);
            expect (! engine.isPlaying (a.id));
            render (engine, scheduler, now, out, 30);   // 0.58 s: fired
            expect (engine.isPlaying (a.id));
            expect (controller.getRunningWaits().empty(), "a fired start still reports a wait");
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.0; });
        }

        beginTest ("an auto-continue chain: the first cue's post-wait, then the second cue's pre-wait, never both");
        {
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.4; });
            document.cues.update (1, [] (Cue& x) { x.preWaitSeconds = 0.3; });
            document.cues.setPlayheadIndex (0);
            const double t0 = now;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (a.id));

            auto waits = controller.getRunningWaits();
            expectEquals ((int) waits.size(), 1);
            const auto* post = find (waits, a.id, WaitProgress::Kind::postWait);
            expect (post != nullptr, "a's post-wait is not reported");

            if (post != nullptr)
            {
                expectWithinAbsoluteError (post->startedAt, t0, 1.0e-9);
                expectWithinAbsoluteError (post->endsAt, t0 + 0.4, 1.0e-9);
            }

            expect (find (waits, b.id, WaitProgress::Kind::preWait) == nullptr, "b's pre-wait shows during a's post-wait");

            render (engine, scheduler, now, out, 40);   // 0.46 s: a's post-wait is over, b's pre-wait runs
            waits = controller.getRunningWaits();
            expectEquals ((int) waits.size(), 1);
            const auto* pre = find (waits, b.id, WaitProgress::Kind::preWait);
            expect (pre != nullptr, "b's pre-wait is not reported after a's post-wait");

            if (pre != nullptr)
            {
                expectWithinAbsoluteError (pre->startedAt, t0 + 0.4, 1.0e-9);
                expectWithinAbsoluteError (pre->endsAt, t0 + 0.7, 1.0e-9);
            }

            expect (find (waits, a.id, WaitProgress::Kind::postWait) == nullptr, "a's post-wait outlives itself");

            render (engine, scheduler, now, out, 25);   // 0.75 s: b started
            expect (engine.isPlaying (b.id));
            expect (controller.getRunningWaits().empty());
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.update (1, [] (Cue& x) { x.preWaitSeconds = 0.0; });
        }

        beginTest ("zero waits show nothing: an auto-continue with no post-wait into a cue with no pre-wait");
        {
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoContinue; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (a.id) && engine.isPlaying (b.id));
            expect (controller.getRunningWaits().empty());
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; });
        }

        beginTest ("a stop of the cue and Esc take its waits away");
        {
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 1.0; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expectEquals ((int) controller.getRunningWaits().size(), 1);
            controller.stopCue (a.id);
            expect (controller.getRunningWaits().empty(), "a stopped cue still reports its pre-wait");

            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expectEquals ((int) controller.getRunningWaits().size(), 1);
            controller.panicAll();
            expect (controller.getRunningWaits().empty(), "Esc left a pre-wait behind");
            stopEverything();   // clears the panic latch as well
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.0; });
        }

        beginTest ("a wait cue reports its own wait");
        {
            Cue w;
            w.name = "W"; w.type = CueType::control; w.control.kind = ControlKind::wait; w.control.seconds = 0.5;
            const int wi = document.cues.add (w);
            const double t0 = now;
            controller.fireSequence (wi);

            const auto waits = controller.getRunningWaits();
            expectEquals ((int) waits.size(), 1);
            const auto* run = find (waits, w.id, WaitProgress::Kind::waitCue);
            expect (run != nullptr, "the wait cue's wait is not reported");

            if (run != nullptr)
            {
                expectWithinAbsoluteError (run->startedAt, t0, 1.0e-9);
                expectWithinAbsoluteError (run->endsAt, t0 + 0.5, 1.0e-9);
            }

            render (engine, scheduler, now, out, 50);   // 0.58 s: over
            expect (controller.getRunningWaits().empty());
            stopEverything();
            document.cues.removeIndices ({ document.cues.indexOf (w.id) });
        }

        beginTest ("a restart during the pre-wait reports one wait with the new times");
        {
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.5; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 10);   // 0.12 s in
            const double t1 = now;
            controller.fireSequence (0);                // hardStopRestart: the pre-wait starts over

            const auto waits = controller.getRunningWaits();
            expectEquals ((int) waits.size(), 1);
            const auto* w = find (waits, a.id, WaitProgress::Kind::preWait);
            expect (w != nullptr);

            if (w != nullptr)
            {
                expectWithinAbsoluteError (w->startedAt, t1, 1.0e-9);
                expectWithinAbsoluteError (w->endsAt, t1 + 0.5, 1.0e-9);
            }

            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.0; });
        }

        beginTest ("stopping a cue takes the auto-continue chain put on behind it (its waits and starts)");
        {
            // a (auto-continue, post-wait 0.4) -> b (pre-wait 0.3, auto-continue, post-wait 0.2) -> c: one GO puts b's and c's starts on
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.4; });
            document.cues.update (1, [] (Cue& x) { x.preWaitSeconds = 0.3; x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.2; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (a.id));
            expectEquals (controller.getNumPending(), 2);
            controller.stopCue (a.id);
            expect (controller.getRunningWaits().empty(), "a stopped cue's post-wait still shows");
            expectEquals (controller.getNumPending(), 0);
            render (engine, scheduler, now, out, 100);   // 1.16 s: nothing behind a may start
            expect (! engine.isPlaying (b.id) && ! engine.isPlaying (c.id), "the chain behind a stopped cue started");
            stopEverything();

            // the same chain, cancelled at b's pre-wait (the waiting card's x): a plays on, c goes with b
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 40);   // 0.46 s: b's pre-wait runs
            expect (find (controller.getRunningWaits(), b.id, WaitProgress::Kind::preWait) != nullptr);
            controller.cancelWait (b.id, WaitProgress::Kind::preWait);
            expect (engine.isPlaying (a.id), "cancelling b's wait stopped a");
            expect (controller.getRunningWaits().empty());
            expectEquals (controller.getNumPending(), 0);
            render (engine, scheduler, now, out, 80);   // 1.4 s
            expect (! engine.isPlaying (b.id) && ! engine.isPlaying (c.id), "a cancelled pre-wait's chain started");
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.update (1, [] (Cue& x) { x.preWaitSeconds = 0.0; x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
        }

        beginTest ("a restart pending while the cue plays: cancelScheduledStart drops it and the sound goes on");
        {
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.3; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 40);   // 0.46 s: playing
            expect (engine.isPlaying (a.id));
            controller.fireSequence (0);                 // hardStopRestart: a new start in 0.3 s, the instance plays on until then
            expect (find (controller.getRunningWaits(), a.id, WaitProgress::Kind::preWait) != nullptr, "the pending restart is not reported");
            controller.cancelScheduledStart (a.id);
            expect (controller.getRunningWaits().empty());
            expect (engine.isPlaying (a.id), "cancelling the restart stopped the sound");
            render (engine, scheduler, now, out, 40);   // 0.46 s later: no restart
            expect (engine.isPlaying (a.id));
            expectEquals (controller.getNumPending(), 0);
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.0; });
        }

        beginTest ("cancelling a pre-wait takes the follow of that start with it (an auto-follow's next cue must not start)");
        {
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.5; x.continueMode = ContinueMode::autoFollow; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (controller.getNumPending() >= 2);   // a's start and its follow
            controller.cancelWait (a.id, WaitProgress::Kind::preWait);
            expect (controller.getRunningWaits().empty());
            expectEquals (controller.getNumPending(), 0);
            render (engine, scheduler, now, out, 90);   // 1.04 s
            expect (! engine.isPlaying (a.id) && ! engine.isPlaying (b.id), "the follow of a cancelled start fired");
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.0; x.continueMode = ContinueMode::none; });
        }

        beginTest ("stopping a cue whose fired start was swept still takes the chain behind that start");
        {
            // a (pre-wait 0.3, auto-continue, post-wait 0.8) -> b; once a plays, an unrelated scheduled start sweeps a's fired entry
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.3; x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.8; });
            document.cues.update (2, [] (Cue& x) { x.preWaitSeconds = 5.0; });   // c: a start far away, put on only to sweep
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 35);   // 0.41 s: a plays, b is due at 1.1 s
            expect (engine.isPlaying (a.id));
            controller.fireSequence (2);                // c's pre-wait goes on: track() sweeps a's fired start
            controller.stopCue (a.id);
            expect (find (controller.getRunningWaits(), a.id, WaitProgress::Kind::postWait) == nullptr, "a stopped cue's post-wait still shows");
            render (engine, scheduler, now, out, 80);   // 1.34 s
            expect (! engine.isPlaying (b.id), "the chain behind a stopped cue started (its start entry had been swept)");
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.preWaitSeconds = 0.0; x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.update (2, [] (Cue& x) { x.preWaitSeconds = 0.0; });
        }

        beginTest ("fade-stop-others takes the chain behind the cue it fades, even a spared cue's start put on behind it");
        {
            // a (auto-continue, post-wait 0.6) -> b: b's start is due at 0.6 s. s is a start control cue aimed at b with fade-stop-others:
            // at 0.2 s it starts b itself and fades a; the start of b that hung off a must not fire on top of that
            Cue s;
            s.name = "s"; s.type = CueType::control; s.control.kind = ControlKind::start; s.control.targetId = b.id;
            s.fadeStopOthers.enabled = true; s.fadeStopOthers.seconds = 0.05; s.fadeStopOthers.scope = FadeStopScope::all;
            const int si = document.cues.add (s);
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.6; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 17);   // 0.2 s
            expect (find (controller.getRunningWaits(), a.id, WaitProgress::Kind::postWait) != nullptr);
            controller.fireSequence (si);
            expect (engine.isPlaying (b.id), "the start control cue did not start b");
            expect (find (controller.getRunningWaits(), a.id, WaitProgress::Kind::postWait) == nullptr, "a faded cue's post-wait still shows");
            render (engine, scheduler, now, out, 60);   // 0.9 s: a is gone, b runs once
            expect (! engine.isPlaying (a.id));
            int instancesOfB = 0;
            for (const auto& p : engine.getPlayingCues())
                if (p.id == b.id && ! p.loaded)
                    ++instancesOfB;
            expectEquals (instancesOfB, 1);   // the start behind a would have restarted b at 0.6 s
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.removeIndices ({ document.cues.indexOf (s.id) });
        }

        beginTest ("cancelling a post-wait card leaves a run of the next cue that was started on its own (a wait cue's wait)");
        {
            // a (auto-continue, post-wait 1.0) -> b as a 2 s wait cue; b is also started on its own at 0.2 s
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 1.0; });
            document.cues.update (1, [] (Cue& x) { x.type = CueType::control; x.control.kind = ControlKind::wait; x.control.seconds = 2.0; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 17);   // 0.2 s
            controller.fireSequence (1);                // b's own wait, 0.2 .. 2.2
            expect (controller.isCueActive (b.id));
            const auto waits = controller.getRunningWaits();
            expect (find (waits, b.id, WaitProgress::Kind::waitCue) != nullptr);
            expect (find (waits, a.id, WaitProgress::Kind::postWait) != nullptr);
            controller.cancelWait (b.id, WaitProgress::Kind::postWait);   // a's post-wait card: only the start behind a goes
            expect (controller.isCueActive (b.id), "the post-wait card's x stopped b's own wait");
            expect (find (controller.getRunningWaits(), a.id, WaitProgress::Kind::postWait) == nullptr);
            expect (find (controller.getRunningWaits(), b.id, WaitProgress::Kind::waitCue) != nullptr);
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.update (1, [] (Cue& x) { x.type = CueType::audio; x.control = {}; });
        }

        beginTest ("timeline group: every child's pre-wait counts from the group's start");
        {
            Cue g;
            g.name = "G";
            g.type = CueType::group;
            const int gi = document.cues.add (g);
            Cue x, y;
            x.name = "x"; x.file = tone; x.parentId = g.id; x.preWaitSeconds = 0.2;
            y.name = "y"; y.file = tone; y.parentId = g.id; y.preWaitSeconds = 0.4;
            document.cues.add (x);
            document.cues.add (y);
            const double t0 = now;
            controller.fireSequence (gi);

            const auto waits = controller.getRunningWaits();
            expectEquals ((int) waits.size(), 2);
            const auto* wx = find (waits, x.id, WaitProgress::Kind::preWait);
            const auto* wy = find (waits, y.id, WaitProgress::Kind::preWait);
            expect (wx != nullptr && wy != nullptr, "a child's pre-wait is not reported");

            if (wx != nullptr && wy != nullptr)
            {
                expectWithinAbsoluteError (wx->startedAt, t0, 1.0e-9);
                expectWithinAbsoluteError (wx->endsAt, t0 + 0.2, 1.0e-9);
                expectWithinAbsoluteError (wy->startedAt, t0, 1.0e-9);
                expectWithinAbsoluteError (wy->endsAt, t0 + 0.4, 1.0e-9);
            }

            controller.stopGroup (g.id, 0);
            render (engine, scheduler, now, out, 2);
            expect (controller.getRunningWaits().empty(), "a stopped group still reports its children's pre-waits");
            stopEverything();
        }

        dir.deleteRecursively();
    }
};

static WaitProgressTests waitProgressTests;

} // namespace gocue::tests
