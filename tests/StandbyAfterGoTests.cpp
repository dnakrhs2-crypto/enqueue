#include "app/CueController.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace gocue::tests
{
namespace
{
struct StandbyFixture
{
    explicit StandbyFixture (const juce::File& tone, bool keepStandby = false)
        : scheduler ([this] { return now; }), controller (engine, document, scheduler)
    {
        engine.prepare (44100.0, 512);
        document.clock = [this] { return now; };
        auto settings = document.settings;
        settings.goKeepsStandby = keepStandby;
        document.setSettings (settings);
        document.cues.setLockPlayheadToSelection (settings.lockPlayheadToSelection);
        for (int i = 0; i < 3; ++i)
        {
            Cue cue;
            cue.name = "cue " + juce::String (i + 1);
            cue.file = tone;
            document.cues.add (cue);
        }
        document.cues.setPlayheadIndex (0);
        controller.onStatus = [this] (const juce::String& text, bool) { status = text; };
    }

    ~StandbyFixture()
    {
        controller.hardStopAll();
        render (2);
    }

    void render (int blocks)
    {
        juce::AudioBuffer<float> out (2, 512);
        for (int i = 0; i < blocks; ++i)
        {
            engine.renderBlock (out, 512);
            now += 512.0 / 44100.0;
            engine.reapFinishedPlayers();
            scheduler.tick();
        }
    }

    juce::Uuid id (int index) const { return document.cues.get (index).id; }

    double now = 10.0;
    AudioEngine engine { 0 };
    ProjectDocument document;
    Scheduler scheduler;
    CueController controller;
    juce::String status;
};
}

class StandbyAfterGoTests : public juce::UnitTest
{
public:
    StandbyAfterGoTests() : UnitTest ("StandbyAfterGo", "Enqueue") {}

    void runTest() override
    {
        using Result = CueController::GoResult;
        beginTest ("setup");
        const auto directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("enqueue_standby_" + juce::Uuid().toString());
        expect (directory.createDirectory().wasOk());
        const auto tone = directory.getChildFile ("tone.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream (tone.createOutputStream());
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                .withSampleRate (44100).withNumChannels (2).withBitsPerSample (16));
            expect (writer != nullptr);
            if (writer != nullptr)
            {
                juce::AudioBuffer<float> samples (2, 44100 * 3);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < samples.getNumSamples(); ++i)
                        samples.setSample (ch, i, 0.25f);
                expect (writer->writeFromAudioSampleBuffer (samples, 0, samples.getNumSamples()));
            }
        }

        beginTest ("default GO advances the playhead and locked selection");
        {
            expect (! WorkspaceSettings().goKeepsStandby);
            StandbyFixture f (tone);
            expect (f.controller.go() == Result::started);
            expect (f.engine.isPlaying (f.id (0)));
            expectEquals (f.document.cues.getPlayheadIndex(), 1);
            expectEquals (f.document.cues.getSelectedIndex(), 1);
            expect (f.status.startsWith ("GO: "));
        }

        for (const auto action : { SecondTriggerAction::nothing, SecondTriggerAction::hardStop,
                                   SecondTriggerAction::hardStopRestart })
        {
            beginTest ("designated standby keeps the cue and applies second trigger " + juce::String ((int) action));
            StandbyFixture f (tone, true);
            f.document.cues.update (0, [action] (Cue& cue) { cue.secondTrigger = action; });
            expect (f.controller.go() == Result::started);
            expectEquals (f.document.cues.getPlayheadIndex(), 0);
            expectEquals (f.document.cues.getSelectedIndex(), 0);
            expect (f.engine.isPlaying (f.id (0)));
            expect (f.status.startsWith ("GO: "));
            f.controller.goKeyReleased();
            f.render (10);

            const auto result = f.controller.go();
            expect (result == (action == SecondTriggerAction::hardStopRestart ? Result::started : Result::ignored));
            f.render (2);
            expectEquals (f.document.cues.getPlayheadIndex(), 0);
            expectEquals (f.document.cues.getSelectedIndex(), 0);
            expect (! f.engine.isPlaying (f.id (1)));
            expect (f.engine.isPlaying (f.id (0)) == (action != SecondTriggerAction::hardStop));
            for (const auto& playing : f.engine.getPlayingCues())
                if (playing.id == f.id (0))
                {
                    if (action == SecondTriggerAction::hardStopRestart)
                        expectLessThan (playing.positionSeconds, 0.05);
                    else
                        expectGreaterThan (playing.positionSeconds, 0.1);
                }
        }

        beginTest ("auto-continue still starts the sequence while standby stays on its first cue");
        {
            StandbyFixture f (tone, true);
            f.document.cues.update (0, [] (Cue& cue)
            {
                cue.continueMode = ContinueMode::autoContinue;
                cue.postWaitSeconds = 1.0;
            });
            expect (f.controller.go() == Result::started);
            expect (f.engine.isPlaying (f.id (0)));
            expect (! f.engine.isPlaying (f.id (1)));
            expectEquals (f.document.cues.getPlayheadIndex(), 0);
            expectEquals (f.document.cues.getSelectedIndex(), 0);
            f.now += 1.0;
            f.scheduler.tick();
            expect (f.engine.isPlaying (f.id (1)));
            expect (! f.engine.isPlaying (f.id (2)));
            expectEquals (f.document.cues.getPlayheadIndex(), 0);
            expectEquals (f.document.cues.getSelectedIndex(), 0);
        }

        beginTest ("goto inside a sequence overrides designated standby");
        {
            StandbyFixture f (tone, true);
            f.document.cues.update (0, [] (Cue& cue) { cue.continueMode = ContinueMode::autoContinue; });
            // Target a fourth cue so both keeping the original cue and advancing past the sequence are wrong.
            Cue destination;
            destination.file = tone;
            f.document.cues.add (destination);
            f.document.cues.update (1, [&] (Cue& cue)
            {
                cue.type = CueType::control;
                cue.control.kind = ControlKind::gotoCue;
                cue.control.targetId = destination.id;
            });
            f.document.cues.setPlayheadIndex (0);
            expect (f.controller.go() == Result::started);
            expect (f.engine.isPlaying (f.id (0)));
            expect (! f.engine.isPlaying (destination.id));
            expectEquals (f.document.cues.getPlayheadIndex(), 3);
            expectEquals (f.document.cues.getSelectedIndex(), 3);
        }

        beginTest ("missing first file leaves designated standby and the failure status intact");
        {
            StandbyFixture f (tone, true);
            f.document.cues.update (0, [&] (Cue& cue) { cue.file = directory.getChildFile ("missing.wav"); });
            expect (f.controller.go() == Result::failed);
            expectEquals (f.document.cues.getPlayheadIndex(), 0);
            expectEquals (f.document.cues.getSelectedIndex(), 0);
            expectEquals (f.engine.getNumPlaying(), 0);
            expect (f.status.isNotEmpty() && ! f.status.startsWith ("GO: "));
        }

        beginTest ("audition GO and normal GO keep designated standby on the same cue");
        {
            StandbyFixture f (tone, true);
            f.document.settings.audition = WorkspaceSettings::Audition::none;
            expect (f.controller.go (true) == Result::started);
            expect (f.engine.isAuditioning (f.id (0)));
            expectEquals (f.document.cues.getPlayheadIndex(), 0);
            expectEquals (f.document.cues.getSelectedIndex(), 0);
            f.controller.goKeyReleased();
            f.render (2);
            expect (f.controller.go() == Result::started);
            expect (! f.engine.isAuditioning (f.id (0)));
            expect (f.engine.isPlaying (f.id (0)));
            expect (! f.engine.isPlaying (f.id (1)));
            expectEquals (f.document.cues.getPlayheadIndex(), 0);
            expectEquals (f.document.cues.getSelectedIndex(), 0);
        }

        beginTest ("save and reopen preserve both standby modes; legacy projects default to next cue");
        {
            const auto file = directory.getChildFile ("standby.enqueue");
            for (const bool keep : { true, false })
            {
                ProjectDocument document;
                auto settings = document.settings;
                settings.goKeepsStandby = keep;
                document.setSettings (settings);
                expect (document.save (file).wasOk());
                const auto json = juce::JSON::parse (file);
                expect (json["settings"].hasProperty ("goKeepsStandby"));
                expect ((bool) json["settings"]["goKeepsStandby"] == keep);
                ProjectDocument reopened;
                reopened.settings.goKeepsStandby = ! keep;
                expect (reopened.load (file).wasOk());
                expect (reopened.settings.goKeepsStandby == keep);
            }
            auto legacy = juce::JSON::parse (file);
            legacy["settings"].getDynamicObject()->removeProperty ("goKeepsStandby");
            expect (file.replaceWithText (juce::JSON::toString (legacy)));
            ProjectDocument reopened;
            reopened.settings.goKeepsStandby = true;
            expect (reopened.load (file).wasOk());
            expect (! reopened.settings.goKeepsStandby);
        }

        beginTest ("cleanup");
        expect (directory.deleteRecursively());
    }
};

static StandbyAfterGoTests standbyAfterGoTests;
} // namespace gocue::tests
