#include "app/CueController.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

class CueControllerTests : public juce::UnitTest
{
public:
    CueControllerTests() : juce::UnitTest ("CueController", "Enqueue") {}

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

    /** Renders 'blocks' blocks, advancing the fake clock and ticking the scheduler after each. */
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

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gocue_ctl_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const auto tone = writeSine (dir, "tone.wav", 2.0);

        AudioEngine engine (0);
        engine.prepare (sampleRate, blockSize);
        juce::AudioBuffer<float> out (2, blockSize);

        ProjectDocument document;
        document.clock = [] { return 0.0; };
        Cue a, b;
        a.name = "a"; a.file = tone;
        b.name = "b"; b.file = tone;
        document.cues.add (a);
        document.cues.add (b);
        document.cues.setSelectedIndex (0);

        double now = 0.0;
        Scheduler scheduler ([&now] { return now; });
        CueController controller (engine, document, scheduler);
        int rejected = 0;
        controller.onGoRejected = [&rejected] { ++rejected; };
        juce::StringArray statuses;
        controller.onStatus = [&statuses] (const juce::String& message, bool) { statuses.add (message); };

        auto stopEverything = [&]
        {
            controller.hardStopAll();
            render (engine, scheduler, now, out, 2);
            expectEquals (engine.getNumPlaying(), 0);
        };

        beginTest ("GO fires the playhead cue and moves the playhead on");
        {
            expect (controller.go() == CueController::GoResult::started);
            expect (engine.isPlaying (a.id));
            expectEquals (document.cues.getPlayheadIndex(), 1);
            expectEquals (document.cues.getSelectedIndex(), 1);   // locked to the playhead
            expect (statuses[statuses.size() - 1].startsWith ("GO"));
            controller.goKeyReleased();
            stopEverything();
        }

        beginTest ("double-GO protection refuses a GO inside the window");
        {
            auto settings = document.settings;
            settings.doubleGoSeconds = 0.5;
            document.setSettings (settings);
            document.cues.setSelectedIndex (0);

            now = 10.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (controller.isGoLocked());

            now = 10.3;
            expect (controller.go() == CueController::GoResult::rejectedDoubleGo);
            expect (! engine.isPlaying (b.id));
            expectEquals (rejected, 1);
            expectEquals (document.cues.getPlayheadIndex(), 1);

            now = 10.6;
            expect (! controller.isGoLocked());
            expect (controller.go() == CueController::GoResult::started);
            expect (engine.isPlaying (b.id));
            controller.goKeyReleased();

            settings.doubleGoSeconds = 0.0;
            document.setSettings (settings);
            stopEverything();
        }

        beginTest ("require key up blocks a repeated GO until the key is released");
        {
            auto settings = document.settings;
            settings.requireKeyUp = true;
            document.setSettings (settings);
            document.cues.setSelectedIndex (0);
            now = 20.0;

            expect (controller.go() == CueController::GoResult::started);
            now = 21.0;
            expect (controller.go() == CueController::GoResult::rejectedKeyUp);
            expect (! engine.isPlaying (b.id));
            controller.goKeyReleased();
            expect (controller.go() == CueController::GoResult::started);
            expect (engine.isPlaying (b.id));
            controller.goKeyReleased();

            settings.requireKeyUp = false;
            document.setSettings (settings);
            stopEverything();
        }

        beginTest ("P pauses the target and Space resumes it instead of firing the next cue");
        {
            document.cues.setSelectedIndex (0);
            now = 30.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 4);

            expect (controller.togglePause());
            render (engine, scheduler, now, out, 2);
            expect (engine.isPaused (a.id));

