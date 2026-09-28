#include "app/CueController.h"
#include "app/VolumeCue.h"
#include "app/ShortcutCatalog.h"
#include "model/ProjectSerializer.h"
#include "ui/CueMenuIcons.h"

#include <cmath>

namespace gocue::tests
{
namespace
{
juce::String koVolume (const char* text) { return juce::String::fromUTF8 (text); }

struct VolumeFixture
{
    VolumeFixture() : scheduler ([this] { return now; }), controller (engine, document, scheduler)
    {
        folder.createDirectory();
        const auto file = folder.getChildFile ("tone.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (44100).withNumChannels (2).withBitsPerSample (16));
        if (writer != nullptr)
        {
            juce::AudioBuffer<float> data (2, 44100);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < data.getNumSamples(); ++i)
                    data.setSample (ch, i, 0.25f * (float) std::sin (i * 0.02));
            writer->writeFromAudioSampleBuffer (data, 0, data.getNumSamples());
        }
        writer.reset();
        sound.file = file;
        sound.name = "Music";
        sound.number = "1";
        sound.gainDb = -3.0;
        sound.audio.infiniteLoop = true;
        sound.levels.resize (2, 2);
        sound.levels.setDefaults();
        volume.type = CueType::fade;
        volume.name = koVolume ("볼륨: Music");
        volume.number = "2";
        volume.fade.mode = FadeMode::volume;
        volume.fade.targetId = sound.id;
        volume.fade.durationSeconds = 2.0;
        volume.fade.mainDb = -6.0;
        volume.fade.curve.shape = CurveShape::parametric;
        volume.fade.curve.domain = AudioDomain::decibel;
        volume.fade.curve.intensity = 2.0;
        document.cues.add (sound);
        document.cues.add (volume);
        document.settings.doubleGoSeconds = 0;
        engine.prepare (44100, 512);
        controller.clock = [this] { return now; };
        controller.getFadeRunner().clock = [this] { return now; };
    }
    ~VolumeFixture()
    {
        engine.shutdown();
        if (folder.isAChildOf (base) && folder.getFileName().startsWith ("EnqueueVolumeCue-")) folder.deleteRecursively();
    }
    double level()
    {
        AudioEngine::LiveState live;
        return engine.getLiveState (sound.id, live) ? live.gainDb : 999.0;
    }
    void step (double seconds) { now += seconds; controller.getFadeRunner().tick(); }
    void render()
    {
        juce::AudioBuffer<float> block (2, 512);
        for (int i = 0; i < 8; ++i) engine.renderBlock (block, 512);
        engine.reapFinishedPlayers();
    }
    const juce::File base = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File folder = base.getChildFile ("EnqueueVolumeCue-" + juce::Uuid().toString());
    AudioEngine engine { 0 };
    ProjectDocument document;
    Scheduler scheduler;
    CueController controller;
    Cue sound, volume;
    double now = 10.0;
};

class VolumeCueRunnerTests : public juce::UnitTest
{
public:
    VolumeCueRunnerTests() : UnitTest ("VolumeCueRunner", "Enqueue") {}
    void runTest() override
    {
        VolumeFixture f;
        auto& fadeRunner = f.controller.getFadeRunner();
        beginTest ("own gain -3 plus offset -6 follows the curve without stopping or restarting");
        expect (f.engine.play (f.sound));
        const auto order = f.engine.getStartOrder (f.sound.id);
        // Poison every legacy option: volume must exclusively take the main lane.
        auto volume = f.volume;
        volume.fade.relative = true;
        volume.fade.stopTargetWhenDone = true;
        volume.fade.fadeLevels = false;
        volume.fade.fadeRate = true;
        volume.fade.rate = 2.0;
        volume.fade.levels.resize (2, 2);
        volume.fade.resizeActive (2, 2);
        volume.fade.setAllActive (true);
        volume.fade.params.push_back ({ 0, 0, 1.0f, true });
        expect (fadeRunner.start (volume));
        f.step (0.5);
        expectWithinAbsoluteError (f.level(), -3.375, 1.0e-6); // (-6) * (0.25 squared)
        f.step (1.5);
        expectWithinAbsoluteError (f.level(), -9.0, 1.0e-6);
        expect (f.engine.isPlaying (f.sound.id) && ! f.engine.isStopping (f.sound.id));
        expectEquals (f.engine.getStartOrder (f.sound.id), order);
        AudioEngine::LiveState live;
        expect (f.engine.getLiveState (f.sound.id, live));
        expectWithinAbsoluteError (live.rate, 1.0, 1.0e-9);
        expectWithinAbsoluteError (live.levels.crosspointDb[0][0], 0.0, 1.0e-9);
        const auto snapshot = f.engine.getPlayingCues();
        expect (! snapshot.empty());
        if (! snapshot.empty()) expectWithinAbsoluteError (snapshot.back().liveGainDb, -9.0, 1.0e-6);

        beginTest ("repeat is idempotent; zero restores own gain; positive offset boosts; revert restores");
        expect (fadeRunner.start (volume)); f.step (2.0);
        expectWithinAbsoluteError (f.level(), -9.0, 1.0e-6);
        volume.fade.mainDb = 0.0;
        expect (fadeRunner.start (volume)); f.step (2.0);
        expectWithinAbsoluteError (f.level(), -3.0, 1.0e-6);
        volume.fade.mainDb = 3.0;
        expect (fadeRunner.start (volume)); f.step (2.0);
        expectWithinAbsoluteError (f.level(), 0.0, 1.0e-6);
        expect (fadeRunner.revertLast());
        expectWithinAbsoluteError (f.level(), -3.0, 1.0e-6);
        expectEquals (f.engine.getStartOrder (f.sound.id), order);

        beginTest ("volume, fade-in, fade-out and custom hand off at the current main level");
        for (const auto mode : { FadeMode::volume, FadeMode::fadeIn, FadeMode::fadeOut, FadeMode::custom })
        {
            f.engine.setLiveGainDb (f.sound.id, -3.0);
            expect (fadeRunner.start (f.volume)); f.step (0.5);
            const double before = f.level();
            auto next = f.volume.duplicated(); next.fade.mode = mode; next.fade.mainDb = -12.0;
            expect (fadeRunner.start (next));
            expectWithinAbsoluteError (f.level(), before, 1.0e-6);
            expect (! fadeRunner.isRunning (f.volume.id));
            f.step (0.25);
            const double handoff = f.level();
            expect (fadeRunner.start (f.volume));
            expectWithinAbsoluteError (f.level(), handoff, 1.0e-6);
            f.step (2.0);
            expectWithinAbsoluteError (f.level(), -9.0, 1.0e-6);
            expect (f.engine.isPlaying (f.sound.id));
        }

        beginTest ("target restart ends the old fade and its revert cannot touch the new instance");
        fadeRunner.resetSession();
        expect (fadeRunner.start (f.volume)); f.step (0.5);
        expect (f.engine.play (f.sound));
        expect (f.engine.getStartOrder (f.sound.id) != order);
        f.step (3.0);
        expect (! fadeRunner.isRunning (f.volume.id));
        expectWithinAbsoluteError (f.level(), -3.0, 1.0e-6);
        expect (! fadeRunner.revertLast());

        beginTest ("volume targets clamp at the cue gain limits");
        for (const double offset : { -120.0, 24.0 })
        {
            f.document.cues.update (0, [offset] (Cue& cue) { cue.gainDb = offset < 0 ? -24.0 : 24.0; });
            volume.fade.mainDb = offset;
            expect (fadeRunner.start (volume)); f.step (2.0);
            expectWithinAbsoluteError (f.level(), offset < 0 ? Cue::minGainDb : Cue::maxGainDb, 1.0e-6);
        }
    }
};

class VolumeCueControllerTests : public juce::UnitTest
{
public:
    VolumeCueControllerTests() : UnitTest ("VolumeCueController", "Enqueue") {}
    void runTest() override
    {
        VolumeFixture f;
        using Result = CueController::GoResult;
        juce::String status;
        bool error = true;
        f.controller.onStatus = [&] (const auto& text, bool isError) { status = text; error = isError; };
        beginTest ("stopped and loaded targets skip with failed and a normal status, without auto-start");
        expect (f.controller.trigger (f.volume) == Result::failed);
        expect (! f.engine.isPlaying (f.sound.id));
        expect (status.startsWith (koVolume ("볼륨 큐 대상이 재생 중이 아니라 건너뜀: ")) && status.contains (f.volume.name));
        expect (! error);
        expect (f.engine.load (f.sound));
        expect (f.controller.trigger (f.volume) == Result::failed);
        expect (f.engine.isLoaded (f.sound.id) && ! f.engine.isPlaying (f.sound.id));

        beginTest ("paused target changes level while staying paused on the same instance");
        expect (f.engine.play (f.sound)); f.engine.pause (f.sound.id); f.render();
        expect (f.engine.isPaused (f.sound.id), "pause ramp completes before the volume cue fires");
        const auto order = f.engine.getStartOrder (f.sound.id);
        expect (f.controller.trigger (f.volume) == Result::started); f.step (2.0);
        expectWithinAbsoluteError (f.level(), -9.0, 1.0e-6);
        expect (f.engine.isPaused (f.sound.id));
        expectEquals (f.engine.getStartOrder (f.sound.id), order);

        beginTest ("GO keeps the Space rule: a paused cue is resumed first, the volume cue at the playhead fires on the next GO");
        f.engine.setLiveGainDb (f.sound.id, -3.0);
        f.document.cues.setPlayheadIndex (1);
        expect (f.controller.go() == Result::resumed);
        f.controller.goKeyReleased();
        f.render();
        expect (! f.engine.isPaused (f.sound.id) && f.engine.isPlaying (f.sound.id));
        expectEquals (f.document.cues.getPlayheadIndex(), 1, "the resume must not use up the volume cue's GO");
        expectWithinAbsoluteError (f.level(), -3.0, 1.0e-6);
        expect (f.controller.go() == Result::started);
        f.controller.goKeyReleased();
        f.step (2.0);
        expectWithinAbsoluteError (f.level(), -9.0, 1.0e-6);
        expect (f.engine.isPlaying (f.sound.id) && ! f.engine.isPaused (f.sound.id));
        expectEquals (f.engine.getStartOrder (f.sound.id), order);

        beginTest ("stopping target is left alone");
        f.engine.resume (f.sound.id);
        f.engine.fadeOutAndStop (f.sound.id, 2000);
        expect (f.engine.isStopping (f.sound.id));
        expect (f.controller.trigger (f.volume) == Result::failed);
        expect (! error);
        expectEquals (f.engine.getStartOrder (f.sound.id), order);

        beginTest ("auditioned volume cue and an auditioning target are both refused");
        expect (f.engine.play (f.sound));
        expect (f.controller.trigger (f.volume, true) == Result::failed);
        expect (error);
        AudioEngine::PlayOptions options; options.audition = true;
        expect (f.engine.play (f.sound, options));
        expect (f.controller.trigger (f.volume) == Result::failed);
        expect (error && status.contains (koVolume ("미리듣기")));

        beginTest ("a failed volume cue does not stop the auto-continue sequence");
        f.engine.stopAll(); f.render();
        f.document.cues.update (1, [] (Cue& cue) { cue.continueMode = ContinueMode::autoContinue; });
        auto next = f.sound.duplicated();
        f.document.cues.add (next);
        f.document.cues.setPlayheadIndex (1);
        expect (f.controller.go() == Result::failed);
        f.scheduler.tick();
        expect (f.controller.getFirstTriggerResult() == Result::failed);
        expect (f.engine.isPlaying (next.id));
        expect (! f.engine.isPlaying (f.sound.id));

        beginTest ("a volume cue without a target reports the missing target (an error), not a skip");
        auto orphan = f.volume.duplicated();
        orphan.fade.targetId = juce::Uuid::null();
        expect (f.controller.trigger (orphan) == Result::failed);
        expect (error);
        expect (! status.contains (koVolume ("건너뜀")));
        expect (! f.engine.isPlaying (f.sound.id));
    }
};

class VolumeCueSerializationTests : public juce::UnitTest
{
public:
    VolumeCueSerializationTests() : UnitTest ("VolumeCueSerialization", "Enqueue") {}
    void runTest() override
    {
        beginTest ("volume round trip retains offset, duration and custom curve, normalises legacy fields");
        Project project;
        Cue cue; cue.type = CueType::fade; cue.fade.mode = FadeMode::volume;
        cue.fade.targetId = juce::Uuid(); cue.fade.mainDb = -4.5; cue.fade.durationSeconds = 3.25;
        cue.fade.curve.shape = CurveShape::custom; cue.fade.curve.domain = AudioDomain::decibel;
        cue.fade.curve.points = { { 0, 0 }, { 0.3, 0.1 }, { 1, 1 } };
        cue.fade.relative = true; cue.fade.stopTargetWhenDone = true; cue.fade.fadeLevels = false;
        cue.fade.fadeRate = true; cue.fade.params.push_back ({ 0, 0, 0.5f, true });
        cue.fade.levels.resize (2, 2); cue.fade.resizeActive (2, 2); cue.fade.setAllActive (true);
        project.cues().push_back (cue);
        const auto json = ProjectSerializer::toJson (project);
        expect (json.contains ("\"volume\""));
        Project loaded; juce::StringArray warnings;
        expect (ProjectSerializer::fromJson (json, loaded, &warnings).wasOk());
        expect (warnings.isEmpty());
        if (loaded.cues().empty()) { expect (false); return; }
        const auto& fade = loaded.cues()[0].fade;
        expect (fade.mode == FadeMode::volume && fade.targetId == cue.fade.targetId);
        expectWithinAbsoluteError (fade.mainDb, -4.5, 1.0e-9);
        expectWithinAbsoluteError (fade.durationSeconds, 3.25, 1.0e-9);
        expect (fade.curve == cue.fade.curve);
        expect (! fade.relative && ! fade.stopTargetWhenDone && fade.fadeLevels && fade.mainActive && ! fade.fadeRate && fade.params.empty());
        for (const auto flag : fade.inputActive) expect (flag == 0);
        for (const auto flag : fade.outputActive) expect (flag == 0);
        for (const auto& row : fade.crosspointActive) for (const auto flag : row) expect (flag == 0);
        expect (project.cues()[0].fade.relative && project.cues()[0].fade.params.size() == 1); // saving never mutates the model

        beginTest ("custom, in and out preserve their existing fields and missing mode remains custom");
        for (const auto mode : { FadeMode::custom, FadeMode::fadeIn, FadeMode::fadeOut })
        {
            project.cues()[0].fade.mode = mode;
            expect (ProjectSerializer::fromJson (ProjectSerializer::toJson (project), loaded).wasOk());
            const auto& old = loaded.cues()[0].fade;
            expect (old.mode == mode && old.relative && old.stopTargetWhenDone && ! old.fadeLevels && old.fadeRate);
            expect (old.params.size() == 1 && old.isInputActive (0) && old.isCrosspointActive (0, 0));
        }
        auto root = ProjectSerializer::toVar (project);
        auto fadeVar = root["lists"][0]["cues"][0]["fade"];
        expect (fadeVar.getDynamicObject() != nullptr);
        if (auto* object = fadeVar.getDynamicObject())
        {
            object->removeProperty ("mode");
            expect (ProjectSerializer::fromJson (juce::JSON::toString (root), loaded).wasOk());
            expect (loaded.cues()[0].fade.mode == FadeMode::custom);
        }
    }
};

class VolumeCuePresentationTests : public juce::UnitTest
{
public:
    VolumeCuePresentationTests() : UnitTest ("VolumeCuePresentation", "Enqueue") {}
    void runTest() override
    {
        beginTest ("file text, signed decimals and input validation");
        Cue sound; sound.number = "1"; sound.name = "Music"; sound.gainDb = -3.0;
        const double values[] { -6, 0, 3, -4.5 };
        const char* labels[] { "-6 dB", "원래 볼륨", "+3 dB", "-4.5 dB" };
        const char* inputs[] { "-6", "0", "+3", "-4.5" };
        for (int i = 0; i < 4; ++i)
        {
            expectEquals (VolumeCue::targetText (&sound, values[i]), koVolume ("→ 1 Music · ") + koVolume (labels[i]));
            expect (VolumeCue::parseOffset (inputs[i]) == values[i]);
        }
        for (const auto* text : { "", "text", "+", "--3", "3x", "1.2.3", "nan", "inf" }) expect (! VolumeCue::parseOffset (text));
        expect (VolumeCue::parseOffset ("-999") == Cue::minGainDb);
        expect (VolumeCue::parseOffset ("999") == Cue::maxGainDb);

        beginTest ("badge thresholds, non-volume fade suppression, moving values, silence, audio and mic");
        AudioEngine::PlayingCue p; p.id = sound.id; p.liveGainDb = -3.04;
        expect (VolumeCue::badgeText (sound, p, false).isEmpty());
        p.liveGainDb = -5.4;
        expectEquals (VolumeCue::badgeText (sound, p, false), koVolume ("볼륨 -2.4 dB"));
        expect (VolumeCue::badgeText (sound, p, true).isEmpty());
        p.liveGainDb = -3.05; expect (VolumeCue::badgeText (sound, p, false).isNotEmpty());
        p.fadingOut = true; expect (VolumeCue::badgeText (sound, p, false).isEmpty()); p.fadingOut = false;
        p.loaded = true; expect (VolumeCue::badgeText (sound, p, false).isEmpty()); p.loaded = false;
        p.liveGainDb = Cue::minGainDb; expectEquals (VolumeCue::badgeText (sound, p, false), koVolume ("볼륨 무음"));
        sound.type = CueType::mic; p.liveGainDb = 0;
        expectEquals (VolumeCue::badgeText (sound, p, false), koVolume ("볼륨 +3 dB"));
        Cue fade; fade.type = CueType::fade; fade.fade.targetId = sound.id;
        const auto lookup = [&] (const juce::Uuid& id) -> const Cue* { return id == sound.id ? &sound : id == fade.id ? &fade : nullptr; };
        for (const auto mode : { FadeMode::fadeIn, FadeMode::fadeOut, FadeMode::custom, FadeMode::volume })
        {
            fade.fade.mode = mode;
            const auto badges = VolumeCue::badgesFor ({ p }, { { fade.id, sound.id, 0.5, 2.0 } }, lookup);
            expect (badges.empty() == (mode != FadeMode::volume));
        }
        auto retiring = p; retiring.startOrder = -1; retiring.fadingOut = true;
        const auto badges = VolumeCue::badgesFor ({ retiring, p }, {}, lookup);
        expectEquals ((int) badges.size(), 1);
        expect (badges.count ({ sound.id, p.startOrder }) == 1);

        beginTest ("volume icon registry and command adapter share distinct geometry; new shortcut has no conflicts");
        const auto* path = CueIcons::pathFor ({ CueType::fade, FadeMode::volume });
        auto icon = CueMenuIcons::create (CommandIDs::addVolumeCue);
        auto* drawable = dynamic_cast<juce::DrawablePath*> (icon.get());
        expect (path != nullptr && drawable != nullptr);
        if (path != nullptr && drawable != nullptr)
        {
            expect (! path->isEmpty() && drawable->getPath() == *path);
            expect (juce::Rectangle<float> (0, 0, 24, 24).contains (path->getBounds()));
            for (const auto mode : { FadeMode::fadeIn, FadeMode::fadeOut, FadeMode::custom })
                expect (*path != *CueIcons::pathFor ({ CueType::fade, mode }));
        }
        const auto& catalog = ShortcutCatalog::get();
        const auto* command = catalog.find (CommandIDs::addVolumeCue);
        expect (command != nullptr && command->id == "cue.addVolume");
        if (command != nullptr)
        {
            const juce::KeyPress key ('7', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier, 0);
            expect (command->defaultKeys == ShortcutKeys { key });
            for (const auto& other : catalog.getCommands()) if (other.commandID != command->commandID) expect (! other.defaultKeys.contains (key));
            for (const auto& other : catalog.getFixedComponents()) expect (! other.defaultKeys.contains (key));
        }
        expect (catalog.find (CommandIDs::showActiveCuesWindow)->defaultKeys.isEmpty());
    }
};
static VolumeCueRunnerTests volumeCueRunnerTests;
static VolumeCueControllerTests volumeCueControllerTests;
static VolumeCueSerializationTests volumeCueSerializationTests;
static VolumeCuePresentationTests volumeCuePresentationTests;
}
}
