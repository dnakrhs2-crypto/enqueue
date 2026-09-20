#include "app/BackupManager.h"
#include "app/Commands.h"
#include "ui/MainComponent.h"
#include "ui/GoCueLookAndFeel.h"
#include "TestGainPlugin.h"

#include <cmath>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

// Targeted reproductions from AuditE3Tests.cpp and AuditE5Tests.cpp.
// Keep their setup and required-behaviour assertions unchanged.
namespace gocue::tests::auditFixUndoE3
{
namespace
{
constexpr double sampleRate = 44100.0;
constexpr int blockSize = 512;

template <typename T, typename Predicate>
T* findChild (juce::Component& root, Predicate predicate)
{
    if (auto* found = dynamic_cast<T*> (&root); found != nullptr && predicate (*found))
        return found;
    for (auto* child : root.getChildren())
        if (auto* found = findChild<T> (*child, predicate))
            return found;
    return nullptr;
}

template <typename T> T* findChild (juce::Component& root)
{
    return findChild<T> (root, [] (const T&) { return true; });
}

// The Windows message pump and MainComponent ownership order are the same as
// ReopenLastProjectTests / MidiMainComponentTests. No private-access macros or
// replacement MainComponent/ProjectDocument implementations are used here.
void drainMessages()
{
   #if JUCE_WINDOWS
    MSG message {};
    for (int count = 0; count < 1000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++count)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
   #endif
}

struct ScratchDirectory
{
    ScratchDirectory() { folder.createDirectory(); }
    ~ScratchDirectory()
    {
        if (folder.isAChildOf (base) && folder.getFileName().startsWith ("AuditE3-"))
            folder.deleteRecursively();
    }
    const juce::File base = juce::File::getCurrentWorkingDirectory().getChildFile ("build");
    const juce::File folder = base.getChildFile ("AuditE3-" + juce::Uuid().toString());
};

struct Fixture
{
    Fixture() : storage (scratch.folder.getChildFile ("test.settings"), options()), settings (storage)
    {
        engine.prepare (sampleRate, blockSize);
        main = std::make_unique<MainComponent> (engine, settings, commands);
        main->setLookAndFeel (&theme);
    }
    ~Fixture()
    {
        main.reset();
        drainMessages();
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        drainMessages();
        engine.shutdown();
        storage.saveIfNeeded();
    }
    static juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options result;
        result.storageFormat = juce::PropertiesFile::storeAsXML;
        result.millisecondsBeforeSaving = -1;
        return result;
    }
    bool open (const Project& project)
    {
        if (ProjectSerializer::save (project, file).failed())
            return false;
        main->openProjectFile (file, false);
        auto* transport = findChild<TransportBar> (*main);
        if (transport != nullptr)
            status = findChild<juce::Label> (*transport, [this] (const juce::Label& label)
            {
                return label.getText() == ko ("열림: ") + file.getFileName();
            });
        // Complete the project-open stop/reset through the real offline callback.
        juce::AudioBuffer<float> silence (2, blockSize);
        for (int i = 0; i < 4; ++i)
            engine.renderBlock (silence, blockSize);
        engine.reapIfNeeded();
        return main->getProjectFile() == file && status != nullptr;
    }
    bool command (juce::CommandID id)
    {
        return main->perform (juce::ApplicationCommandTarget::InvocationInfo (id));
    }
    juce::String statusText() const { return status != nullptr ? status->getText() : "<no status label>"; }

    ScratchDirectory scratch;
    const juce::File file = scratch.folder.getChildFile ("show.enqueue");
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine { 0 }; // RegionPlaybackTests: synchronous, deterministic disk reads.
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    juce::Component::SafePointer<juce::Label> status;
};

Project blankProject()
{
    Project project;
    project.name = "Audit E3";
    project.ensureMainList();
    project.settings.autoBackup = false;
    project.settings.backupBeforeSave = false;
    return project;
}

bool writeSine (const juce::File& file, double frequency)
{
    if (file.getParentDirectory().createDirectory().failed())
        return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                  .withSampleRate (sampleRate)
                                                  .withNumChannels (2)
                                                  .withBitsPerSample (16));
    if (writer == nullptr)
        return false;
    const int samples = (int) (12.0 * sampleRate);
    juce::AudioBuffer<float> buffer (2, samples);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < samples; ++i)
            buffer.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate));
    return writer->writeFromAudioSampleBuffer (buffer, 0, samples);
}

