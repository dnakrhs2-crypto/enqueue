#include "app/CueController.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{
namespace
{

struct AuditDirectory
{
    const juce::File parent = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File path = parent.getChildFile ("enqueue_audit_e1_" + juce::Uuid().toString());

    ~AuditDirectory()
    {
        if (path.isAChildOf (parent))
            path.deleteRecursively();
    }
};

} // namespace

// Selected reproductions from AuditE2 (1, 2, 7) and AuditE1 (1).
// The original test bodies and their correct-behaviour assertions are preserved.
class AuditFixCueTests : public juce::UnitTest
{
public:
    AuditFixCueTests() : juce::UnitTest ("AuditFixCue", "Enqueue") {}

    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 512;
    static constexpr double blockSeconds = blockSize / sampleRate;

    // Same synchronous engine and render/reap/tick order as CueControllerTests.
    // Each claim gets its own document, controller, scheduler and fake clock.
    struct Fixture
    {
        double now = 0.0;
        AudioEngine engine { 0 };
        ProjectDocument document;
        Scheduler scheduler { [this] { return now; } };
        CueController controller { engine, document, scheduler };
        juce::AudioBuffer<float> out { 2, blockSize };

        Fixture()
        {
            engine.prepare (sampleRate, blockSize);
            document.clock = [this] { return now * 1000.0; };
            controller.startRecording();
        }

        ~Fixture()
        {
            controller.cancelPending();
            engine.stopAll();
        }

        void render (int blocks = 1)
        {
            for (int i = 0; i < blocks; ++i)
            {
                engine.renderBlock (out, blockSize);
                now += blockSeconds;
                engine.reapFinishedPlayers();
                scheduler.tick();
            }
        }

        void renderUntil (double until)
        {
            while (now < until)
                render();
        }

        CueController::GoResult go()
        {
            const auto result = controller.go();
            controller.goKeyReleased();
            return result;
        }

        juce::String playheadName() const
        {
            const auto* cue = document.cues.getPlayhead();
            return cue != nullptr ? cue->name : "<none>";
        }

        juce::Uuid playheadId() const
        {
            const auto* cue = document.cues.getPlayhead();
            return cue != nullptr ? cue->id : juce::Uuid::null();
        }
    };

    static Cue audio (const juce::String& name, const juce::File& file, double seconds = 10.0)
    {
        Cue cue;
        cue.name = name;
        cue.file = file;
        cue.audio.endSeconds = seconds;
        return cue;
    }

    static Cue group (GroupMode mode)
    {
        Cue cue;
        cue.name = "G";
        cue.type = CueType::group;
        cue.group.mode = mode;
        return cue;
    }

