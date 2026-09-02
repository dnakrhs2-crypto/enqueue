#include "app/CueController.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

class CueControllerTests : public juce::UnitTest
{
public:
    CueControllerTests() : juce::UnitTest ("CueController", "GoCue") {}

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

    static void render (AudioEngine& engine, juce::AudioBuffer<float>& out, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            engine.renderBlock (out, blockSize);

        engine.reapFinishedPlayers();
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

        CueController controller (engine, document);
        double now = 0.0;
        controller.clock = [&now] { return now; };
        int rejected = 0;
        controller.onGoRejected = [&rejected] { ++rejected; };
        juce::StringArray statuses;
        controller.onStatus = [&statuses] (const juce::String& message, bool) { statuses.add (message); };

        beginTest ("GO fires the standby cue and moves the selection on");
        {
            expect (controller.go() == CueController::GoResult::started);
            expect (engine.isPlaying (a.id));
            expectEquals (document.cues.getSelectedIndex(), 1);
            expect (statuses[statuses.size() - 1].startsWith ("GO"));
            controller.goKeyReleased();
            engine.stopAll();
            render (engine, out, 2);
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
            expectEquals (document.cues.getSelectedIndex(), 1);

            now = 10.6;
            expect (! controller.isGoLocked());
            expect (controller.go() == CueController::GoResult::started);
            expect (engine.isPlaying (b.id));
            controller.goKeyReleased();

            settings.doubleGoSeconds = 0.0;
            document.setSettings (settings);
            engine.stopAll();
            render (engine, out, 2);
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
            engine.stopAll();
            render (engine, out, 2);
        }

        beginTest ("P pauses the target and Space resumes it instead of firing the next cue");
        {
            document.cues.setSelectedIndex (0);
            now = 30.0;
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, out, 4);

            expect (controller.togglePause());
            render (engine, out, 2);
            expect (engine.isPaused (a.id));

            now = 31.0;
            expect (controller.go() == CueController::GoResult::resumed);
            controller.goKeyReleased();
            render (engine, out, 2);
            expect (! engine.isPaused (a.id));
            expect (engine.isPlaying (a.id));
            expect (! engine.isPlaying (b.id));
            expectEquals (document.cues.getSelectedIndex(), 1);   // unchanged by the resume

            expect (controller.togglePause());                     // pause again ...
            render (engine, out, 2);
            expect (engine.isPaused (a.id));
            expect (controller.togglePause());                     // ... and P resumes too
            render (engine, out, 2);
            expect (! engine.isPaused (a.id));

            engine.stopAll();
            render (engine, out, 2);
            expect (! controller.togglePause());                   // nothing playing
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
            render (engine, out, 2);
            expectEquals (engine.getNumPlaying(), 2);

            now = 41.0;
            controller.panicAll();
            render (engine, out, 2);
            expect (engine.getPlayingCues()[0].fadingOut);
            expectEquals (engine.getNumPlaying(), 2);              // still fading

            now = 41.2;
            controller.panicAll();                                 // double Esc
            render (engine, out, 2);
            expectEquals (engine.getNumPlaying(), 0);

            settings.panicSeconds = 2.0;
            document.setSettings (settings);
        }

        beginTest ("second-trigger rules: ignore, restart, devamp");
        {
            Cue loop;
            loop.name = "loop";
            loop.file = tone;
            loop.audio.endSeconds = 0.25;
            loop.audio.infiniteLoop = true;
            loop.secondTrigger = SecondTriggerAction::nothing;
            const int index = document.cues.add (loop);
            document.cues.setSelectedIndex (index);

            expect (controller.preview() == CueController::GoResult::started);
            render (engine, out, 10);
            const double before = engine.getPlayingCues()[0].positionSeconds;
            expect (controller.preview() == CueController::GoResult::ignored);
            render (engine, out, 1);
            expectGreaterThan (engine.getPlayingCues()[0].positionSeconds, before);   // not restarted

            document.cues.update (index, [] (Cue& c) { c.secondTrigger = SecondTriggerAction::hardStopRestart; });
            expect (controller.preview() == CueController::GoResult::started);
            render (engine, out, 1);
            expectLessThan (engine.getPlayingCues()[0].positionSeconds, 0.05);     // restarted from the top

            document.cues.update (index, [] (Cue& c) { c.secondTrigger = SecondTriggerAction::devamp; });
            render (engine, out, 30);                                                // well into the loop
            expect (controller.preview() == CueController::GoResult::ignored);
            expectGreaterThan (engine.getPlayingCues()[0].lengthSeconds, 0.0);       // no longer infinite

            for (int i = 0; i < 60 && engine.isPlaying (loop.id); ++i)
                render (engine, out, 1);

            expect (! engine.isPlaying (loop.id));

            document.cues.update (index, [] (Cue& c) { c.secondTrigger = SecondTriggerAction::hardStop; });
            controller.preview();
            render (engine, out, 2);
            expect (controller.preview() == CueController::GoResult::ignored);
            render (engine, out, 2);
            expect (! engine.isPlaying (loop.id));
        }

        beginTest ("reset all stops everything and selects the first cue");
        {
            document.cues.setSelectedIndex (1);
            controller.preview();
            render (engine, out, 2);
            controller.resetAll();
            render (engine, out, 2);
            expectEquals (engine.getNumPlaying(), 0);
            expectEquals (document.cues.getSelectedIndex(), 0);
        }

        expect (dir.deleteRecursively());
    }
};

static CueControllerTests cueControllerTests;

} // namespace gocue::tests