Cue audioCue (const juce::File& file)
{
    Cue cue;
    cue.file = file;
    cue.name = "Audit tone";
    cue.number = "1";
    cue.durationSeconds = 12.0;
    cue.numChannels = 2;
    return cue;
}

struct Measurement { double rms = 0.0, frequency = 0.0; };

Measurement render (AudioEngine& engine)
{
    juce::AudioBuffer<float> block (2, blockSize);
    for (int i = 0; i < 8; ++i) // settle gain ramps and the resampler's pre-read samples
        engine.renderBlock (block, blockSize);
    double sum = 0.0, firstCrossing = 0.0, lastCrossing = 0.0;
    int crossings = 0, sample = 0;
    float previous = 0.0f;
    for (int b = 0; b < 16; ++b)
    {
        engine.renderBlock (block, blockSize);
        for (int i = 0; i < blockSize; ++i, ++sample)
        {
            const float value = block.getSample (0, i);
            sum += (double) value * value;
            if (sample > 0 && previous <= 0.0f && value > 0.0f)
            {
                const double crossing = sample - 1.0 - previous / (double) (value - previous);
                if (crossings++ == 0)
                    firstCrossing = crossing;
                lastCrossing = crossing;
            }
            previous = value;
        }
    }
    return { std::sqrt (sum / sample), crossings > 1 ? (crossings - 1) * sampleRate / (lastCrossing - firstCrossing) : 0.0 };
}

}

class AuditFixUndoE3Tests : public juce::UnitTest
{
public:
    AuditFixUndoE3Tests() : UnitTest ("AuditFixUndo E3", "Enqueue") {}
    void runTest() override
    {
        testFileUndo();
        testEnvelopeUndo();
        testFirstBackupMove();
    }

private:
    bool require (bool condition, const juce::String& message)
    {
        expect (condition, "SETUP: " + message);
        return condition;
    }

    void testFileUndo()
    {
        beginTest ("audit E3-1: undo file replacement restores the next GO audio");
        Fixture f;
        const auto a = f.scratch.folder.getChildFile ("A.wav");
        const auto b = f.scratch.folder.getChildFile ("B.wav");
        if (! require (writeSine (a, 440.0) && writeSine (b, 880.0), "create equal-length 440/880 Hz files")) return;
        auto project = blankProject();
        const auto cue = audioCue (a);
        project.cues().push_back (cue);
        if (! require (f.open (project), "open A in the real MainComponent")) return;
        auto* inspector = findChild<CueInspector> (*f.main);
        auto* drop = inspector != nullptr ? findChild<juce::FileDragAndDropTarget> (*inspector) : nullptr;
        if (! require (drop != nullptr, "find the inspector's actual file replacement target")) return;
        const auto showsFile = [inspector] (const juce::File& file)
        {
            return findChild<juce::Label> (*inspector, [&file] (const juce::Label& label)
            { return label.getText() == file.getFullPathName(); }) != nullptr;
        };
        if (! require (showsFile (a), "inspector initially displays A")) return;
        if (! require (f.command (CommandIDs::loadCue) && f.engine.isLoaded (cue.id), "LOAD A")) return;
        const juce::StringArray paths { b.getFullPathName() };
        if (! require (drop->isInterestedInFileDrag (paths), "inspector accepts B")) return;
        drop->filesDropped (paths, 0, 0); // same replaceFile() used by the inspector's file chooser
        if (! require (showsFile (b) && f.engine.isLoaded (cue.id), "B replaces the loaded cue")) return;
        if (! require (f.command (CommandIDs::undo) && showsFile (a), "UNDO restores the displayed file to A")) return;
        if (! require (f.command (CommandIDs::go) && f.engine.isPlaying (cue.id), "GO starts the restored cue")) return;
        const auto heard = render (f.engine);
        // Inspect the real document through its normal save output AFTER the measured GO.
        if (! require (f.command (CommandIDs::saveProject), "serialize the restored document")) return;
        Project disk;
        if (! require (ProjectSerializer::load (f.file, disk).wasOk() && disk.cues().size() == 1, "read back document")) return;
        logMessage ("OBSERVED E3-1: document=" + disk.cues()[0].file.getFileName()
                    + "; UI=A.wav; outputHz=" + juce::String (heard.frequency, 3) + "; RMS=" + juce::String (heard.rms, 6));
        expectEquals (disk.cues()[0].file.getFullPathName(), a.getFullPathName());
        expectGreaterThan (heard.rms, 0.3);
        expectWithinAbsoluteError (heard.frequency, 440.0, 1.0, "UNDO must make the next GO play A (440 Hz)");
    }