    bool writeSine (const juce::File& file)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr, "Fixture: could not create the temporary WAV");
        if (stream == nullptr)
            return false;

        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
            .withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
        expect (writer != nullptr, "Fixture: could not create the WAV writer");
        if (writer == nullptr)
            return false;

        const int samples = (int) (20.0 * sampleRate);
        juce::AudioBuffer<float> buffer (2, samples);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < samples; ++i)
                buffer.setSample (ch, i, 0.5f * (float) std::sin (
                    2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));

        const bool written = writer->writeFromAudioSampleBuffer (buffer, 0, samples);
        expect (written, "Fixture: could not write the WAV samples");
        return written;
    }

    void runTest() override
    {
        const juce::TemporaryFile tone (".wav");
        beginTest ("audit E2-1: start-first-enter keeps playhead on B");
        if (! writeSine (tone.getFile()))
            return;

        firstEnter (tone.getFile());
        disabledFollow (tone.getFile());
        nestedEnter (tone.getFile());
        controlEnter (tone.getFile());
        retriggeredFollow (tone.getFile());
        groupFollowTermination (tone.getFile());
        followGoto (tone.getFile());
        followSelection (tone.getFile());
        preWaitEnter (tone.getFile());
        unlockedGrouping (tone.getFile());

        beginTest ("audit E1-1: paused hotkey fade-in starts at floor");
        AuditDirectory directory;
        const auto created = directory.path.createDirectory();
        expect (created.wasOk(), created.getErrorMessage());
        if (created.failed())
            return;

        const auto constantTone = writeConstant (directory.path.getChildFile ("constant.wav"));
        if (! constantTone.existsAsFile())
            return;

        pausedFadeIn (constantTone);
    }

    void firstEnter (const juce::File& tone)
    {
        Fixture f;
        auto g = group (GroupMode::startFirstEnter);
        auto a = audio ("A", tone), b = audio ("B", tone), c = audio ("C", tone);
        a.parentId = b.parentId = g.id;
        for (const auto& cue : { g, a, b, c })
            f.document.cues.add (cue);

        f.document.cues.setPlayheadIndex (0);
        expect (f.go() == CueController::GoResult::started);
        expect (f.engine.isPlaying (a.id));
        expect (! f.engine.isPlaying (b.id) && ! f.engine.isPlaying (c.id));
        const auto afterFirstGo = f.playheadName();
        expect (f.playheadId() == b.id, "Expected playhead B; observed " + afterFirstGo);

        expect (f.go() == CueController::GoResult::started);
        expect (f.engine.isPlaying (b.id), "The next GO must start B");
        expect (! f.engine.isPlaying (c.id), "The next GO must not skip B and start C");
        logMessage ("OBSERVED E2-1: first GO playhead=" + afterFirstGo
                    + "; next GO: B=" + juce::String ((int) f.engine.isPlaying (b.id))
                    + ", C=" + juce::String ((int) f.engine.isPlaying (c.id)));
    }

    void disabledFollow (const juce::File& tone)
    {
        beginTest ("audit E2-2: auto-follow skips disabled B in playhead");
        Fixture f;
        auto a = audio ("A", tone, 0.2), b = audio ("B", tone);
        auto c = audio ("C", tone), d = audio ("D", tone);
        a.continueMode = ContinueMode::autoFollow;
        b.armed = false;
        b.continueMode = ContinueMode::none;
        // The default second-trigger action is hardStopRestart, as in the claim.
        for (const auto& cue : { a, b, c, d })
            f.document.cues.add (cue);

        f.document.cues.setPlayheadIndex (0);
        expect (f.go() == CueController::GoResult::started);
        const auto afterFirstGo = f.playheadName();
        expect (f.playheadId() == d.id, "Expected playhead D after GO; observed " + afterFirstGo);
        f.renderUntil (0.3);
        expect (! f.engine.isPlaying (a.id) && ! f.engine.isPlaying (b.id));
        expect (f.engine.isPlaying (c.id), "Fixture: C must have auto-followed A");
        const auto beforeNextGo = f.playheadName();
        const auto firstOrder = f.engine.getStartOrder (c.id);

        expect (f.go() == CueController::GoResult::started);
        const auto secondOrder = f.engine.getStartOrder (c.id);
        expectEquals (secondOrder, firstOrder, "The next GO must not restart C");
        expect (f.engine.isPlaying (d.id), "The next GO must start D");
        logMessage ("OBSERVED E2-2: playhead after GO=" + afterFirstGo + ", after follow=" + beforeNextGo
                    + "; C start order=" + juce::String (firstOrder) + " -> " + juce::String (secondOrder)
                    + "; D playing=" + juce::String ((int) f.engine.isPlaying (d.id)));
    }

    void nestedEnter (const juce::File& tone)
    {
        for (const double preWait : { 0.0, 0.5 })
            for (const bool enterOuter : { false, true })
            {
                beginTest ("audit R2-1: " + juce::String (preWait > 0.0 ? "scheduled " : "immediate ")
                           + (enterOuter ? "nested enter belongs to the fired enter group"
                                         : "nested enter does not leak through start-first"));
                Fixture f;
                auto g = group (enterOuter ? GroupMode::startFirstEnter : GroupMode::startFirst);
                g.preWaitSeconds = preWait;
                auto h = group (GroupMode::startFirstEnter);
                h.name = "H";
                h.parentId = g.id;
                auto a = audio ("A", tone), b = audio ("B", tone), c = audio ("C", tone);
                a.parentId = b.parentId = h.id;
                for (const auto& cue : { g, h, a, b, c })
                    f.document.cues.add (cue);

                f.document.cues.setPlayheadIndex (0);
                expect (f.go() == CueController::GoResult::started);
                expect (f.playheadId() == (enterOuter ? b.id : c.id),
                        "Only the group fired by GO may enter; observed " + f.playheadName());
                f.renderUntil (preWait + 0.1);
                expect (f.engine.isPlaying (a.id));
                expect (f.playheadId() == (enterOuter ? b.id : c.id));
                expect (f.go() == CueController::GoResult::started);
                expect (f.engine.isPlaying (enterOuter ? b.id : c.id));
                expect (! f.engine.isPlaying (enterOuter ? c.id : b.id));
            }
    }

    void controlEnter (const juce::File& tone)
    {
        beginTest ("audit R2-1: start control does not inherit its target's enter destination");
        Fixture f;
        auto g = group (GroupMode::startFirstEnter);
        auto a = audio ("A", tone), b = audio ("B", tone), next = audio ("next", tone);
        a.parentId = b.parentId = g.id;
        Cue start;
        start.type = CueType::control;
        start.control.kind = ControlKind::start;
        start.control.targetId = g.id;
        for (const auto& cue : { start, next, g, a, b })
            f.document.cues.add (cue);

        f.document.cues.setPlayheadIndex (0);
        expect (f.go() == CueController::GoResult::started);
        expect (f.engine.isPlaying (a.id));
        expect (f.playheadId() == next.id, "Start control must leave its own next cue ready");
        expect (f.go() == CueController::GoResult::started);
        expect (f.engine.isPlaying (next.id));
        expect (! f.engine.isPlaying (b.id));
    }

    void retriggeredFollow (const juce::File& tone)
    {
        const juce::StringArray names {
            "ignored follow corrects GO to the actual next cue",
            "ignored follow preserves a moved playhead",
            "ignored follow preserves a playhead moved away and back",
            "ignored follow cannot undo a later GO on the last row",
            "ignored follow cannot undo a list switch and return",
            "a sequence fired without GO cannot correct the playhead"
        };
        for (int cursorMove = 0; cursorMove < names.size(); ++cursorMove)
        {
            beginTest ("audit R2-2: " + names[cursorMove]);
            Fixture f;
            auto a = audio ("A", tone, 0.2), b = audio ("B", tone);
            auto c = audio ("C", tone), d = audio ("D", tone), e = audio ("E", tone);
            a.continueMode = ContinueMode::autoFollow;
            b.armed = false;
            c.continueMode = ContinueMode::autoContinue;
            c.secondTrigger = SecondTriggerAction::hardStop;
            for (const auto& cue : { a, b, c, d, e })
                f.document.cues.add (cue);
            const int otherList = f.document.addContainer ("other", false);

            f.document.cues.setSelectedIndex (2);
            expect (f.controller.preview() == CueController::GoResult::started);
            expect (f.engine.isPlaying (c.id));
            f.document.cues.setPlayheadIndex (cursorMove == 5 ? 4 : 0);
            if (cursorMove == 5)
                expectEquals (f.controller.fireSequence (0), 4);
            else
                expect (f.go() == CueController::GoResult::started);
            expect (f.playheadId() == e.id, "GO initially anticipates the full continuation");
            if (cursorMove == 1 || cursorMove == 2)
                f.document.cues.setPlayheadIndex (1);
            if (cursorMove == 2)
                f.document.cues.setPlayheadIndex (4);
            if (cursorMove == 3)
                expect (f.go() == CueController::GoResult::started);
            if (cursorMove == 4)
            {
                f.document.setActiveContainer (otherList);
                f.document.setActiveContainer (0);
            }

            f.renderUntil (0.4);
            expect (! f.engine.isPlaying (a.id) && ! f.engine.isPlaying (b.id));
            expect (! f.engine.isPlaying (c.id), "The follow must hard-stop the previewed C");
            expect (! f.engine.isPlaying (d.id));
            expectEquals ((int) f.engine.isPlaying (e.id), (int) (cursorMove == 3));
            expectEquals (f.controller.getNumPending(), 0);
            const auto expected = cursorMove == 0 ? d.id : cursorMove == 1 ? b.id : e.id;
            expect (f.playheadId() == expected, "Unexpected playhead after ignored follow: " + f.playheadName());
            if (cursorMove == 0)
            {
                expect (f.go() == CueController::GoResult::started);
                expect (f.engine.isPlaying (d.id), "The next GO must start the unplayed D");
                expect (! f.engine.isPlaying (e.id));
            }
        }
    }

    void groupFollowTermination (const juce::File& tone)
    {
        for (const bool enter : { false, true })
            for (const double preWait : { 0.0, 0.5 })
            {
                beginTest ("audit R2-2: " + juce::String (preWait > 0.0 ? "scheduled " : "immediate ")
                           + (enter ? "enter retains correction through successive child follows"
                                    : "start-first isolates child follow termination"));
                Fixture f;
                auto g = group (enter ? GroupMode::startFirstEnter : GroupMode::startFirst);
                g.preWaitSeconds = preWait;
                auto a = audio ("A", tone, 0.1), b = audio ("B", tone, 0.1);
                auto c = audio ("C", tone), d = audio ("D", tone), e = audio ("E", tone);
                auto outside = audio ("outside", tone);
                a.parentId = b.parentId = c.parentId = d.parentId = e.parentId = g.id;
                a.continueMode = b.continueMode = ContinueMode::autoFollow;
                c.continueMode = ContinueMode::autoContinue;
                c.secondTrigger = SecondTriggerAction::hardStop;
                // C is started independently after G, so G's own restart rule cannot stop it first.
                for (const auto& cue : { g, a, b, c, d, e, outside })
                    f.document.cues.add (cue);
                f.document.cues.setPlayheadIndex (0);
                expect (f.go() == CueController::GoResult::started);
                expect (f.playheadId() == (enter ? e.id : outside.id));
                f.renderUntil (preWait + 0.05);
                expect (f.controller.fire (c.id) == CueController::GoResult::started);
                f.renderUntil (preWait + 0.4);
                expect (! f.engine.isPlaying (c.id) && ! f.engine.isPlaying (d.id));
                expect (f.controller.hasPlayed (b.id), "The second child follow must have run");
                expect (f.playheadId() == (enter ? d.id : outside.id));
                expect (f.go() == CueController::GoResult::started);
                expect (f.engine.isPlaying (enter ? d.id : outside.id));
            }
    }

    void followGoto (const juce::File& tone)
    {
        beginTest ("audit R2-2: a follow's goto wins even when it targets the anticipated cursor");
        Fixture f;
        auto a = audio ("A", tone, 0.2), c = audio ("C", tone);
        auto d = audio ("D", tone), e = audio ("E", tone);
        a.continueMode = ContinueMode::autoFollow;
        c.continueMode = ContinueMode::autoContinue;
        c.secondTrigger = SecondTriggerAction::hardStop;
        Cue jump;
        jump.type = CueType::control;
        jump.control.kind = ControlKind::gotoCue;
        jump.control.targetId = e.id;
        jump.continueMode = ContinueMode::autoContinue;
        for (const auto& cue : { a, jump, c, d, e })
            f.document.cues.add (cue);
        expect (f.controller.fire (c.id) == CueController::GoResult::started);
        f.document.cues.setPlayheadIndex (0);
        expect (f.go() == CueController::GoResult::started);
        expect (f.playheadId() == e.id);
        f.renderUntil (0.4);
        expect (! f.engine.isPlaying (c.id) && ! f.engine.isPlaying (d.id));
        expect (f.playheadId() == e.id, "The explicit goto must beat the ignored follow's D destination");
    }

    void followSelection (const juce::File& tone)
    {
        beginTest ("audit R2-2: an unchanged follow destination preserves multiple selection");
        Fixture f;
        auto a = audio ("A", tone, 0.2), b = audio ("B", tone), c = audio ("C", tone);
        a.continueMode = ContinueMode::autoFollow;
        for (const auto& cue : { a, b, c })
            f.document.cues.add (cue);
        f.document.cues.setPlayheadIndex (0);
        expect (f.go() == CueController::GoResult::started);
        f.document.cues.setSelection ({ 1, 2 }, 2);
        expect (f.playheadId() == c.id);
        f.renderUntil (0.4);
        expect (f.engine.isPlaying (b.id));
        expect (f.playheadId() == c.id);
        expectEquals ((int) f.document.cues.getSelectedIndices().size(), 2);
    }

    void preWaitEnter (const juce::File& tone)
    {
        for (const bool childSequence : { false, true })
        {
            beginTest (childSequence ? "audit R2-3: pre-wait enter skips the first child's sequence"
                                     : "audit R2-3: pre-wait enter keeps B ready before and after the start");
            Fixture f;
            auto g = group (GroupMode::startFirstEnter);
            g.preWaitSeconds = 1.0;
            auto a = audio ("A", tone), b = audio ("B", tone);
            auto c = audio ("C", tone), outside = audio ("outside", tone);
            a.parentId = b.parentId = c.parentId = g.id;
            if (childSequence)
            {
                a.continueMode = ContinueMode::autoContinue;
                a.postWaitSeconds = 0.1;
                b.preWaitSeconds = 0.1;
            }
            for (const auto& cue : { g, a, b, c, outside })
                f.document.cues.add (cue);

            f.document.cues.setPlayheadIndex (0);
            expect (f.go() == CueController::GoResult::started);
            const auto destination = childSequence ? c.id : b.id;
            expect (f.playheadId() == destination, "The scheduled group must return its child destination immediately");
            expect (! f.engine.isPlaying (a.id));
            f.renderUntil (0.9);
            expect (! f.engine.isPlaying (a.id));
            f.renderUntil (1.3);
            expect (f.engine.isPlaying (a.id));
            expectEquals ((int) f.engine.isPlaying (b.id), (int) childSequence);
            expect (f.playheadId() == destination, "The group start must preserve the enter destination");
            expect (f.go() == CueController::GoResult::started);
            expect (f.engine.isPlaying (destination));
            expect (! f.engine.isPlaying (outside.id));
        }
    }

    void unlockedGrouping (const juce::File& tone)
    {
        beginTest ("audit E2-7: grouping preserves unlocked playhead C");
        Fixture f;
        auto& list = f.document.cues;
        const auto a = audio ("A", tone), b = audio ("B", tone), c = audio ("C", tone);
        const auto d = audio ("D", tone), e = audio ("E", tone);
        for (const auto& cue : { a, b, c, d, e })
            list.add (cue);
        list.setLockPlayheadToSelection (false);
        list.setPlayheadIndex (2);
        list.setSelection ({ 0, 1 }, 0);
        expect (f.playheadId() == c.id, "Fixture: selection must leave the unlocked playhead on C");
        expectEquals ((int) list.getSelectedIndices().size(), 2);

        expectEquals (list.wrapInGroup (list.getSelectedIndices(), group (GroupMode::timeline)), 0);
        expectEquals (list.size(), 6);
        const auto afterGrouping = f.playheadName();
        expect (f.playheadId() == c.id, "Grouping A/B must preserve C; observed " + afterGrouping);
        expect (f.go() == CueController::GoResult::started);
        expect (f.engine.isPlaying (c.id), "The next GO must start C");
        expect (! f.engine.isPlaying (e.id), "The next GO must not jump to E");
        logMessage ("OBSERVED E2-7: playhead before=C, after grouping=" + afterGrouping
                    + "; next GO: C=" + juce::String ((int) f.engine.isPlaying (c.id))
                    + ", E=" + juce::String ((int) f.engine.isPlaying (e.id)));
    }

    static constexpr float signalLevel = 0.5f;

    static void render (AudioEngine& engine, juce::AudioBuffer<float>& out, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            engine.renderBlock (out, blockSize);
    }

    juce::File writeConstant (const juce::File& file)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (sampleRate)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (24));
        expect (writer != nullptr, "constant WAV fixture must be writable");
        if (writer == nullptr)
            return {};

        juce::AudioBuffer<float> data (2, (int) sampleRate * 2);
        for (int ch = 0; ch < data.getNumChannels(); ++ch)
            for (int i = 0; i < data.getNumSamples(); ++i)
                data.setSample (ch, i, signalLevel);

        const bool written = writer->writeFromAudioSampleBuffer (data, 0, data.getNumSamples());
        expect (written, "constant WAV fixture must be complete");
        return written ? file : juce::File();
    }

    void pausedFadeIn (const juce::File& tone)
    {
        AudioEngine engine (0); // synchronous disk reads, as in FadeRunnerTests
        engine.prepare (sampleRate, blockSize);
        ProjectDocument document;
        document.clock = [] { return 0.0; };
        document.settings.minLevelDb = -60.0;
        Cue target;
        target.file = tone;
        target.gainDb = 0.0;
        target.audio.preservePitch = false;
        document.cues.add (target);

        Cue fade;
        fade.type = CueType::fade;
        fade.fade.mode = FadeMode::fadeIn;
        fade.fade.targetId = target.id;
        fade.fade.durationSeconds = 5.0;
        const juce::KeyPress hotkey (juce::KeyPress::F7Key);
        fade.hotkey = hotkey.getTextDescription();
        document.cues.add (fade);

        // Neither rendering nor a runner tick advances this clock. No fade
        // progress can account for an above-floor sample in the resumed block.
        Scheduler scheduler ([] { return 100.0; });
        CueController controller (engine, document, scheduler);
        controller.getFadeRunner().clock = [] { return 100.0; };
        juce::AudioBuffer<float> out (2, blockSize);
        expect (controller.trigger (target) == CueController::GoResult::started);
        render (engine, out, 8);
        const float baseline = out.getMagnitude (0, 0, blockSize);
        expectWithinAbsoluteError (baseline, signalLevel, 1.0e-6f);

        expect (controller.togglePause(), "P must pause the target");
        render (engine, out, 4); // finish the pause gate and exercise paused early returns
        expect (engine.isPaused (target.id));
        expectWithinAbsoluteError (out.getMagnitude (0, 0, blockSize), 0.0f, 1.0e-7f);
        expect (controller.handleHotkey (hotkey), "use cue hotkey, since GO resumes paused cues first");
        expect (controller.getFadeRunner().isRunning (fade.id));
        controller.getFadeRunner().tick();

        AudioEngine::LiveState live;
        expect (engine.getLiveState (target.id, live));
        expectWithinAbsoluteError (live.gainDb, -60.0, 1.0e-9);
        render (engine, out, 1);
        const float firstPeak = out.getMagnitude (0, 0, blockSize);
        const float floorPeak = baseline * juce::Decibels::decibelsToGain (-60.0f);
        int peakSample = 0;
        for (int i = 1; i < blockSize; ++i)
            if (std::abs (out.getSample (0, i)) > std::abs (out.getSample (0, peakSample)))
                peakSample = i;

        logMessage ("E1-1 observed: baseline=" + juce::String (baseline, 9)
                    + ", resumed first-block PCM peak=" + juce::String (firstPeak, 9)
                    + ", peak sample=" + juce::String (peakSample)
                    + ", peak/baseline=" + juce::String (firstPeak / baseline, 9)
                    + ", -60 dB ceiling=" + juce::String (floorPeak, 9));
        expectLessOrEqual (firstPeak, floorPeak + 1.0e-6f,
                           "first resumed block must not expose the pre-pause gain");

        render (engine, out, 1);
        logMessage ("E1-1 control: second-block PCM peak="
                    + juce::String (out.getMagnitude (0, 0, blockSize), 9));
        expectWithinAbsoluteError (out.getMagnitude (0, 0, blockSize), floorPeak, 1.0e-6f);
    }

};

static AuditFixCueTests auditFixCueTests;

} // namespace gocue::tests
