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
        const auto tone1 = writeSine (dir, "tone1.wav", 1.0);

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
            const auto bWaits = controller.getRunningWaits();   // kept: 'bPre' points into it
            const auto* bPre = find (bWaits, b.id, WaitProgress::Kind::preWait);
            expect (bPre != nullptr);
            controller.cancelWait (b.id, WaitProgress::Kind::preWait, bPre != nullptr ? bPre->startId : 0);
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
            const auto aWaits = controller.getRunningWaits();   // kept: 'aPre' points into it
            const auto* aPre = find (aWaits, a.id, WaitProgress::Kind::preWait);
            expect (aPre != nullptr);
            controller.cancelWait (a.id, WaitProgress::Kind::preWait, aPre != nullptr ? aPre->startId : 0);
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
            const auto* aPost = find (waits, a.id, WaitProgress::Kind::postWait);
            expect (aPost != nullptr);
            controller.cancelWait (b.id, WaitProgress::Kind::postWait, aPost != nullptr ? aPost->startId : 0);   // a's post-wait card: only the start behind a goes
            expect (controller.isCueActive (b.id), "the post-wait card's x stopped b's own wait");
            expect (find (controller.getRunningWaits(), a.id, WaitProgress::Kind::postWait) == nullptr);
            expect (find (controller.getRunningWaits(), b.id, WaitProgress::Kind::waitCue) != nullptr);
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.update (1, [] (Cue& x) { x.type = CueType::audio; x.control = {}; });
        }

        beginTest ("a cue that starts as scheduled and fade-stops the others keeps its own follow");
        {
            // a (auto-continue, post-wait 0.3) -> b (auto-follow -> c, fade-stop-others all) -> c: b's start fires at 0.3 s and fades a;
            // the start behind a that just fired is b's own run now - its follow must survive the chain cancel
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.3; });
            document.cues.update (1, [] (Cue& x) { x.continueMode = ContinueMode::autoFollow; x.fadeStopOthers.enabled = true; x.fadeStopOthers.seconds = 0.05; x.fadeStopOthers.scope = FadeStopScope::all; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 35);   // 0.41 s: b started as scheduled, a fades
            expect (engine.isPlaying (b.id));
            render (engine, scheduler, now, out, 200);  // 2.7 s: b (2 s) is over, its follow starts c
            expect (engine.isPlaying (c.id), "b's follow died with the chain behind the faded cue");
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.update (1, [] (Cue& x) { x.continueMode = ContinueMode::none; x.fadeStopOthers = {}; });
        }

        beginTest ("fade-stop-others ending a wait cue takes the chain behind the wait cue too");
        {
            // a as a 2 s wait cue (auto-continue, post-wait 0.5) -> b; s starts b with fade-stop-others all at 0.2 s: a's wait ends,
            // and the start of b put on behind it (due at 2.5 s) must not start b a second time
            document.cues.update (0, [] (Cue& x) { x.type = CueType::control; x.control.kind = ControlKind::wait; x.control.seconds = 2.0; x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.5; });
            Cue s;
            s.name = "s2"; s.type = CueType::control; s.control.kind = ControlKind::start; s.control.targetId = b.id;
            s.fadeStopOthers.enabled = true; s.fadeStopOthers.seconds = 0.05; s.fadeStopOthers.scope = FadeStopScope::all;
            const int si = document.cues.add (s);
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 17);   // 0.2 s
            controller.fireSequence (si);
            expect (engine.isPlaying (b.id));
            expect (! controller.isCueActive (a.id), "the wait cue was not ended by fade-stop-others");
            auto startOrderOfB = [&engine, &b]
            {
                juce::int64 order = -1;
                for (const auto& p : engine.getPlayingCues())
                    if (p.id == b.id && ! p.loaded)
                        order = p.startOrder;
                return order;
            };
            const auto firstStart = startOrderOfB();
            render (engine, scheduler, now, out, 60);   // 0.9 s: the start of b put on behind a (due at 0.5 s: a's post-wait counts from a's start) must not restart it
            expect (engine.isPlaying (b.id));
            expectEquals (startOrderOfB(), firstStart, "b was started again by the start behind the ended wait cue");
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.type = CueType::audio; x.control = {}; x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.removeIndices ({ document.cues.indexOf (s.id) });
        }

        beginTest ("a post-wait card cancels the one start it leads to, not another run's start of the same cue");
        {
            // a (auto-continue, post-wait 1.0) -> b (memo, auto-continue, post-wait 0.3) -> c (pre-wait 0.2): a GO on a puts b@1.0 and c@1.5 on;
            // a GO on b at 0.1 s runs b at once and puts c@0.6 on. Cancelling b's post-wait card (the c@0.6 start) must leave c@1.5.
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 1.0; });
            document.cues.update (1, [] (Cue& x) { x.type = CueType::control; x.control.kind = ControlKind::memo; x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.3; });
            document.cues.update (2, [] (Cue& x) { x.preWaitSeconds = 0.2; });
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 9);    // 0.1 s
            document.cues.setPlayheadIndex (1);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            const auto waits = controller.getRunningWaits();
            const auto* post = find (waits, b.id, WaitProgress::Kind::postWait);
            expect (post != nullptr && post->startsCueId == c.id, "b's own post-wait is not reported");
            controller.cancelWait (b.id, WaitProgress::Kind::postWait, post != nullptr ? post->startId : 0);
            render (engine, scheduler, now, out, 50);   // 0.68 s: the c@0.6 start must not have fired
            expect (! engine.isPlaying (c.id), "the start behind b's own run fired");
            render (engine, scheduler, now, out, 80);   // 1.6 s: c@1.5 (a's run) is untouched
            expect (engine.isPlaying (c.id), "the other run's start of c was cancelled too");
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.update (1, [] (Cue& x) { x.type = CueType::audio; x.control = {}; x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
            document.cues.update (2, [] (Cue& x) { x.preWaitSeconds = 0.0; });
        }

        beginTest ("cancelling a wait cue's card inside a running playlist stops the playlist");
        {
            Cue g;
            g.name = "PL"; g.type = CueType::group; g.group.mode = GroupMode::playlist;
            const int gi = document.cues.add (g);
            Cue w, x;
            w.name = "pw"; w.type = CueType::control; w.control.kind = ControlKind::wait; w.control.seconds = 1.0; w.parentId = g.id;
            x.name = "px"; x.file = tone; x.parentId = g.id;
            document.cues.add (w);
            document.cues.add (x);
            controller.fireSequence (gi);
            expect (controller.isCueActive (w.id));
            controller.cancelWait (w.id, WaitProgress::Kind::waitCue, 0);
            expect (! controller.isCueActive (g.id), "the playlist is still active after its wait cue was cancelled");
            render (engine, scheduler, now, out, 120);   // 1.4 s
            expect (! engine.isPlaying (x.id), "the playlist went on after its wait cue was cancelled");
            stopEverything();
            document.cues.removeIndices ({ document.cues.indexOf (g.id) });
        }

        beginTest ("a restart pre-wait of a playlist's running child is cancelled alone (the child plays on, the playlist stays)");
        {
            Cue g;
            g.name = "PL2"; g.type = CueType::group; g.group.mode = GroupMode::playlist;
            const int gi = document.cues.add (g);
            Cue h, l;
            h.name = "H"; h.type = CueType::group; h.parentId = g.id; h.preWaitSeconds = 0.3;
            l.name = "L"; l.file = tone; l.parentId = h.id;
            document.cues.add (h);
            document.cues.add (l);
            controller.fireSequence (gi);               // h is due at 0.3 s
            render (engine, scheduler, now, out, 35);   // 0.41 s: l plays inside h
            expect (engine.isPlaying (l.id));
            controller.fireSequence (document.cues.indexOf (h.id));   // a GO on h while it runs: a restart after its pre-wait
            const auto hWaits = controller.getRunningWaits();   // kept: 'pre' points into it
            const auto* pre = find (hWaits, h.id, WaitProgress::Kind::preWait);
            expect (pre != nullptr, "the restart's pre-wait is not reported");
            controller.cancelWait (h.id, WaitProgress::Kind::preWait, pre != nullptr ? pre->startId : 0);
            expect (engine.isPlaying (l.id), "cancelling the restart stopped the running child");
            expect (controller.isCueActive (g.id), "cancelling the restart stopped the playlist");
            expect (find (controller.getRunningWaits(), h.id, WaitProgress::Kind::preWait) == nullptr);
            stopEverything();
            document.cues.removeIndices ({ document.cues.indexOf (g.id) });
        }

        beginTest ("cancelling the pre-wait of a playlist's child that has not started stops the playlist");
        {
            Cue g;
            g.name = "PL3"; g.type = CueType::group; g.group.mode = GroupMode::playlist;
            const int gi = document.cues.add (g);
            Cue h, l, nb;
            h.name = "H3"; h.type = CueType::group; h.parentId = g.id; h.preWaitSeconds = 0.5;
            l.name = "L3"; l.file = tone; l.parentId = h.id;
            nb.name = "B3"; nb.file = tone; nb.parentId = g.id;
            document.cues.add (h);
            document.cues.add (l);
            document.cues.add (nb);
            controller.fireSequence (gi);               // h is due at 0.5 s: the list's own start of it
            const auto hWaits = controller.getRunningWaits();
            const auto* pre = find (hWaits, h.id, WaitProgress::Kind::preWait);
            expect (pre != nullptr);
            controller.cancelWait (h.id, WaitProgress::Kind::preWait, pre != nullptr ? pre->startId : 0);
            expect (! controller.isCueActive (g.id), "the playlist is still active after its child's first pre-wait was cancelled");
            render (engine, scheduler, now, out, 100);  // 1.16 s
            expect (! engine.isPlaying (l.id) && ! engine.isPlaying (nb.id), "the playlist went on after its child's pre-wait was cancelled");
            stopEverything();
            document.cues.removeIndices ({ document.cues.indexOf (g.id) });
        }

        beginTest ("cancelling another run's pre-wait of a playlist's current child leaves the playlist's own start");
        {
            // G: playlist [x (1 s), q (memo, auto-continue), w (pre-wait 2 s)]. A GO on q at 0.2 s puts w@2.2 on (a run of its own);
            // once x is over the list itself steps through q and puts w@3.0 on. Cancelling the w@2.2 card must leave w@3.0.
            Cue g;
            g.name = "PL4"; g.type = CueType::group; g.group.mode = GroupMode::playlist;
            const int gi = document.cues.add (g);
            Cue x, q, w;
            x.name = "X4"; x.file = tone1; x.parentId = g.id;
            q.name = "Q4"; q.type = CueType::control; q.control.kind = ControlKind::memo; q.continueMode = ContinueMode::autoContinue; q.parentId = g.id;
            w.name = "W4"; w.file = tone; w.preWaitSeconds = 2.0; w.parentId = g.id;
            document.cues.add (x);
            document.cues.add (q);
            document.cues.add (w);
            controller.fireSequence (gi);               // x plays (1 s)
            render (engine, scheduler, now, out, 17);   // 0.2 s
            controller.fireSequence (document.cues.indexOf (q.id));   // q at once, w@2.2 behind it
            render (engine, scheduler, now, out, 86);   // 1.2 s: x is over, the list stepped through q and put w@3.0 on
            const auto wWaits = controller.getRunningWaits();
            const WaitProgress* separate = nullptr;
            const WaitProgress* own = nullptr;
            for (const auto& wp : wWaits)
                if (wp.cueId == w.id && wp.kind == WaitProgress::Kind::preWait && (separate == nullptr || wp.endsAt < separate->endsAt))
                    separate = &wp;
            for (const auto& wp : wWaits)
                if (wp.cueId == w.id && wp.kind == WaitProgress::Kind::preWait && &wp != separate)
                    own = &wp;
            expect (separate != nullptr && own != nullptr, "the two starts of w are not both reported");
            controller.cancelWait (w.id, WaitProgress::Kind::preWait, separate != nullptr ? separate->startId : 0);
            expect (controller.isCueActive (g.id), "cancelling the other run's start stopped the playlist");
            expect (! engine.isPlaying (w.id));
            render (engine, scheduler, now, out, 160);  // 3.06 s: the list's own w@3.0 fired
            expect (engine.isPlaying (w.id), "the playlist's own start of w was cancelled too");
            stopEverything();
            document.cues.removeIndices ({ document.cues.indexOf (g.id) });
        }

        beginTest ("a playlist re-entered by its own control child keeps the id of the start it put on next");
        {
            // G: playlist [x (1 s), s (start control -> G), w (pre-wait 0.5), nb]: after x, s runs at once and advances G to w (re-entrant);
            // the outer step for s must not overwrite the id of w's start with its own (0). Cancelling w's card then stops the list.
            Cue g;
            g.name = "PL5"; g.type = CueType::group; g.group.mode = GroupMode::playlist;
            const int gi = document.cues.add (g);
            Cue x, s, w, nb;
            x.name = "X5"; x.file = tone1; x.parentId = g.id;
            s.name = "S5"; s.type = CueType::control; s.control.kind = ControlKind::start; s.control.targetId = g.id; s.parentId = g.id;
            w.name = "W5"; w.file = tone; w.preWaitSeconds = 0.5; w.parentId = g.id;
            nb.name = "B5"; nb.file = tone; nb.parentId = g.id;
            document.cues.add (x);
            document.cues.add (s);
            document.cues.add (w);
            document.cues.add (nb);
            controller.fireSequence (gi);               // x plays (1 s)
            render (engine, scheduler, now, out, 100);  // 1.16 s: x over, s ran and moved the list on to w (due at ~1.5 s)
            const auto wWaits = controller.getRunningWaits();
            const auto* pre = find (wWaits, w.id, WaitProgress::Kind::preWait);
            expect (pre != nullptr, "w's pre-wait is not reported after the re-entrant step");
            controller.cancelWait (w.id, WaitProgress::Kind::preWait, pre != nullptr ? pre->startId : 0);
            expect (! controller.isCueActive (g.id), "the list's own start of w was not recognised (its id was overwritten by the re-entrant step)");
            render (engine, scheduler, now, out, 100);  // 2.3 s
            expect (! engine.isPlaying (w.id) && ! engine.isPlaying (nb.id), "the list went on after its child's pre-wait was cancelled");
            stopEverything();
            document.cues.removeIndices ({ document.cues.indexOf (g.id) });
        }

        beginTest ("a stale pre-wait id (the start fired since the screen was drawn) does nothing - least of all stop the playlist");
        {
            Cue g;
            g.name = "PL6"; g.type = CueType::group; g.group.mode = GroupMode::playlist;
            const int gi = document.cues.add (g);
            Cue w, nb;
            w.name = "W6"; w.file = tone; w.preWaitSeconds = 0.3; w.parentId = g.id;
            nb.name = "B6"; nb.file = tone; nb.parentId = g.id;
            document.cues.add (w);
            document.cues.add (nb);
            controller.fireSequence (gi);               // w due at 0.3 s
            const auto wWaits = controller.getRunningWaits();
            const auto* pre = find (wWaits, w.id, WaitProgress::Kind::preWait);
            expect (pre != nullptr);
            const int staleId = pre != nullptr ? pre->startId : 0;
            render (engine, scheduler, now, out, 35);   // 0.41 s: w started
            expect (engine.isPlaying (w.id));
            controller.cancelWait (w.id, WaitProgress::Kind::preWait, staleId);   // the card the screen still showed
            expect (engine.isPlaying (w.id), "a stale pre-wait id stopped the cue that had just started");
            expect (controller.isCueActive (g.id), "a stale pre-wait id stopped the playlist");
            stopEverything();
            document.cues.removeIndices ({ document.cues.indexOf (g.id) });
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