    void testEnvelopeUndo()
    {
        beginTest ("audit E3-2: live envelope undo and redo restore the audible level");
        Fixture f;
        const auto file = f.scratch.folder.getChildFile ("envelope.wav");
        if (! require (writeSine (file, 440.0), "create long envelope test audio")) return;
        auto cue = audioCue (file);
        cue.audio.envelope.enabled = true;
        cue.audio.envelope.linear = true;
        cue.audio.envelope.lockToTrim = true;
        cue.audio.envelope.points = { { 0.0, 0.2 }, { 1.0, 0.2 } };
        auto project = blankProject();
        project.cues().push_back (cue);
        if (! require (f.open (project), "open the attenuated cue")) return;
        auto* inspector = findChild<CueInspector> (*f.main);
        if (! require (inspector != nullptr, "find inspector")) return;
        inspector->showTimeTab();
        auto* panel = findChild<TimeLoopsPanel> (*inspector);
        auto* toggle = panel != nullptr ? findChild<juce::ToggleButton> (*panel, [] (const juce::ToggleButton& button)
        { return button.getButtonText() == ko ("사용"); }) : nullptr;
        if (! require (toggle != nullptr && toggle->getToggleState(), "real envelope use toggle starts enabled")) return;
        if (! require (f.command (CommandIDs::go) && f.engine.isPlaying (cue.id), "GO starts the attenuated cue")) return;
        const auto low = render (f.engine);
        if (! require (std::abs (low.rms - 0.5 / std::sqrt (2.0) * 0.2) < 0.003, "initial envelope audibly attenuates to 20%")) return;
        toggle->setToggleState (false, juce::dontSendNotification);
        toggle->onClick(); // actual TimeLoopsPanel::updateSelected + setLiveEnvelope
        const auto off = render (f.engine);
        if (! require (! toggle->getToggleState() && std::abs (off.rms - 0.5 / std::sqrt (2.0)) < 0.003, "disabling envelope is heard live")) return;
        if (! require (f.command (CommandIDs::undo) && toggle->getToggleState(), "UNDO restores the enabled UI toggle")) return;
        const auto undone = render (f.engine);
        if (! require (f.command (CommandIDs::redo) && ! toggle->getToggleState(), "REDO restores the disabled UI toggle")) return;
        const auto redone = render (f.engine);
        if (! require (f.command (CommandIDs::undo) && toggle->getToggleState(), "second UNDO restores enabled UI")) return;
        const auto undoneAgain = render (f.engine);
        if (! require (f.command (CommandIDs::saveProject), "serialize the restored envelope")) return;
        Project disk;
        if (! require (ProjectSerializer::load (f.file, disk).wasOk() && disk.cues().size() == 1, "read back envelope document")) return;
        const auto& envelope = disk.cues()[0].audio.envelope;
        logMessage ("OBSERVED E3-2: RMS enabled=" + juce::String (low.rms, 6) + "; disabled=" + juce::String (off.rms, 6)
                    + "; undo=" + juce::String (undone.rms, 6) + "; redo=" + juce::String (redone.rms, 6)
                    + "; undoAgain=" + juce::String (undoneAgain.rms, 6) + "; UI/document enabled=" + juce::String ((int) envelope.enabled));
        expect (envelope.enabled && envelope.points.size() == 2);
        if (envelope.points.size() == 2)
        {
            expectWithinAbsoluteError (envelope.points[0].level, 0.2, 1e-6);
            expectWithinAbsoluteError (envelope.points[1].level, 0.2, 1e-6);
        }
        expectWithinAbsoluteError (undone.rms, low.rms, 0.003, "UNDO must restore the quiet envelope in the running player");
        expectWithinAbsoluteError (redone.rms, off.rms, 0.003, "REDO must restore the unattenuated output");
        expectWithinAbsoluteError (undoneAgain.rms, low.rms, 0.003, "a second UNDO must also restore the audible level");
    }

