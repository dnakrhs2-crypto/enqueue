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

// Selected reproductions from AuditE2 (1, 7) and AuditE1 (1).
// Includes nested and pre-wait group-entry regressions.
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
        nestedEnter (tone.getFile());
        controlEnter (tone.getFile());
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
