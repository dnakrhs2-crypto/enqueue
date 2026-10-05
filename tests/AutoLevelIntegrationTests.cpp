#include "AutoLevelTestSupport.h"
#include "VolumeCueUiTestSupport.h"
#include "ui/AutoLevelDialog.h"
#include "ui/UiUtils.h"
#include "app/ShortcutCatalog.h"
#include <limits>

namespace gocue::tests
{
using namespace auto_level;

namespace
{
Cue micCue()
{
    Cue cue;
    cue.type = CueType::mic;
    cue.mic.numInputs = 2;
    cue.levels.resize (2, 2); cue.levels.setDefaults();
    return cue;
}

struct EngineRig
{
    EngineRig() { engine.prepare (rate, block); }
    void feed (double seconds, double lufs = -26)
    {
        for (int b = 0; b < (int) std::llround (seconds * 100); ++b)
        {
            signal.fill (input, lufs);
            engine.renderBlock (output, block, input.getArrayOfReadPointers(), 2);
            meter.process (output);
            engine.getLoudnessMeter().poll();
        }
    }
    AudioEngine engine { 0 };
    Signal signal;
    Meter meter;
    juce::AudioBuffer<float> input { 2, block }, output { 2, block };
};
}

class AutoLevelEngineTests : public juce::UnitTest
{
public:
    AutoLevelEngineTests() : UnitTest ("AutoLevel engine", "Enqueue") {}
    void runTest() override
    {
        beginTest ("16. offline master gain and screen loudness are after the leveler");
        EngineRig rig;
        const auto cue = micCue();
        expect (rig.engine.play (cue));
        rig.engine.setAutoLevel (true, -16);
        rig.feed (20);
        expect (rig.engine.getAutoLevelGainDb() > 8.0);
        expectWithinAbsoluteError (rig.engine.getLoudnessMeter().getStats().shortTerm().value, rig.meter.lufs(), 0.03);
        logMessage ("engine gain dB = " + juce::String (rig.engine.getAutoLevelGainDb(), 6)
                    + ", screen S = " + juce::String (rig.engine.getLoudnessMeter().getStats().shortTerm().value, 6));

        beginTest ("disabled engine output equals the existing master path bit for bit, also after switching off");
        EngineRig off;
        expect (off.engine.play (cue));
        off.feed (0.1);
        for (int ch = 0; ch < 2; ++ch)
            expect (std::memcmp (off.input.getReadPointer (ch), off.output.getReadPointer (ch), block * sizeof (float)) == 0);
        rig.engine.setAutoLevel (false, -16);
        rig.feed (0.1);
        for (int ch = 0; ch < 2; ++ch)
            expect (std::memcmp (rig.input.getReadPointer (ch), rig.output.getReadPointer (ch), block * sizeof (float)) == 0);

        beginTest ("fading-out player holds a moving fader from its first fade block");
        holdScenario ([] (auto& r, const auto& c) { r.engine.fadeOutAndStop (c.id, 5000); }, 4.9);

        beginTest ("duck/boost and the release ramp hold the moving fader");
        for (const double duck : { -12.0, 6.0 })
        {
            EngineRig r;
            expect (r.engine.play (cue)); r.engine.setAutoLevel (true, -16); r.feed (6);
            const auto before = r.engine.getAutoLevelGainDb();
            r.engine.setDuckDb (cue.id, duck, 0.4); r.feed (1.0);
            expect (r.engine.isAutoLevelHeld());
            const auto letGo = r.engine.getAutoLevelGainDb();
            expect (std::abs (letGo - before) <= 1.0, "a glide, not a ride on");
            r.feed (2.0);
            expectWithinAbsoluteError (r.engine.getAutoLevelGainDb(), letGo, 0.01);
            r.engine.setDuckDb (cue.id, 0.0, 2.0); r.feed (1.9);
            expect (r.engine.isAutoLevelHeld(), "the release ramp is the user's move too");
            expectWithinAbsoluteError (r.engine.getAutoLevelGainDb(), letGo, 0.01);
        }

        beginTest ("a duck released before a new player's first render still holds for that whole ramp block");
        EngineRig firstRamp;
        expect (firstRamp.engine.play (cue)); firstRamp.engine.setAutoLevel (true, -16); firstRamp.feed (6);
        const auto beforeFirstRamp = firstRamp.engine.getAutoLevelGainDb();
        const auto nextCue = micCue();
        AudioEngine::PlayOptions options; options.duckDb = -12;
        expect (firstRamp.engine.play (nextCue, options));
        firstRamp.engine.setDuckDb (nextCue.id, 0.0, 0.001);
        firstRamp.feed (0.01);
        expect (firstRamp.engine.isAutoLevelHeld(), "the release block of a duck the cue started with is held");
        expectWithinAbsoluteError (firstRamp.engine.getAutoLevelGainDb(), beforeFirstRamp, 0.06);   // at most the glide's first 10 ms

        beginTest ("live gain edits and volume/fade cues remain held for minutes, compared with the cue's own gain");
        holdScenario ([] (auto& r, const auto& c) { r.engine.setLiveGainDb (c.id, -12); }, 120);
        EngineRig initial;
        auto initialCue = cue; initialCue.gainDb = -6;
        expect (initial.engine.play (initialCue)); initial.engine.setAutoLevel (true, -16); initial.feed (6);
        expect (initial.engine.getAutoLevelGainDb() > 1.0); // the cue's own starting gain is not an edit
        initial.engine.setLiveGainDb (initialCue.id, -6.04); initial.feed (1);
        const auto withinTolerance = initial.engine.getAutoLevelGainDb();
        initial.engine.setLiveGainDb (initialCue.id, -6.06); initial.feed (1.0);
        const auto letGo = initial.engine.getAutoLevelGainDb();
        expect (std::abs (letGo - withinTolerance) <= 1.0, "a glide, not a ride on");
        initial.feed (2);
        expectWithinAbsoluteError (initial.engine.getAutoLevelGainDb(), letGo, 0.01);

        beginTest ("a fade-in cue holds while it lifts its target and lets go once the target reaches its own level");
        EngineRig lifted;
        const auto target = micCue();   // its own level: 0 dB
        AudioEngine::PlayOptions fromFloor;
        fromFloor.hasStartGain = true;
        fromFloor.startGainDb = -60.0;
        expect (lifted.engine.play (target, fromFloor));
        lifted.engine.setAutoLevel (true, -16);
        for (int i = 1; i <= 20; ++i)
        {
            lifted.engine.setLiveGainDb (target.id, -60.0 + 60.0 * i / 20.0);   // the fade cue's 2 s lift
            lifted.feed (0.1);
        }
        expectWithinAbsoluteError (lifted.engine.getAutoLevelGainDb(), 0.0, 0.01);   // held all through the fade
        lifted.feed (8);
        expect (lifted.engine.getAutoLevelGainDb() > 1.0, "at its own level the cue is ridden like any other");

        beginTest ("panic gate: closing, closed, and a scheduled soft panic freeze gain and leave output silent");
        holdScenario ([] (auto& r, const auto&) { r.engine.closeOutputGate (1000, false); }, 5, true);
        holdScenario ([] (auto& r, const auto&) { r.engine.fadeOutAndStopAll (5000); }, 6, true);

        beginTest ("integrated envelope at the audible position holds; loaded and finished players do not hold another cue");
        volume_ui::Scratch scratch;
        const auto file = scratch.folder.getChildFile ("autolevel.wav");
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
            expect (writer != nullptr);
            if (writer != nullptr)
            {
                Signal source; juce::AudioBuffer<float> b (2, rate); source.fill (b, -26);
                expect (writer->writeFromAudioSampleBuffer (b, 0, rate));
            }
        }
        Cue sound; sound.file = file; sound.durationSeconds = 1; sound.numChannels = 2;
        sound.audio.infiniteLoop = true;
        sound.levels.resize (2, 2); sound.levels.setDefaults();
        EngineRig envelope;
        expect (envelope.engine.play (sound)); envelope.engine.setAutoLevel (true, -16); envelope.feed (6);
        const auto beforeEnvelope = envelope.engine.getAutoLevelGainDb();
        expect (beforeEnvelope > 1.0);
        Envelope shape; shape.enabled = true; shape.points = { { 0, 0.5 }, { 1, 0.1 } };
        envelope.engine.setLiveEnvelope (sound.id, shape); envelope.feed (1.0);
        const auto envelopeLetGo = envelope.engine.getAutoLevelGainDb();
        expect (std::abs (envelopeLetGo - beforeEnvelope) <= 1.0, "a glide, not a ride on");
        envelope.feed (4.0);
        expectWithinAbsoluteError (envelope.engine.getAutoLevelGainDb(), envelopeLetGo, 0.01);
        envelope.engine.setLiveEnvelope (sound.id, {}); envelope.feed (6);
        expect (envelope.engine.getAutoLevelGainDb() > beforeEnvelope + 0.5);
        envelope.engine.stopAll(); envelope.feed (0.1);
        expect (envelope.engine.play (cue)); envelope.feed (6);
        expect (envelope.engine.getAutoLevelGainDb() > beforeEnvelope + 1);