    void testFirstBackupMove()
    {
        beginTest ("audit E3-4: first automatic backup resolves accompanying audio after a folder move");
        ScratchDirectory scratch;
        // Absent directory is the proposed failure; pre-existing directory is the
        // control. Both use the exact snapshot writer called by autoBackupIfDue().
        for (const bool preExisting : { false, true })
        {
            const juce::String kind = preExisting ? "existing-directory control" : "first-backup directory";
            const auto original = scratch.folder.getChildFile (preExisting ? "control-original" : "first-original");
            const auto moved = scratch.folder.getChildFile (preExisting ? "control-moved" : "first-moved");
            const auto file = original.getChildFile ("show.enqueue");
            if (! require (original.createDirectory().wasOk(), kind + ": create original project folder")) continue;
            ProjectDocument document;
            if (! require (document.save (file).wasOk(), kind + ": first save of a new empty project")) continue;
            const auto tone = original.getChildFile ("audio").getChildFile ("tone.wav");
            if (! require (writeSine (tone, 440.0), kind + ": accompanying audio exists")) continue;
            document.perform ("Add audio after first save", [&] { document.cues.add (audioCue (tone)); });
            if (! require (document.isDirty(), kind + ": added audio is unsaved")) continue;
            const auto dir = BackupManager::backupDirFor (file);
            if (! require (! dir.exists(), kind + ": no earlier pre-save backup")) continue;
            if (preExisting && ! require (dir.createDirectory().wasOk(), "create control backup directory")) continue;
            const auto target = BackupManager::makeUniqueBackupFile (file, juce::Time::getCurrentTime());
            if (! require (target.getParentDirectory().isDirectory() == preExisting, kind + ": target generation did not create a directory")) continue;
            // MainComponent::autoBackupIfDue: document.toProject -> makeUniqueBackupFile -> save.
            // No timer wait or manual re-save (which would create .backups first).
            if (! require (ProjectSerializer::save (document.toProject(), target).wasOk(), kind + ": write first snapshot")) continue;
            const bool hasRelative = target.loadFileAsString().contains ("\"fileRelative\"");
            if (! require (original.copyDirectoryTo (moved), kind + ": copy the whole project, audio and backups")) continue;
            const auto relocated = moved.getChildFile (target.getRelativePathFrom (original));
            const auto relocatedTone = moved.getChildFile ("audio").getChildFile ("tone.wav");
            if (! require (relocated.existsAsFile() && relocatedTone.existsAsFile(), kind + ": copied media and backup both exist")) continue;
            // Guard the resolved recursive-delete target: only this test's original child.
            if (! require (original.isAChildOf (scratch.folder) && original.deleteRecursively(), kind + ": remove only the original test folder")) continue;
            if (! require (! tone.existsAsFile(), kind + ": original absolute audio path is unavailable")) continue;
            Project reopened;
            juce::StringArray warnings;
            if (! require (ProjectSerializer::load (relocated, reopened, &warnings).wasOk() && reopened.cues().size() == 1, kind + ": load moved backup")) continue;
            const auto& resolved = reopened.cues()[0];
            logMessage ("OBSERVED E3-4 " + kind + ": fileRelative=" + juce::String ((int) hasRelative)
                        + "; fileMissing=" + juce::String ((int) resolved.fileMissing) + "; resolved=" + resolved.file.getFullPathName()
                        + "; warnings=" + warnings.joinIntoString (" | "));
            expect (hasRelative && resolved.file == relocatedTone && ! resolved.fileMissing && warnings.isEmpty(),
                    kind + ": backup must resolve the audio that travelled with it");
        }
    }

};
static AuditFixUndoE3Tests auditFixUndoE3Tests;
}

