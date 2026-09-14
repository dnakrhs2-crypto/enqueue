#include "app/CueController.h"
#include "ui/UiUtils.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** 페이드아웃 (F): the project's fade-out time decides how long the target cue takes to go; 0 = the cue's own stop fade. */
class FadeOutSettingTests : public juce::UnitTest
{
public:
    FadeOutSettingTests() : juce::UnitTest ("FadeOutSetting", "Enqueue") {}

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
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("gocue_fadeout_" + juce::Uuid().toString());
        expect (dir.createDirectory().wasOk());
        const auto tone = writeSine (dir, "tone.wav", 4.0);

        AudioEngine engine (0);
        engine.prepare (sampleRate, blockSize);
        juce::AudioBuffer<float> out (2, blockSize);

        ProjectDocument document;
        document.clock = [] { return 0.0; };
        Cue a;
        a.name = "a"; a.file = tone; a.fadeOutMs = 100;   // the cue's own stop fade: short
        document.cues.add (a);
        document.cues.setSelectedIndex (0);

        double now = 0.0;
        Scheduler scheduler ([&now] { return now; });
        CueController controller (engine, document, scheduler);

        beginTest ("the project's fade-out time stretches F beyond the cue's own stop fade");
        {
            auto settings = document.settings;
            settings.fadeOutSeconds = 0.8;
            document.setSettings (settings);
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 20);   // 0.23 s of sound
            expect (controller.fadeOutTarget());
            render (engine, scheduler, now, out, 40);   // 0.46 s into the fade: still there (the cue's own 100 ms would be long over)
            expect (engine.isPlaying (a.id), "F stopped the cue on its own short stop fade instead of the project's time");
            render (engine, scheduler, now, out, 40);   // 0.93 s: over
            expect (! engine.isPlaying (a.id), "the fade-out did not end after the project's time");
            controller.hardStopAll();
            render (engine, scheduler, now, out, 2);
        }

        beginTest ("fade-out time 0 = the cue's own stop fade");
        {
            auto settings = document.settings;
            settings.fadeOutSeconds = 0.0;
            document.setSettings (settings);
            document.cues.setPlayheadIndex (0);
            expect (controller.go() == CueController::GoResult::started);
            controller.goKeyReleased();
            render (engine, scheduler, now, out, 20);
            expect (controller.fadeOutTarget());
            render (engine, scheduler, now, out, 20);   // 0.23 s: the 100 ms stop fade is over
            expect (! engine.isPlaying (a.id), "with 0 the cue's own stop fade must apply");
            controller.hardStopAll();
            render (engine, scheduler, now, out, 2);
        }

        beginTest ("the setting is clamped and defaults to one second");
        {
            WorkspaceSettings s;
            expectWithinAbsoluteError (s.fadeOutSeconds, 1.0, 1.0e-9);
            s.fadeOutSeconds = -3.0;
            s.sanitise();
            expectWithinAbsoluteError (s.fadeOutSeconds, 0.0, 1.0e-9);
            s.fadeOutSeconds = 1.0e9;
            s.sanitise();
            expectWithinAbsoluteError (s.fadeOutSeconds, WorkspaceSettings::maxFadeOutSeconds, 1.0e-9);
            s.fadeOutSeconds = 0.004;   // positive stays positive (and readable): never rounds back to 0 = 큐별
            s.panicSeconds = 0.004;
            s.sanitise();
            expectWithinAbsoluteError (s.fadeOutSeconds, 0.01, 1.0e-9);
            expectWithinAbsoluteError (s.panicSeconds, 0.01, 1.0e-9);
            s.fadeOutSeconds = 0.0;     // 0 is a mode, not a time: it stays 0
            s.panicSeconds = 0.0;
            s.sanitise();
            expectWithinAbsoluteError (s.fadeOutSeconds, 0.0, 1.0e-9);
            expectWithinAbsoluteError (s.panicSeconds, 0.0, 1.0e-9);
        }

        beginTest ("a fade time reads back as it was set, and a typed one is a number or nothing");
        {
            expectEquals (plainSeconds (1.0), juce::String ("1"));
            expectEquals (plainSeconds (0.5), juce::String ("0.5"));
            expectEquals (plainSeconds (0.25), juce::String ("0.25"));
            expectEquals (plainSeconds (10.0), juce::String ("10"));
            expectEquals (plainSeconds (0.0), juce::String ("0"));
            expectEquals (secondsLabel (2.5), ko ("2.5초"));

            for (const char* bad : { "", " ", ".", "abc", "1..5", "1.2.3", "-1", "1e3", "1,5" })
                expect (! parseSeconds (bad).has_value(), juce::String ("'") + bad + "' must not read as a time");

            const auto typed = [] (const char* text) { return parseSeconds (text).value_or (-1.0); };
            expectWithinAbsoluteError (typed (" 1.5 "), 1.5, 1.0e-9);
            expectWithinAbsoluteError (typed (".5"), 0.5, 1.0e-9);
            expectWithinAbsoluteError (typed ("2."), 2.0, 1.0e-9);
            expectWithinAbsoluteError (typed ("0"), 0.0, 1.0e-9);        // 0 stays 0: 큐별 / 즉시 정지
            expectWithinAbsoluteError (typed ("0.004"), 0.01, 1.0e-9);   // a positive value never rounds back to 0
            expectEquals (plainSeconds (typed ("0.004")), juce::String ("0.01"));
        }

        dir.deleteRecursively();
    }
};

static FadeOutSettingTests fadeOutSettingTests;

} // namespace gocue::tests