        EngineRig loaded;
        sound.audio.envelope = shape;
        expect (loaded.engine.load (sound)); expect (loaded.engine.play (cue));
        loaded.engine.setAutoLevel (true, -16); loaded.feed (6);
        expect (loaded.engine.getAutoLevelGainDb() > 1);

        beginTest ("legacy cue fade-in is an integrated envelope and never attracts compensation");
        EngineRig fadeIn;
        sound.audio.infiniteLoop = false;
        sound.audio.envelope = Envelope::fromFadeIn (5.0);
        expect (fadeIn.engine.play (sound)); fadeIn.engine.setAutoLevel (true, -16); fadeIn.feed (0.9);
        expectEquals (fadeIn.engine.getAutoLevelGainDb(), 0.0);
    }
private:
    void holdScenario (const std::function<void (EngineRig&, const Cue&)>& change, double seconds, bool silent = false)
    {
        EngineRig rig; const auto cue = micCue();
        expect (rig.engine.play (cue)); rig.engine.setAutoLevel (true, -16); rig.feed (6);
        const auto gain = rig.engine.getAutoLevelGainDb();
        expect (gain > 1.0);
        change (rig, cue); rig.feed (1.0);   // the hand lets go of the moving fader: a short glide, no corner
        const auto letGo = rig.engine.getAutoLevelGainDb();
        expect (std::abs (letGo - gain) <= 1.0, "a glide, not a ride on");
        rig.feed (juce::jmax (0.0, seconds - 1.0));
        expectWithinAbsoluteError (rig.engine.getAutoLevelGainDb(), letGo, 0.01);
        if (silent) expectEquals (rig.output.getMagnitude (0, block), 0.0f);
    }
};