namespace gocue::tests::auditFixUndoE5
{
namespace
{
// Explicit-instantiation access keeps the production class definitions and
// translation units unchanged. Only the real MainComponent's fixture state is
// exposed; edits, stop, delete, undo and chooser completion use production paths.
template <class Tag, typename Tag::Type member>
struct MemberAccess
{
    friend typename Tag::Type auditMember (Tag) { return member; }
};
struct DocumentMember
{
    using Type = ProjectDocument MainComponent::*;
    friend Type auditMember (DocumentMember);
};
struct ControllerMember
{
    using Type = CueController MainComponent::*;
    friend Type auditMember (ControllerMember);
};
template struct MemberAccess<DocumentMember, &MainComponent::document>;
template struct MemberAccess<ControllerMember, &MainComponent::controller>;

template <typename T, typename Predicate>
T* findChild (juce::Component& root, Predicate predicate)
{
    if (auto* found = dynamic_cast<T*> (&root); found != nullptr && predicate (*found)) return found;
    for (auto* child : root.getChildren())
        if (auto* found = findChild<T> (*child, predicate)) return found;
    return nullptr;
}
template <typename T> T* findChild (juce::Component& root)
{
    return findChild<T> (root, [] (const T&) { return true; });
}

// The Windows GUI dispatch used by ReopenLastProjectTests. No nested JUCE app
// or substitute MainComponent is involved.
void drainMessages()
{
   #if JUCE_WINDOWS
    MSG message {};
    for (int count = 0; count < 1000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++count)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
   #endif
}

bool pumpUntil (const std::function<bool()>& ready, int timeoutMs = 3000)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    do
    {
        drainMessages();
        if (ready()) return true;
        juce::Thread::sleep (5);
    }
    while (juce::Time::getMillisecondCounterHiRes() < deadline);
    return ready();
}

// Undo must be able to instantiate TestGain; otherwise a missing test format
// could masquerade as the product's lost-plugin defect.
class TestGainFormat final : public juce::AudioPluginFormat
{
public:
    juce::String getName() const override { return "Test"; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override {}
    bool fileMightContainThisPluginType (const juce::String& id) override { return id == "test://gain"; }
    juce::String getNameOfPluginFromIdentifier (const juce::String&) override { return "TestGain"; }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription& d) override { return fileMightContainThisPluginType (d.fileOrIdentifier); }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
    void createPluginInstance (const juce::PluginDescription&, double, int, PluginCreationCallback callback) override
    {
        callback (std::make_unique<TestGainPlugin> (1.0f), {});
    }
};

constexpr double sampleRate = 44100.0;
constexpr int blockSize = 512;

// Same ownership/destruction order as the MIDI and reopen GUI fixtures, with
// AudioEngine(0) for synchronous rendering as in FadeRunnerTests.
struct Fixture
{
    static juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options result;
        result.millisecondsBeforeSaving = -1;
        return result;
    }
    Fixture() : storage (folder.getChildFile ("test.settings"), options()), settings (storage)
    {
        folder.createDirectory();
        engine.prepare (sampleRate, blockSize);
        main = std::make_unique<MainComponent> (engine, settings, commands);
        main->setLookAndFeel (&theme);
        main->setSize (1541, 980);
        document().settings.autoBackup = false;
        document().settings.backupBeforeSave = false;
        document().settings.copyFilesIntoProject = false;
        document().settings.autoLoadNewCues = false;
        document().settings.startOnClose = false;
        document().clock = [this] { return now * 1000.0; };
        controller().getFadeRunner().stopTicking();
        controller().getFadeRunner().clock = [this] { return now; };
    }
    ~Fixture()
    {
        main.reset();
        drainMessages();
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        drainMessages();
        engine.stopAll();
        render (10);
        storage.saveIfNeeded();
        const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory);
        if (folder.isAChildOf (temp) && folder.getFileName().startsWith ("EnqueueAuditE5-"))
            folder.deleteRecursively();
    }
    ProjectDocument& document() { return (*main).*auditMember (DocumentMember {}); }
    CueController& controller() { return (*main).*auditMember (ControllerMember {}); }
    CueInspector& inspector() { return *findChild<CueInspector> (*main); }
    CueTable& table() { return *findChild<CueTable> (*main); }
    void show()
    {
        main->addToDesktop (0);
        main->setVisible (true);
        main->toFront (true);
        main->grabKeyboardFocus();
        drainMessages();
    }
    void select (const juce::Uuid& id)
    {
        document().cues.setSelectedIndex (document().cues.indexOf (id));
        if (auto* tabs = findChild<juce::TabbedComponent> (inspector())) tabs->setCurrentTabIndex (0);
        inspector().setEditable (true);
    }
    void render (int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            engine.renderBlock (out, blockSize);
            now += blockSize / sampleRate;
            engine.reapFinishedPlayers();
            if (main != nullptr) controller().getFadeRunner().tick();
        }
    }
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("EnqueueAuditE5-" + juce::Uuid().toString());
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine { 0 };
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    double now = 0.0;
    juce::AudioBuffer<float> out { 2, blockSize };
};

}