            now = 31.0;
            expect (controller.go() == CueController::GoResult::resumed);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 2);
            expect (! engine.isPaused (a.id));
            expect (engine.isPlaying (a.id));
            expect (! engine.isPlaying (b.id));
            expectEquals (document.cues.getPlayheadIndex(), 1);   // unchanged by the resume

            expect (controller.togglePause());
            render (engine, scheduler, now, out, 2);
            expect (engine.isPaused (a.id));
            expect (controller.togglePause());
            render (engine, scheduler, now, out, 2);
            expect (! engine.isPaused (a.id));

            stopEverything();
            expect (! controller.togglePause());
        }

        beginTest ("Esc fades everything over the panic time and a second Esc stops at once");
        {
            auto settings = document.settings;
            settings.panicSeconds = 1.0;
            document.setSettings (settings);
            document.cues.setSelectedIndex (0);
            now = 40.0;
            controller.go();
            controller.goKeyReleased();
            now = 40.1;
            controller.go();
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 2);
            expectEquals (engine.getNumPlaying(), 2);

            now = 41.0;
            controller.panicAll();
            render (engine, scheduler, now, out, 2);
            expect (engine.getPlayingCues()[0].fadingOut);
            expectEquals (engine.getNumPlaying(), 2);

            // a GO during the panic fade is refused: nothing may outlive the panic
            expect (controller.isPanicLatched());
            controller.go();
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 2);
            expectEquals (engine.getNumPlaying(), 2);   // still just the two fading cues
            expect (! controller.handleHotkey (juce::KeyPress::createFromDescription ("F5")) || engine.getNumPlaying() == 2);

            now = 41.2;
            controller.panicAll();
            render (engine, scheduler, now, out, 2);
            expectEquals (engine.getNumPlaying(), 0);

            settings.panicSeconds = 2.0;
            document.setSettings (settings);
        }

        beginTest ("second-trigger rules: ignore, restart, devamp, hard stop");
        {
            Cue loop;
            loop.name = "loop";
            loop.file = tone;
            loop.audio.endSeconds = 0.25;
            loop.audio.infiniteLoop = true;
            loop.secondTrigger = SecondTriggerAction::nothing;
            const int index = document.cues.add (loop);
            document.cues.setSelectedIndex (index);
            now += 0.2;   // past the short latch a hard stop leaves behind (the 5 ms gate close, with margin)

            expect (controller.preview() == CueController::GoResult::started);
            render (engine, scheduler, now, out, 10);
            const double before = engine.getPlayingCues()[0].positionSeconds;
            expect (controller.preview() == CueController::GoResult::ignored);
            render (engine, scheduler, now, out, 1);
            expectGreaterThan (engine.getPlayingCues()[0].positionSeconds, before);

            document.cues.update (index, [] (Cue& c) { c.secondTrigger = SecondTriggerAction::hardStopRestart; });
            expect (controller.preview() == CueController::GoResult::started);
            render (engine, scheduler, now, out, 1);
            expectLessThan (engine.getPlayingCues()[0].positionSeconds, 0.05);

            document.cues.update (index, [] (Cue& c) { c.secondTrigger = SecondTriggerAction::devamp; });
            render (engine, scheduler, now, out, 30);
            expect (controller.preview() == CueController::GoResult::ignored);
            expectGreaterThan (engine.getPlayingCues()[0].lengthSeconds, 0.0);

            for (int i = 0; i < 60 && engine.isPlaying (loop.id); ++i)
                render (engine, scheduler, now, out, 1);

            expect (! engine.isPlaying (loop.id));

            document.cues.update (index, [] (Cue& c) { c.secondTrigger = SecondTriggerAction::hardStop; });
            controller.preview();
            render (engine, scheduler, now, out, 2);
            expect (controller.preview() == CueController::GoResult::ignored);
            render (engine, scheduler, now, out, 2);
            expect (! engine.isPlaying (loop.id));

            document.cues.remove (index);
        }

        beginTest ("a pre-wait delays the start and Esc cancels it");
        {
            document.cues.update (0, [] (Cue& c) { c.preWaitSeconds = 0.5; });
            document.cues.setSelectedIndex (0);
            now = 50.0;

            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (! engine.isPlaying (a.id));
            expectEquals (controller.getNumPending(), 1);
            expectEquals (document.cues.getPlayheadIndex(), 1);

            now = 50.4;
            scheduler.tick();
            expect (! engine.isPlaying (a.id));
            now = 50.5;
            scheduler.tick();
            expect (engine.isPlaying (a.id));
            expectEquals (controller.getNumPending(), 0);   // the start ran: nothing is pending any more
            stopEverything();

            document.cues.setSelectedIndex (0);
            now = 60.0;
            controller.go();
            controller.goKeyReleased();
            controller.panicAll();
            expectEquals (controller.getNumPending(), 0);
            now = 61.0;
            scheduler.tick();
            expect (! engine.isPlaying (a.id));

            document.cues.update (0, [] (Cue& c) { c.preWaitSeconds = 0.0; });
        }

        beginTest ("auto-continue starts the next cue after the post-wait and the playhead skips the sequence");
        {
            Cue c;
            c.name = "c";
            c.file = tone;
            document.cues.add (c);   // a, b, c
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.3; });
            document.cues.setSelectedIndex (0);
            now = 70.0;

            expectEquals (controller.sequenceEnd (0), 2);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (a.id));
            expect (! engine.isPlaying (b.id));
            expectEquals (document.cues.getPlayheadIndex(), 2);

            now = 70.29;
            scheduler.tick();
            expect (! engine.isPlaying (b.id));
            now = 70.3;
            scheduler.tick();
            expect (engine.isPlaying (b.id));
            expect (! engine.isPlaying (c.id));
            stopEverything();

            // zero post-wait: both start in the same GO
            document.cues.update (0, [] (Cue& x) { x.postWaitSeconds = 0.0; });
            document.cues.setSelectedIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (a.id) && engine.isPlaying (b.id));
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; });
        }

        beginTest ("auto-follow starts the next cue when the first one finishes");
        {
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::autoFollow; x.audio.endSeconds = 0.2; });
            document.cues.setSelectedIndex (0);
            now = 80.0;

            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (a.id));
            expect (! engine.isPlaying (b.id));
            expectEquals (document.cues.getPlayheadIndex(), 2);

            render (engine, scheduler, now, out, 10);              // 0.116 s: still playing
            expect (! engine.isPlaying (b.id));

            for (int i = 0; i < 40 && ! engine.isPlaying (b.id); ++i)
                render (engine, scheduler, now, out, 1);

            expect (! engine.isPlaying (a.id));
            expect (engine.isPlaying (b.id));
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.continueMode = ContinueMode::none; x.audio.endSeconds = -1.0; });
        }

        beginTest ("a disabled cue is passed over by GO, whatever the old skip flag says");
        {
            document.cues.update (0, [] (Cue& x) { x.armed = false; x.skipIfDisarmed = true; });
            document.cues.setSelectedIndex (0);
            now = 90.0;
            expect (controller.preview (false) == CueController::GoResult::failed);   // a preview / cart button: refused too
            expect (! engine.isPlaying (a.id));
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (! engine.isPlaying (a.id));
            expect (engine.isPlaying (b.id));
            expectEquals (document.cues.getPlayheadIndex(), 2);
            stopEverything();

            // the older "silent but continuing" flag no longer matters: one 비활성화 switch, one behaviour
            document.cues.update (0, [] (Cue& x) { x.skipIfDisarmed = false; x.continueMode = ContinueMode::autoContinue; x.postWaitSeconds = 0.2; });
            document.cues.setSelectedIndex (0);
            now = 91.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (! engine.isPlaying (a.id));
            expect (engine.isPlaying (b.id));
            now = 91.3;
            scheduler.tick();
            expect (! engine.isPlaying (a.id));
            stopEverything();
            document.cues.update (0, [] (Cue& x) { x.armed = true; x.skipIfDisarmed = true; x.continueMode = ContinueMode::none; x.postWaitSeconds = 0.0; });
        }

        beginTest ("fade-stop-others fades the running cues when the cue starts");
        {
            document.cues.setSelectedIndex (0);
            controller.preview();                                    // a runs
            render (engine, scheduler, now, out, 2);
            document.cues.update (1, [] (Cue& x) { x.fadeStopOthers.enabled = true; x.fadeStopOthers.seconds = 0.1; });
            document.cues.setSelectedIndex (1);
            now = 100.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 1);

            bool aFading = false;
            for (const auto& p : engine.getPlayingCues())
                if (p.id == a.id)
                    aFading = p.fadingOut;

            expect (aFading);
            expect (engine.isPlaying (b.id));
            render (engine, scheduler, now, out, 12);              // 0.14 s > 0.1 s fade
            expect (! engine.isPlaying (a.id));
            expect (engine.isPlaying (b.id));
            stopEverything();
            document.cues.update (1, [] (Cue& x) { x.fadeStopOthers.enabled = false; });
        }

        beginTest ("ducking lowers the other cues while the cue runs and restores them afterwards");
        {
            document.cues.setSelectedIndex (0);
            controller.preview();                                    // a runs (2 s)
            render (engine, scheduler, now, out, 2);
            document.cues.update (1, [] (Cue& x) { x.duck.enabled = true; x.duck.levelDb = -20.0; x.duck.seconds = 0.01; x.audio.endSeconds = 0.1; });
            document.cues.setSelectedIndex (1);
            now = 110.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expectWithinAbsoluteError (engine.getDuckDb (a.id), -20.0, 1e-9);

            for (int i = 0; i < 40 && engine.isPlaying (b.id); ++i)
                render (engine, scheduler, now, out, 1);

            render (engine, scheduler, now, out, 1);
            expect (! engine.isPlaying (b.id));
            expect (engine.isPlaying (a.id));
            expectWithinAbsoluteError (engine.getDuckDb (a.id), 0.0, 1e-9);   // restored
            stopEverything();
            document.cues.update (1, [] (Cue& x) { x.duck.enabled = false; x.audio.endSeconds = -1.0; });
        }

        beginTest ("hotkeys fire cues (with their sequence) without moving the playhead");
        {
            document.cues.update (1, [] (Cue& x) { x.hotkey = "F5"; });
            document.cues.setSelectedIndex (0);
            now = 120.0;

            expect (controller.handleHotkey (juce::KeyPress::createFromDescription ("F5")));
            expect (engine.isPlaying (b.id));
            expect (! engine.isPlaying (a.id));
            expectEquals (document.cues.getPlayheadIndex(), 0);
            expect (! controller.handleHotkey (juce::KeyPress::createFromDescription ("F6")));

            // two cues on one key (a live edit race; the loader clears duplicates): one key fires one cue
            document.cues.update (0, [] (Cue& x) { x.hotkey = "F5"; });
            const int hotkeyStatusesBefore = statuses.size();
            expect (controller.handleHotkey (juce::KeyPress::createFromDescription ("F5")));
            int hotkeyFires = 0;

            for (int i = hotkeyStatusesBefore; i < statuses.size(); ++i)
                if (statuses[i].startsWith (juce::String::fromUTF8 ("\xED\x95\xAB\xED\x82\xA4")))   // "핫키"
                    ++hotkeyFires;

            expectEquals (hotkeyFires, 1);
            document.cues.update (0, [] (Cue& x) { x.hotkey = ""; });
            stopEverything();
            document.cues.update (1, [] (Cue& x) { x.hotkey = ""; });
        }

        beginTest ("wall-clock triggers fire once for the matching second and day");
        {
            document.cues.update (0, [] (Cue& x)
            {
                x.wallClock.enabled = true;
                x.wallClock.hour = 19;
                x.wallClock.minute = 30;
                x.wallClock.second = 5;
                x.wallClock.daysMask = 0x7f;
            });

            const juce::Time match (2026, 8, 2, 19, 30, 5, 250, true);    // 2026-09-02 19:30:05.250 (a Wednesday)
            const int before = statuses.size();
            controller.checkWallClock (match);
            expect (engine.isPlaying (a.id));
            controller.checkWallClock (match + juce::RelativeTime::milliseconds (400));   // same second: no second fire
            controller.checkWallClock (juce::Time (2026, 8, 2, 19, 30, 6, 0, true));    // next second: no match
            int fired = 0;

            for (int i = before; i < statuses.size(); ++i)
                if (statuses[i].contains (juce::String::fromUTF8 ("\xEC\x8B\x9C\xEA\xB0\x84 \xED\x8A\xB8\xEB\xA6\xAC\xEA\xB1\xB0")))   // "시간 트리거"
                    ++fired;

            expectEquals (fired, 1);
            stopEverything();

            document.cues.update (0, [] (Cue& x) { x.wallClock.daysMask = 1 << 1; });   // Mondays only
            controller.checkWallClock (juce::Time (2026, 8, 9, 19, 30, 5, 0, true));    // Wednesday again: no
            expect (! engine.isPlaying (a.id));
            document.cues.update (0, [] (Cue& x) { x.wallClock.enabled = false; });
        }

        beginTest ("load prepares a cue silently and the next GO starts it from the loaded position");
        {
            document.cues.setSelectedIndex (0);
            now = 130.0;
            expect (controller.loadSelected (0.3));
            expect (engine.isLoaded (a.id));
            expect (! engine.isPlaying (a.id));
            expectEquals (engine.getNumPlaying(), 0);

            const auto entries = engine.getPlayingCues();
            expectEquals ((int) entries.size(), 1);
            expect (entries[0].loaded);
            expectWithinAbsoluteError (entries[0].positionSeconds, 0.3, 1e-6);

            render (engine, scheduler, now, out, 3);
            expectWithinAbsoluteError (out.getRMSLevel (0, 0, blockSize), 0.0f, 1e-6f);   // silent while loaded
            expect (engine.isLoaded (a.id));

            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (a.id));
            expect (! engine.isLoaded (a.id));
            render (engine, scheduler, now, out, 1);
            expectGreaterThan (out.getRMSLevel (0, 0, blockSize), 0.3f);
            expectWithinAbsoluteError (engine.getPlayingCues()[0].positionSeconds, 0.3 + blockSize / sampleRate, 1e-3);
            stopEverything();

            document.cues.setSelectedIndex (0);      // the GO above moved the playhead (and selection) to b
            expect (controller.loadSelected (0.0));
            expect (engine.isLoaded (a.id));
            engine.unload (a.id);
            render (engine, scheduler, now, out, 2);
            expect (! engine.isLoaded (a.id));
            expectEquals ((int) engine.getPlayingCues().size(), 0);
        }

        beginTest ("audition: no output while the cue runs, an alternate patch, and a normal GO restarts it for real");
        {
            document.cues.setPlayheadIndex (0);
            document.cues.setSelectedIndex (0);
            auto s = document.settings;
            s.audition = WorkspaceSettings::Audition::none;
            document.setSettings (s);

            expect (controller.go (true) == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (a.id));
            expect (engine.isAuditioning (a.id));
            render (engine, scheduler, now, out, 4);
            expectWithinAbsoluteError (out.getRMSLevel (0, 0, blockSize), 0.0f, 1e-6f);   // silent audition
            expect (statuses[statuses.size() - 1].startsWith (juce::String::fromUTF8 ("\xEC\x98\xA4\xEB\x94\x94\xEC\x85\x98")));   // 오디션

            // a normal GO on the auditioning cue restarts it with real output (the second-trigger rule is skipped)
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 4);
            expect (! engine.isAuditioning (a.id));
            expectWithinAbsoluteError (out.getRMSLevel (0, 0, blockSize), 0.3536f, 0.02f);
            stopEverything();

            // alternate patch: cue output 1 -> device output 2 only
            auto alt = AudioPatch::makeDefault ("Alt");
            alt.numCueOutputs = 2;
            alt.sanitise();
            alt.setRouting (0, 0, LevelMatrix::silentDb);
            alt.setRouting (1, 1, LevelMatrix::silentDb);
            alt.setRouting (0, 1, 0.0);
            auto patches = document.patches;
            patches.push_back (alt);
            document.setPatches (patches);
            engine.setPatches (document.patches);
            s = document.settings;
            s.audition = WorkspaceSettings::Audition::alternatePatch;
            s.auditionPatchId = alt.id;
            document.setSettings (s);

            document.cues.setSelectedIndex (0);
            expect (controller.preview (true) == CueController::GoResult::started);
            render (engine, scheduler, now, out, 4);
            expectWithinAbsoluteError (out.getRMSLevel (0, 0, blockSize), 0.0f, 1e-6f);
            expectWithinAbsoluteError (out.getRMSLevel (1, 0, blockSize), 0.3536f, 0.02f);
            stopEverything();

            // "always audition" makes a plain GO audition too
            s.alwaysAudition = true;
            document.setSettings (s);
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isAuditioning (a.id));
            stopEverything();
            s.alwaysAudition = false;
            s.audition = WorkspaceSettings::Audition::unchanged;
            document.setSettings (s);
            engine.setPatches ({});
        }

        beginTest ("reset all stops everything and puts the playhead on the first cue");
        {
            document.cues.setSelectedIndex (1);
            controller.preview();
            render (engine, scheduler, now, out, 2);
            controller.resetAll();
            render (engine, scheduler, now, out, 2);
            expectEquals (engine.getNumPlaying(), 0);
            expectEquals (document.cues.getPlayheadIndex(), 0);
        }

        beginTest ("an auto-follow cue with a pre-wait does not start its follower before it has even started");
        {
            document.cues.update (0, [] (Cue& c) { c.preWaitSeconds = 1.0; c.continueMode = ContinueMode::autoFollow; });
            document.cues.setSelectedIndex (0);
            now = 100.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (! engine.isPlaying (a.id));
            expect (! engine.isPlaying (b.id));

            now = 101.0;
            scheduler.tick();                      // A starts in this tick; the follow watch must not misread "not playing" as "ended"
            expect (engine.isPlaying (a.id));
            expect (! engine.isPlaying (b.id));
            render (engine, scheduler, now, out, 3);
            expect (! engine.isPlaying (b.id));

            engine.stop (a.id);
            render (engine, scheduler, now, out, 3);
            expect (engine.isPlaying (b.id));    // now A is over: B follows
            stopEverything();
            document.cues.update (0, [] (Cue& c) { c.preWaitSeconds = 0.0; c.continueMode = ContinueMode::none; });
        }

        beginTest ("auto-follow remembers the next cue by id: a cue inserted in between does not steal the follow");
        {
            document.cues.update (0, [] (Cue& c) { c.continueMode = ContinueMode::autoFollow; });
            document.cues.setSelectedIndex (0);
            now = 110.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 2);

            Cue inserted;
            inserted.name = "inserted";
            inserted.file = tone;
            document.cues.add (inserted, 1);       // now: a, inserted, b
            engine.stop (a.id);
            render (engine, scheduler, now, out, 3);
            expect (engine.isPlaying (b.id));
            expect (! engine.isPlaying (inserted.id));
            stopEverything();
            document.cues.remove (1);
            document.cues.update (0, [] (Cue& c) { c.continueMode = ContinueMode::none; });
        }

        beginTest ("restarting a cue drops its previous follow so the follower fires once");
        {
            document.cues.update (0, [] (Cue& c) { c.continueMode = ContinueMode::autoFollow; });
            document.cues.setSelectedIndex (0);
            now = 120.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 2);
            expectEquals (controller.getNumPending(), 1);

            document.cues.setPlayheadIndex (0);
            now = 121.0;
            expect (controller.go() == CueController::GoResult::started);   // hardStopRestart
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 2);
            expectEquals (controller.getNumPending(), 1);   // one follow, not two

            controller.resetSelected();                      // reset cancels the follow entirely
            document.cues.setSelectedIndex (0);
            controller.resetSelected();
            render (engine, scheduler, now, out, 3);
            expect (! engine.isPlaying (b.id));
            stopEverything();
            document.cues.update (0, [] (Cue& c) { c.continueMode = ContinueMode::none; });
        }

        beginTest ("ducks from several cues add up and each one is removed when its cue ends");
        {
            Cue c;
            c.name = "c";
            c.file = tone;
            c.duck.enabled = true;
            c.duck.levelDb = -12.0;
            c.duck.seconds = 0.1;
            document.cues.add (c);
            document.cues.update (document.cues.indexOf (b.id), [] (Cue& cue) { cue.duck.enabled = true; cue.duck.levelDb = -6.0; cue.duck.seconds = 0.1; });

            expect (engine.play (a));
            render (engine, scheduler, now, out, 2);
            controller.fireSequence (document.cues.indexOf (b.id));        // b ducks a by -6
            expect (engine.isPlaying (b.id));
            expectWithinAbsoluteError (engine.getDuckDb (a.id), -6.0, 1e-9);
            controller.fireSequence (document.cues.indexOf (c.id));        // c ducks a by -12 (and b)
            expect (engine.isPlaying (c.id));
            expectWithinAbsoluteError (engine.getDuckDb (a.id), -18.0, 1e-9);

            engine.stop (b.id);
            render (engine, scheduler, now, out, 3);
            expectWithinAbsoluteError (engine.getDuckDb (a.id), -12.0, 1e-9);   // b's share is gone, c's stays
            engine.stop (c.id);
            render (engine, scheduler, now, out, 3);
            expectWithinAbsoluteError (engine.getDuckDb (a.id), 0.0, 1e-9);
            stopEverything();
            document.cues.remove (document.cues.indexOf (c.id));
            document.cues.update (document.cues.indexOf (b.id), [] (Cue& cue) { cue.duck.enabled = false; });
        }

        beginTest ("wall-clock triggers are not skipped when a check arrives late");
        {
            document.cues.update (1, [] (Cue& cue) { cue.wallClock.enabled = true; cue.wallClock.hour = 9; cue.wallClock.minute = 30; cue.wallClock.second = 0; cue.wallClock.daysMask = 0x7f; });
            const juce::Time before (2026, 8, 2, 9, 29, 58, 0);   // 2 s before 09:30:00 (a Wednesday in September)
            controller.checkWallClock (before);
            expect (! engine.isPlaying (b.id));
            controller.checkWallClock (before + juce::RelativeTime::seconds (3.0));   // 09:30:01: 09:30:00 fell inside the gap
            expect (engine.isPlaying (b.id));
            stopEverything();
            document.cues.update (1, [] (Cue& cue) { cue.wallClock.enabled = false; });
        }

        beginTest ("a stop-pending loaded instance is not started by GO; the fresh load is");
        {
            document.cues.setSelectedIndex (0);
            expect (controller.loadSelected (0.0));
            expect (controller.loadSelected (0.0));   // replaces the first before any audio block ran
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 4);
            expect (engine.isPlaying (a.id));
            expectWithinAbsoluteError (out.getRMSLevel (0, 0, blockSize), 0.3536f, 0.02f);   // one instance plays
            stopEverything();
        }

        beginTest ("undo restores the whole selection and the playhead");
        {
            document.cues.setLockPlayheadToSelection (false);
            document.cues.setSelection ({ 0, 1 }, 0);
            document.cues.setPlayheadIndex (1);
            document.perform ("rename", [&] { document.cues.update (0, [] (Cue& cue) { cue.name = "renamed"; }); });
            document.cues.setSelectedIndex (1);
            document.cues.setPlayheadIndex (0);
            expect (document.undo());
            expectEquals ((int) document.cues.getSelectedIndices().size(), 2);
            expectEquals (document.cues.getSelectedIndex(), 0);
            expectEquals (document.cues.getPlayheadIndex(), 1);
            expectEquals (document.cues.get (0).name, juce::String ("a"));
            document.cues.setLockPlayheadToSelection (true);
            expectEquals (document.cues.getPlayheadIndex(), 0);   // turning the lock on snaps the playhead to the selection
            document.cues.setSelectedIndex (0);
        }

        beginTest ("a devamp cue ends the target's loop and starts the next cue at the loop point");
        {
            document.cues.update (0, [] (Cue& c) { c.audio.endSeconds = 0.5; c.audio.infiniteLoop = true; });
            Cue devamp;
            devamp.type = CueType::devamp;
            devamp.name = "devamp";
            devamp.devamp.targetId = a.id;
            devamp.devamp.startNextCue = true;
            devamp.devamp.stopTarget = true;
            document.cues.add (devamp, 1);   // a, devamp, b

            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);   // a loops forever
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 10);
            expect (engine.isPlaying (a.id));

            expect (controller.go() == CueController::GoResult::started);   // devamp fires
            controller.goKeyReleased();
            expect (! engine.isPlaying (b.id));                              // not yet: b waits for the loop point
            expectEquals (controller.getNumPending(), 1);
            render (engine, scheduler, now, out, 60);                        // past the end of the pass (0.5 s)
            expect (! engine.isPlaying (a.id));                              // stopped at the loop point
            expect (engine.isPlaying (b.id));                                // and b started there
            stopEverything();

            // no target playing: the devamp fails with a message
            expect (controller.trigger (document.cues.get (1)) == CueController::GoResult::failed);
            expect (statuses[statuses.size() - 1].isNotEmpty());

            // ... and GO on it fails the same way, keeping the error as the last status (no "GO:" line on top)
            document.cues.setPlayheadIndex (1);
            now += 1.0;
            const int before = statuses.size();
            expect (controller.go() == CueController::GoResult::failed);
            expectEquals (statuses.size(), before + 1);
            expect (statuses[statuses.size() - 1].contains (juce::CharPointer_UTF8 ("\xEC\x9E\xAC\xEC\x83\x9D \xEC\xA4\x91\xEC\x9D\xB4 \xEC\x95\x84\xEB\x8B\x99\xEB\x8B\x88\xEB\x8B\xA4")));   // 재생 중이 아닙니다
            expectEquals (document.cues.getPlayheadIndex(), 2);                                              // the playhead still moves on
            document.cues.remove (document.cues.indexOf (devamp.id));
            document.cues.update (0, [] (Cue& c) { c.audio.endSeconds = -1.0; c.audio.infiniteLoop = false; });
        }

        beginTest ("timeline group: every child starts at its own pre-wait, playhead goes past the group");
        {
            Cue g;
            g.name = "G";
            g.type = CueType::group;
            const int gi = document.cues.add (g);
            Cue x, y;
            x.name = "x"; x.file = tone; x.parentId = g.id;
            y.name = "y"; y.file = tone; y.parentId = g.id; y.preWaitSeconds = 0.3;
            document.cues.add (x);
            document.cues.add (y);
            Cue after;
            after.name = "after"; after.file = tone;
            document.cues.add (after);
            now += 1.0;
            document.cues.setPlayheadIndex (gi);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (x.id));
            expect (! engine.isPlaying (y.id));
            expectEquals (document.cues.getPlayheadIndex(), document.cues.indexOf (after.id));
            expect (controller.isCueActive (g.id));
            render (engine, scheduler, now, out, 30);   // 0.35 s
            expect (engine.isPlaying (y.id));

            // stopping the group stops both children and the group is no longer active
            controller.stopGroup (g.id, 0);
            render (engine, scheduler, now, out, 2);
            expect (! engine.isPlaying (x.id) && ! engine.isPlaying (y.id));
            expect (! controller.isCueActive (g.id));
            stopEverything();
            document.cues.removeIndices ({ document.cues.indexOf (g.id), document.cues.indexOf (after.id) });
        }

        beginTest ("playlist group: children one after another, second GO skips, loop starts over");
        {
            Cue g;
            g.name = "P";
            g.type = CueType::group;
            g.group.mode = GroupMode::playlist;
            const int gi = document.cues.add (g);
            Cue x, y;
            x.name = "x"; x.file = tone; x.parentId = g.id;
            y.name = "y"; y.file = tone; y.parentId = g.id;
            document.cues.add (x);
            document.cues.add (y);
            now += 1.0;
            document.cues.setPlayheadIndex (gi);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (x.id) && ! engine.isPlaying (y.id));

            // x ends -> y follows
            engine.stop (x.id);
            render (engine, scheduler, now, out, 2);
            expect (engine.isPlaying (y.id));

            // y ends -> the list is over (no loop)
            engine.stop (y.id);
            render (engine, scheduler, now, out, 2);
            expect (! engine.isPlaying (x.id) && ! engine.isPlaying (y.id));
            expect (! controller.isCueActive (g.id));

            // with loop: after y comes x again; a second trigger on the running group skips to the next child
            document.cues.update (gi, [] (Cue& c) { c.group.loop = true; });
            now += 1.0;
            expect (controller.trigger (document.cues.get (gi)) == CueController::GoResult::started);
            expect (engine.isPlaying (x.id));
            expect (controller.trigger (document.cues.get (gi)) == CueController::GoResult::ignored);   // skip
            render (engine, scheduler, now, out, 2);
            expect (engine.isPlaying (y.id));
            engine.stop (x.id);
            engine.stop (y.id);
            render (engine, scheduler, now, out, 2);
            expect (engine.isPlaying (x.id));   // looped
            stopEverything();
            expect (! controller.isCueActive (g.id));
            document.cues.remove (document.cues.indexOf (g.id));
        }

        beginTest ("playlist crossfade: the next child starts before the current one ends");
        {
            Cue g;
            g.name = "X";
            g.type = CueType::group;
            g.group.mode = GroupMode::playlist;
            g.group.crossfade = true;
            g.group.crossfadeSeconds = 0.5;
            const int gi = document.cues.add (g);
            Cue x, y;
            x.name = "x"; x.file = tone; x.parentId = g.id;   // 2 s
            y.name = "y"; y.file = tone; y.parentId = g.id;
            document.cues.add (x);
            document.cues.add (y);
            now += 1.0;
            expect (controller.trigger (document.cues.get (gi)) == CueController::GoResult::started);
            render (engine, scheduler, now, out, 100);   // 1.16 s: still only x
            expect (engine.isPlaying (x.id) && ! engine.isPlaying (y.id));
            render (engine, scheduler, now, out, 40);    // 1.62 s: inside the last 0.5 s of x
            expect (engine.isPlaying (y.id));
            expect (engine.isPlaying (x.id));            // fading out, still audible
            stopEverything();
            document.cues.remove (document.cues.indexOf (g.id));
        }

        beginTest ("start-first groups: enter moves the playhead inside, random takes every child once per round");
        {
            Cue g;
            g.name = "S";
            g.type = CueType::group;
            g.group.mode = GroupMode::startFirstEnter;
            const int gi = document.cues.add (g);
            Cue x, y;
            x.name = "x"; x.file = tone; x.parentId = g.id;
            y.name = "y"; y.file = tone; y.parentId = g.id;
            document.cues.add (x);
            document.cues.add (y);
            now += 1.0;
            document.cues.setPlayheadIndex (gi);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (x.id) && ! engine.isPlaying (y.id));
            expectEquals (document.cues.getPlayheadIndex(), document.cues.indexOf (y.id));   // entered the group
            stopEverything();

            document.cues.update (gi, [] (Cue& c) { c.group.mode = GroupMode::startFirst; });
            now += 1.0;
            document.cues.setPlayheadIndex (gi);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (engine.isPlaying (x.id));
            expectEquals (document.cues.getPlayheadIndex(), juce::jmin (document.cues.subtreeEnd (gi), document.cues.size() - 1));
            stopEverything();

            document.cues.update (gi, [] (Cue& c) { c.group.mode = GroupMode::random; });
            controller.randomChoice = [] (int) { return 0; };   // always the first candidate
            juce::StringArray order;

            for (int round = 0; round < 3; ++round)
            {
                now += 1.0;
                expect (controller.trigger (document.cues.get (gi)) == CueController::GoResult::started);
                order.add (engine.isPlaying (x.id) ? "x" : engine.isPlaying (y.id) ? "y" : "-");
                stopEverything();
            }

            expectEquals (order.joinIntoString (","), juce::String ("x,y,x"));   // x, then the only unplayed y, then a new round
            document.cues.remove (document.cues.indexOf (g.id));
        }

        beginTest ("control cues: start / pause / stop / load / reset / goto / arm / target / wait / memo");
        {
            Cue x;
            x.name = "x"; x.file = tone;
            const int xi = document.cues.add (x);
            auto makeControl = [&] (ControlKind kind, const juce::Uuid& target, double seconds = 0.0)
            {
                Cue c;
                c.type = CueType::control;
                c.control.kind = kind;
                c.control.targetId = target;
                c.control.seconds = seconds;
                c.name = "ctl";
                return c;
            };

            now += 1.0;
            Cue startCue = makeControl (ControlKind::start, x.id);
            document.cues.add (startCue);
            expect (controller.trigger (startCue) == CueController::GoResult::started);
            expect (engine.isPlaying (x.id));
            render (engine, scheduler, now, out, 4);

            Cue pauseCue = makeControl (ControlKind::pause, x.id);
            document.cues.add (pauseCue);
            expect (controller.trigger (pauseCue) == CueController::GoResult::started);
            render (engine, scheduler, now, out, 2);
            expect (engine.isPaused (x.id));
            expect (controller.trigger (startCue) == CueController::GoResult::started);   // start resumes a paused target
            render (engine, scheduler, now, out, 2);
            expect (! engine.isPaused (x.id) && engine.isPlaying (x.id));

            Cue stopCue = makeControl (ControlKind::stop, x.id);
            document.cues.add (stopCue);
            expect (controller.trigger (stopCue) == CueController::GoResult::started);
            render (engine, scheduler, now, out, 2);
            expect (! engine.isPlaying (x.id));

            Cue loadCue = makeControl (ControlKind::load, x.id, 0.5);
            document.cues.add (loadCue);
            expect (controller.trigger (loadCue) == CueController::GoResult::started);
            expect (engine.isLoaded (x.id));

            Cue resetCue = makeControl (ControlKind::reset, x.id);
            document.cues.add (resetCue);
            expect (controller.trigger (startCue) == CueController::GoResult::started);
            expect (controller.hasPlayed (x.id));
            expect (controller.trigger (resetCue) == CueController::GoResult::started);
            render (engine, scheduler, now, out, 2);
            expect (! engine.isPlaying (x.id));
            expect (! controller.hasPlayed (x.id));

            // goto: GO on it leaves the playhead on the target instead of the next row
            Cue gotoCue = makeControl (ControlKind::gotoCue, x.id);
            const int gi = document.cues.add (gotoCue);
            now += 1.0;
            document.cues.setPlayheadIndex (gi);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expectEquals (document.cues.getPlayheadIndex(), xi);

            // arm / disarm change the target's armed flag; target re-points a fade cue
            Cue disarmCue = makeControl (ControlKind::disarm, x.id);
            document.cues.add (disarmCue);
            expect (controller.trigger (disarmCue) == CueController::GoResult::started);
            expect (! document.cues.get (xi).armed);
            Cue armCue = makeControl (ControlKind::arm, x.id);
            document.cues.add (armCue);
            expect (controller.trigger (armCue) == CueController::GoResult::started);
            expect (document.cues.get (xi).armed);

            Cue fadeCue;
            fadeCue.type = CueType::fade;
            fadeCue.name = "fade";
            fadeCue.fade.targetId = x.id;
            document.cues.add (fadeCue);
            Cue targetCue = makeControl (ControlKind::target, fadeCue.id);
            targetCue.control.secondTargetId = a.id;
            document.cues.add (targetCue);
            expect (controller.trigger (targetCue) == CueController::GoResult::started);
            expect (document.cues.findById (fadeCue.id)->fade.targetId == a.id);

            // wait: active for its length, so an auto-follow behind it waits; memo does nothing
            Cue waitCue = makeControl (ControlKind::wait, juce::Uuid::null(), 0.4);
            waitCue.continueMode = ContinueMode::autoFollow;
            const int wi = document.cues.add (waitCue);
            Cue afterWait;
            afterWait.name = "afterWait"; afterWait.file = tone;
            document.cues.add (afterWait, wi + 1);
            now += 1.0;
            controller.fireSequence (wi);
            expect (controller.isCueActive (waitCue.id));
            expect (! engine.isPlaying (afterWait.id));
            render (engine, scheduler, now, out, 20);    // 0.23 s
            expect (! engine.isPlaying (afterWait.id));
            render (engine, scheduler, now, out, 20);    // 0.46 s: the wait is over
            expect (engine.isPlaying (afterWait.id));
            expect (! controller.isCueActive (waitCue.id));

            Cue memoCue = makeControl (ControlKind::memo, juce::Uuid::null());
            document.cues.add (memoCue);
            expect (controller.trigger (memoCue) == CueController::GoResult::started);
            expect (controller.hasPlayed (memoCue.id));

            // a control cue without its target fails with a message
            Cue broken = makeControl (ControlKind::stop, juce::Uuid::null());
            document.cues.add (broken);
            expect (controller.trigger (broken) == CueController::GoResult::failed);
            stopEverything();

            while (document.cues.size() > 2)
                document.cues.remove (document.cues.size() - 1);
        }

        beginTest ("sequence recording remembers the started cues with their times");
        {
            now = 100.0;
            document.cues.setPlayheadIndex (0);
            controller.startRecording();
            expect (controller.isRecording());
            expect (controller.go() == CueController::GoResult::started);   // a at 0
            controller.goKeyReleased();
            now = 101.5;
            expect (controller.go() == CueController::GoResult::started);   // b at 1.5
            controller.goKeyReleased();
            expectEquals (controller.getNumRecorded(), 2);
            const auto starts = controller.stopRecording();
            expect (! controller.isRecording());
            expectEquals ((int) starts.size(), 2);
            expect (starts[0].cueId == a.id);
            expectWithinAbsoluteError (starts[0].seconds, 0.0, 1e-9);
            expect (starts[1].cueId == b.id);
            expectWithinAbsoluteError (starts[1].seconds, 1.5, 1e-9);
            expectEquals (controller.getNumRecorded(), 0);
            stopEverything();
        }

        beginTest ("cues in an inactive list: hotkeys, control targets and follows reach them; goto switches the list");
        {
            // a second list with y (hotkey F9) and z following it
            document.addContainer ("second", false);
            document.setActiveContainer (1);
            Cue y, z;
            y.name = "y"; y.file = tone; y.hotkey = juce::KeyPress (juce::KeyPress::F9Key).getTextDescription(); y.continueMode = ContinueMode::autoFollow;
            z.name = "z"; z.file = tone;
            document.cues.add (y);
            document.cues.add (z);
            document.setActiveContainer (0);
            expectEquals (document.getActiveContainer(), 0);
            expect (document.cues.indexOf (y.id) < 0);

            now += 1.0;
            expect (controller.handleHotkey (juce::KeyPress (juce::KeyPress::F9Key)));
            expect (engine.isPlaying (y.id));
            engine.stop (y.id);
            render (engine, scheduler, now, out, 2);
            expect (engine.isPlaying (z.id));   // the auto-follow ran inside the inactive list
            stopEverything();

            // a control cue in the active list starting a cue of the other list
            Cue startY;
            startY.type = CueType::control;
            startY.control.kind = ControlKind::start;
            startY.control.targetId = y.id;
            startY.name = "start y";
            document.cues.add (startY);
            now += 1.0;
            expect (controller.trigger (startY) == CueController::GoResult::started);
            expect (engine.isPlaying (y.id));
            stopEverything();

            // goto across lists brings the other list to the front with the playhead on the target
            Cue gotoZ;
            gotoZ.type = CueType::control;
            gotoZ.control.kind = ControlKind::gotoCue;
            gotoZ.control.targetId = z.id;
            gotoZ.name = "goto z";
            const int gi = document.cues.add (gotoZ);
            now += 1.0;
            document.cues.setPlayheadIndex (gi);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expectEquals (document.getActiveContainer(), 1);
            expectEquals (document.cues.getPlayheadIndex(), document.cues.indexOf (z.id));

            // a cart has no GO
            document.setContainerCart (1, true, 2, 2);
            now += 1.0;
            expect (controller.go() == CueController::GoResult::nothingSelected);
            document.setContainerCart (1, false, 2, 2);
            document.setActiveContainer (0);
            document.removeContainer (1);
            stopEverything();
            document.cues.remove (document.cues.indexOf (gotoZ.id));
            document.cues.remove (document.cues.indexOf (startY.id));
        }

        beginTest ("a cue that would start itself is refused; a stopped cue releases its duck");
        {
            // start control cue A targets start control cue B, which targets A: no stack overflow, a failure message
            Cue ca, cb;
            ca.type = CueType::control; ca.control.kind = ControlKind::start; ca.name = "ca";
            cb.type = CueType::control; cb.control.kind = ControlKind::start; cb.name = "cb";
            ca.control.targetId = cb.id;
            cb.control.targetId = ca.id;
            document.cues.add (ca);
            document.cues.add (cb);
            now += 1.0;
            const int before = statuses.size();
            expect (controller.trigger (document.cues.get (document.cues.indexOf (ca.id))) == CueController::GoResult::started);   // ca starts cb, cb refuses ca
            expect (statuses.size() > before);
            expect (statuses[statuses.size() - 1].isNotEmpty());
            stopEverything();
            document.cues.remove (document.cues.indexOf (cb.id));
            document.cues.remove (document.cues.indexOf (ca.id));

            // a ducking cue that is stopped (pending cancelled) lets the others come back up
            document.cues.update (1, [] (Cue& c) { c.duck.enabled = true; c.duck.levelDb = -12.0; c.duck.seconds = 0.05; });
            now += 1.0;
            expect (controller.fire (a.id) == CueController::GoResult::started);
            render (engine, scheduler, now, out, 4);
            expect (controller.fire (b.id) == CueController::GoResult::started);   // b ducks a
            render (engine, scheduler, now, out, 10);
            expectWithinAbsoluteError (engine.getDuckDb (a.id), -12.0, 0.01);
            controller.stopCue (b.id);
            render (engine, scheduler, now, out, 30);
            expectWithinAbsoluteError (engine.getDuckDb (a.id), 0.0, 0.01);   // released with the cleanup watch
            document.cues.update (1, [] (Cue& c) { c.duck.enabled = false; });
            stopEverything();
        }

        beginTest ("played cues are remembered for the second colour until reset");
        {
            controller.resetAll();
            render (engine, scheduler, now, out, 2);
            document.cues.setPlayheadIndex (0);
            expect (! controller.hasPlayed (a.id));
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            expect (controller.hasPlayed (a.id));
            controller.resetAll();
            expect (! controller.hasPlayed (a.id));
            render (engine, scheduler, now, out, 2);
        }

        expect (dir.deleteRecursively());
    }
};

static CueControllerTests cueControllerTests;

} // namespace gocue::tests