class AutoLevelSettingsTests : public juce::UnitTest
{
public:
    AutoLevelSettingsTests() : UnitTest ("AutoLevel settings and UI", "Enqueue") {}
    void runTest() override
    {
        beginTest ("17. settings save/open round trip and missing-key legacy defaults");
        volume_ui::Scratch scratch;
        Project project; project.settings.autoLevelEnabled = true; project.settings.autoLevelTargetLufs = -21.3;
        const auto file = scratch.folder.getChildFile ("level.enqueue");
        expect (ProjectSerializer::save (project, file).wasOk());
        Project read;
        expect (ProjectSerializer::load (file, read, nullptr).wasOk());
        expect (read.settings.autoLevelEnabled); expectEquals (read.settings.autoLevelTargetLufs, -21.3);
        auto old = ProjectSerializer::toVar (project);
        auto* settings = old.getProperty ("settings", {}).getDynamicObject();
        expect (settings != nullptr);
        if (settings != nullptr)
        {
            settings->removeProperty ("autoLevelEnabled"); settings->removeProperty ("autoLevelTargetLufs");
            settings->setProperty ("unknownFutureSetting", 123);
            expect (ProjectSerializer::fromJson (juce::JSON::toString (old), read).wasOk());
            expect (! read.settings.autoLevelEnabled); expectEquals (read.settings.autoLevelTargetLufs, -16.0);
            for (const auto pair : { std::pair<double, double> { -90, -40 }, { 10, -6 }, { -18.26, -18.3 } })
            {
                settings->setProperty ("autoLevelTargetLufs", pair.first);
                expect (ProjectSerializer::fromJson (juce::JSON::toString (old), read).wasOk());
                expectEquals (read.settings.autoLevelTargetLufs, pair.second);
            }
            for (const auto& invalid : { juce::var(), juce::var ("broken"), juce::var (true) })
            {
                settings->setProperty ("autoLevelTargetLufs", invalid);
                expect (ProjectSerializer::fromJson (juce::JSON::toString (old), read).wasOk());
                expectEquals (read.settings.autoLevelTargetLufs, -16.0);
            }
        }
        for (const double value : { std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
        {
            WorkspaceSettings s; s.autoLevelTargetLufs = value; s.sanitise(); expectEquals (s.autoLevelTargetLufs, -16.0);
        }

        beginTest ("18. menu order, stable catalog ID and exact strings, no default key, enabled tick, show lock");
        volume_ui::Fixture fixture;
        const auto* definition = ShortcutCatalog::get().find ("settings.autoLevel");
        expect (definition != nullptr);
        if (definition != nullptr)
        {
            expectEquals (definition->name, ko ("자동 레벨 맞추기..."));
            expectEquals (definition->description, ko ("마스터로 나가는 소리를 목표 LUFS에 맞춰 천천히 자동 조절 (프로젝트에 저장)"));
            expectEquals (definition->menuCategory, ko ("설정"));
            expect (definition->category == ShortcutCategory::fileSettings && definition->scope == ShortcutScope::mainWindow);
            expect (definition->defaultKeys.isEmpty());
        }
        auto menu = fixture.main->getMenuForIndex (4, ko ("설정"));
        juce::PopupMenu::MenuItemIterator iterator (menu);
        for (const auto id : { CommandIDs::audioSettings, CommandIDs::audioPatches, CommandIDs::autoLevelSettings })
        {
            expect (iterator.next()); expectEquals (iterator.getItem().itemID, (int) id);
        }
        expect (iterator.next()); expect (iterator.getItem().isSeparator);
        auto info = [&fixture]
        {
            juce::ApplicationCommandInfo result (CommandIDs::autoLevelSettings);
            fixture.main->getCommandInfo (CommandIDs::autoLevelSettings, result); return result;
        };
        expect ((info().flags & juce::ApplicationCommandInfo::isDisabled) == 0);
        expect ((info().flags & juce::ApplicationCommandInfo::isTicked) == 0);

        beginTest ("dialog applies segmented buttons and Enter/focus-lost target edits, meter follows injected engine readings");
        double fakeGain = 2.3;
        AutoLevelDialog::Content content (fixture.document(), fixture.engine, [&fakeGain] { return fakeGain; });
        content.setLookAndFeel (&fixture.theme);
        auto* on = volume_ui::child<juce::TextButton> (content, [] (const auto& b) { return b.getButtonText() == ko ("켜기"); });
        auto* off = volume_ui::child<juce::TextButton> (content, [] (const auto& b) { return b.getButtonText() == ko ("끄기"); });
        auto* target = volume_ui::child<juce::TextEditor> (content);
        auto* gain = volume_ui::child<juce::Label> (content, [] (const auto& c) { return c.getComponentID() == "autoLevelGain"; });
        expect (on != nullptr && off != nullptr && target != nullptr && gain != nullptr);
        if (on != nullptr && off != nullptr && target != nullptr && gain != nullptr)
        {
            expectEquals (target->getText(), juce::String ("-16.0"));
            on->onClick();
            expect (fixture.document().settings.autoLevelEnabled); expect (fixture.document().isDirty());
            expect ((info().flags & juce::ApplicationCommandInfo::isTicked) != 0);
            expect (on->getToggleState() && ! off->getToggleState());
            expectEquals (gain->getText(), juce::String ("+2.3 dB"));
            fakeGain = -4.7; content.refreshMeter(); expectEquals (gain->getText(), juce::String ("-4.7 dB"));

            // As drawn: the off segment keeps the segment pair's outline (on the panel-coloured card it would vanish and
            // read as a plain word), and the meter fills from its 0 dB tick to the reading - which way and how far, at a glance.
            const auto sameColour = [] (juce::Colour a, juce::Colour b, int tolerance)
            {
                return std::abs ((int) a.getRed() - (int) b.getRed()) <= tolerance && std::abs ((int) a.getGreen() - (int) b.getGreen()) <= tolerance
                    && std::abs ((int) a.getBlue() - (int) b.getBlue()) <= tolerance;
            };
            const auto snapshot = [&content] { return content.createComponentSnapshot (content.getLocalBounds(), true, 1.0f); };
            const auto edge = snapshot().getPixelAt (off->getX(), off->getBounds().getCentreY());
            expect (! sameColour (edge, Palette::panel, 6), "the off segment's outline is drawn: " + edge.toDisplayString (false));
            auto* correction = volume_ui::child<juce::Label> (content, [] (const auto& c) { return c.getText() == ko ("보정"); });
            expect (correction != nullptr);
            if (correction != nullptr)
            {
                const int left = correction->getRight() + 7, right = gain->getX() - 7, y = correction->getBounds().getCentreY();
                const double width = right - left, zero = left + width * 20.0 / 32.0;
                for (const double reading : { 6.0, -6.0 })
                {
                    fakeGain = reading; content.refreshMeter();
                    const auto image = snapshot();
                    const double end = zero + width * reading / 32.0;
                    const int from = (int) std::ceil (juce::jmin (zero, end)) + 3, to = (int) std::floor (juce::jmax (zero, end)) - 4;
                    int filled = 0;
                    for (int x = from; x <= to; ++x)
                        if (sameColour (image.getPixelAt (x, y), Palette::accent, 40)) ++filled;
                    logMessage ("meter fill between 0 dB and " + juce::String (reading) + " dB: " + juce::String (filled) + " of " + juce::String (to - from + 1) + " px");
                    expect (to > from && filled == to - from + 1, "the meter fills from 0 dB to " + juce::String (reading) + " dB");
                }
                fakeGain = -4.7; content.refreshMeter();
            }
            target->setText ("-99", false); target->onReturnKey(); expectEquals (fixture.document().settings.autoLevelTargetLufs, -40.0);
            target->setText ("-3", false); target->onFocusLost(); expectEquals (fixture.document().settings.autoLevelTargetLufs, -6.0);
            target->setText ("-20.26", false); target->onReturnKey(); expectEquals (target->getText(), juce::String ("-20.3"));
            target->setText ("--20", false); target->onFocusLost(); expectEquals (target->getText(), juce::String ("-20.3"));
            off->onClick(); expect (! fixture.document().settings.autoLevelEnabled);
            expectEquals (gain->getText(), juce::String ("0 dB")); expect (gain->getAlpha() < 1.0f);
            on->onClick();
        }
        expect (fixture.command (CommandIDs::toggleShowMode));
        expect ((info().flags & juce::ApplicationCommandInfo::isDisabled) != 0);
        expect (fixture.command (CommandIDs::autoLevelSettings)); // locked command cannot launch a native window
        expect (fixture.command (CommandIDs::toggleShowMode));

        beginTest ("document notifications apply enabled/target to engine, project adoption and new project apply defaults");
        fixture.engine.prepare (rate, block);
        expect (fixture.engine.play (micCue()));
        Signal source; juce::AudioBuffer<float> input (2, block), output (2, block);
        for (int i = 0; i < 4000; ++i)
        {
            source.fill (input, -30); fixture.engine.renderBlock (output, block, input.getArrayOfReadPointers(), 2);
        }
        expectWithinAbsoluteError (fixture.engine.getAutoLevelGainDb(), 9.7, 0.05); // target entered in the dialog: -20.3 minus -30
        project.settings.autoLevelEnabled = false;
        fixture.document().adopt (std::move (project), file);
        for (int i = 0; i < 4; ++i) fixture.engine.renderBlock (output, block, input.getArrayOfReadPointers(), 2);
        expectEquals (fixture.engine.getAutoLevelGainDb(), 0.0);
        auto s = fixture.document().settings; s.autoLevelEnabled = true; fixture.document().setSettings (s);
        fixture.document().newProject();
        expect (! fixture.document().settings.autoLevelEnabled); expectEquals (fixture.document().settings.autoLevelTargetLufs, -16.0);
        content.setLookAndFeel (nullptr);
    }
};
static AutoLevelEngineTests autoLevelEngineTests;
static AutoLevelSettingsTests autoLevelSettingsTests;
} // namespace gocue::tests