class AuditFixUndoE5Tests : public juce::UnitTest
{
public:
    AuditFixUndoE5Tests() : UnitTest ("AuditFixUndo E5", "Enqueue") {}
    void runTest() override
    {
        testFadeUndo();
        testPendingMemoOnQuit();
        for (bool cart : { false, true })
            for (bool savedPlugin : { false, true })
                testContainerUndo (cart, savedPlugin);
    }

private:
    bool require (bool condition, const juce::String& message)
    {
        expect (condition, "SETUP: " + message);
        return condition;
    }

    juce::File writeTone (Fixture& f, const juce::String& filename)
    {
        const auto file = f.folder.getChildFile (filename);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
            .withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
        if (! require (writer != nullptr, "create WAV")) return {};
        juce::AudioBuffer<float> samples (2, static_cast<int> (20 * sampleRate));
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < samples.getNumSamples(); ++i)
                samples.setSample (ch, i, 0.5f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * i / 100.0)));
        if (! require (writer->writeFromAudioSampleBuffer (samples, 0, samples.getNumSamples()), "write WAV")) return {};
        return file;
    }

    void testFadeUndo()
    {
        beginTest ("audit E5-1: unrelated name Undo preserves a stopped fade's live gain");
        Fixture f;
        const auto tone = writeTone (f, "A.wav");
        if (! tone.existsAsFile()) return;
        Cue a; a.name = "A"; a.file = tone;
        Cue fade; fade.name = "F"; fade.type = CueType::fade;
        fade.fade.targetId = a.id;
        fade.fade.mode = FadeMode::fadeOut;
        fade.fade.durationSeconds = 10.0;
        fade.fade.curve.shape = CurveShape::linear;
        fade.fade.curve.domain = AudioDomain::decibel;
        Cue b; b.name = "B-original"; b.type = CueType::control; b.control.kind = ControlKind::memo;
        f.document().cues.add (a);
        f.document().cues.add (fade);
        f.document().cues.add (b);
        f.show();
        if (! require (f.controller().fire (a.id) == CueController::GoResult::started, "start A")) return;
        f.render (10);
        if (! require (f.controller().fire (fade.id) == CueController::GoResult::started, "start F")) return;
        f.render (173); // about two seconds into the ten-second fade
        auto* active = findChild<ActiveCuesPanel> (*f.main);
        if (! require (active != nullptr && active->onStopRequested != nullptr, "active-cue stop callback")) return;
        active->onStopRequested (fade.id); // the actual F x button's MainComponent callback
        if (! require (! f.controller().getFadeRunner().isRunning (fade.id) && f.engine.isPlaying (a.id), "only F stopped")) return;
        f.render (20); // let the audio gain smoother settle
        AudioEngine::LiveState before;
        if (! require (f.engine.getLiveState (a.id, before) && before.gainDb < -5.0, "A remains attenuated")) return;
        const float rmsBefore = f.out.getRMSLevel (0, 0, blockSize);

        f.select (b.id);
        f.table().beginCellEdit (f.document().cues.indexOf (b.id), CueTable::colName);
        auto* nameEditor = findChild<juce::TextEditor> (f.table(), [&b] (const auto& e) { return e.getText() == b.name; });
        if (! require (nameEditor != nullptr, "real inline name editor for B")) return;
        nameEditor->selectAll();
        nameEditor->insertTextAtCaret ("B-edited");
        f.table().finishEditing();
        if (! require (f.document().findCueAnywhere (b.id)->name == "B-edited", "B edit committed")) return;
        AudioEngine::LiveState afterEdit;
        if (! require (f.engine.getLiveState (a.id, afterEdit) && std::abs (afterEdit.gainDb - before.gainDb) < 1.0e-6,
                       "name edit itself did not change A gain")) return;
        f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::undo));
        if (! require (f.document().findCueAnywhere (b.id)->name == b.name, "Undo restored B name")) return;
        AudioEngine::LiveState after;
        if (! require (f.engine.getLiveState (a.id, after), "A still running after Undo")) return;
        f.render (20);
        const float rmsAfter = f.out.getRMSLevel (0, 0, blockSize);
        logMessage ("OBS E5-1: A gainDb " + juce::String (before.gainDb, 6) + " -> " + juce::String (after.gainDb, 6)
                    + "; RMS " + juce::String (rmsBefore, 6) + " -> " + juce::String (rmsAfter, 6));
        expectWithinAbsoluteError (after.gainDb, before.gainDb, 1.0e-6, "Unrelated Undo must preserve A live gain");
        expectWithinAbsoluteError (rmsAfter, rmsBefore, 0.003f, "Unrelated Undo must preserve rendered output level");
    }

    void testPendingMemoOnQuit()
    {
        beginTest ("audit E5-2: quit with focused memo asks to save and persists the typed text");
        Fixture f;
        Cue a; a.type = CueType::control; a.control.kind = ControlKind::memo; a.name = "Memo"; a.notes = "saved memo";
        const auto projectFile = f.folder.getChildFile ("memo.enqueue");
        ProjectDocument seed;
        seed.settings.autoBackup = false;
        seed.settings.backupBeforeSave = false;
        seed.settings.startOnClose = false;
        seed.cues.add (a);
        if (! require (seed.save (projectFile).wasOk(), "saved project")) return;
        f.main->openProjectFile (projectFile);
        f.show();
        f.select (a.id);
        f.inspector().showNotes();
        auto* memo = findChild<juce::TextEditor> (f.inspector(), [] (const auto& e) { return e.isMultiLine(); });
        if (! require (memo != nullptr, "inspector memo field")) return;
        memo->grabKeyboardFocus();
        if (! require (pumpUntil ([memo] { return memo->hasKeyboardFocus (false); }), "memo really holds keyboard focus")) return;
        const juce::String typed = "unsaved memo typed before Alt+F4";
        memo->selectAll();
        memo->insertTextAtCaret (typed);
        // Dispatch input notifications without moving focus or manually committing.
        drainMessages();
        if (! require (memo->hasKeyboardFocus (false) && memo->getText() == typed, "typed memo keeps focus")) return;
        if (! require (f.engine.getNumPlaying() == 0 && ! f.document().settings.startOnClose, "idle quit, no close cue")) return;
        const bool dirtyBeforeQuit = f.document().isDirty();
        auto quit = std::make_shared<bool> (false);
        auto* main = f.main.get();
        // Exactly the MainComponent path used by systemRequestedQuit (Main.cpp),
        // replacing only JUCEApplication::quit with an observable continuation.
        main->confirmReplaceProjectThen ([main, quit] { main->fireCloseCueThen ([quit] { *quit = true; }); });
        const bool quitImmediately = *quit;
        drainMessages();
        auto* prompt = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
        const bool prompted = prompt != nullptr;
        logMessage ("OBS E5-2: dirty before quit=" + juce::String (dirtyBeforeQuit ? 1 : 0)
                    + "; quit immediately=" + juce::String (quitImmediately ? 1 : 0)
                    + "; save prompt=" + juce::String (prompted ? 1 : 0)
                    + "; model memo='" + f.document().findCueAnywhere (a.id)->notes + "'");
        expect (! quitImmediately, "Quit must wait for a choice while a memo edit is pending");
        expect (prompted, "Pending memo must cause the save confirmation");
        if (prompt != nullptr)
        {
            prompt->exitModalState (1); // choose Save through the real dialog callback
            expect (pumpUntil ([quit] { return *quit; }), "Quit continues after successful Save");
        }
        ProjectDocument disk;
        if (! require (disk.load (projectFile).wasOk(), "reload disk project")) return;
        const auto saved = disk.findCueAnywhere (a.id)->notes;
        logMessage ("OBS E5-2: disk memo='" + saved + "'; editor memo='" + memo->getText() + "'");
        expectEquals (saved, typed, "Save-before-quit must include the focused memo");
        // Control: the same real editor successfully commits on normal focus loss.
        f.main->grabKeyboardFocus();
        drainMessages();
        expectEquals (f.document().findCueAnywhere (a.id)->notes, typed, "Control: normal focus-loss commit works");
    }

    void testContainerUndo (bool cart, bool savedPlugin)
    {
        beginTest ("audit E5-3: " + juce::String (cart ? "cart" : "list") + " delete Undo preserves "
                   + (savedPlugin ? "an unsaved parameter edit" : "an unsaved added plugin"));
        Fixture f;
        f.engine.getPluginHost().getFormatManager().addFormat (std::make_unique<TestGainFormat>());
        auto& doc = f.document();
        const int container = doc.addContainer ("Delete me", cart);
        doc.setActiveContainer (container);
        Cue a; a.name = "Plugin cue";
        doc.cues.add (a);
        doc.cues.setSelectedIndex (0);
        auto& chain = f.engine.getCueChain (a.id);
        auto plugin = std::make_unique<TestGainPlugin> (1.0f);
        auto* gain = plugin.get();
        chain.addPlugin (std::move (plugin));
        const auto original = chain.getStates();
        PluginChain factoryControl;
        const auto errors = factoryControl.restore (original, f.engine.makePluginFactory());
        if (! require (errors.isEmpty() && factoryControl.getNumSlots() == 1
                       && dynamic_cast<TestGainPlugin*> (factoryControl.getSlot (0).plugin.get()) != nullptr,
                       "MainComponent's real factory can restore TestGain")) return;
        if (savedPlugin)
        {
            // Exercise the actual save/decorator path to seed the older state.
            const auto file = f.folder.getChildFile ("old-plugin.enqueue");
            if (! require (doc.save (file).wasOk(), "establish project path")) return;
            f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::saveProject));
            if (! require (doc.findCueAnywhere (a.id)->plugins.size() == 1, "saved plugin reflected in model")) return;
        }
        gain->gain = 0.25f; // TestGainPlugin's parameter, serialized by getStateInformation
        gain->updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
        const auto before = chain.getStates();
        if (! require (before.size() == 1 && before[0].stateBase64 != original[0].stateBase64, "parameter/state really changed")) return;
        const int containersBefore = doc.getNumContainers();
        f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::removeContainer));
        if (! require (doc.getNumContainers() == containersBefore - 1 && doc.findCueAnywhere (a.id) == nullptr,
                       "real remove-container command deleted cue")) return;
        f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::undo));
        if (! require (doc.getNumContainers() == containersBefore && doc.findCueAnywhere (a.id) != nullptr,
                       "real Undo restored container and cue")) return;
        auto* restored = f.engine.findCueChain (a.id);
        const auto after = restored != nullptr ? restored->getStates() : std::vector<PluginSlotState> {};
        auto* restoredGain = restored != nullptr && restored->getNumSlots() > 0
                                ? dynamic_cast<TestGainPlugin*> (restored->getSlot (0).plugin.get()) : nullptr;
        logMessage ("OBS E5-3: " + juce::String (cart ? "cart" : "list") + (savedPlugin ? " saved-plugin" : " new-plugin")
                    + "; slots 1 -> " + juce::String (static_cast<int> (after.size()))
                    + "; gain 0.25 -> " + (restoredGain != nullptr ? juce::String (restoredGain->gain, 2) : "<missing>")
                    + "; state " + before[0].stateBase64 + " -> " + (after.empty() ? "<missing>" : after[0].stateBase64));
        expectEquals (static_cast<int> (after.size()), static_cast<int> (before.size()), "Undo must restore the live plugin count");
        expectEquals (after.empty() ? juce::String ("<missing>") : after[0].stateBase64, before[0].stateBase64,
                      "Undo must restore the plugin state just before deletion");
        if (after.size() == before.size())
        {
            expect (restoredGain != nullptr, "Restored slot must contain the actual plugin");
            if (restoredGain != nullptr) expectWithinAbsoluteError (restoredGain->gain, 0.25f, 1.0e-6f);
        }
    }

};
static AuditFixUndoE5Tests auditFixUndoE5Tests;
}
