#include "app/CueController.h"
#include "MainComponentTestAccess.h"
#include "ui/GoCueLookAndFeel.h"

#include <algorithm>
#include <cmath>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
namespace
{
constexpr double sampleRate = 44100.0;
constexpr int blockSize = 512;
constexpr double blockSeconds = blockSize / sampleRate;

// CueControllerTests / AuditFixCueTests: synchronous reads, a fresh document,
// scheduler and fake clock for every scenario, and render/reap/tick in that order.
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
        controller.getFadeRunner().stopAll();
        engine.stopAll();
    }

    void render (int blocks = 1)
    {
        for (int i = 0; i < blocks; ++i)
        {
            engine.renderBlock (out, blockSize);
            now += blockSeconds;
            engine.reapFinishedPlayers();
            controller.getFadeRunner().tick();
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

};

int startsOf (const std::vector<CueController::RecordedStart>& starts, const juce::Uuid& id)
{
    return (int) std::count_if (starts.begin(), starts.end(), [&] (const auto& start) { return start.cueId == id; });
}

Cue audio (const juce::String& name, const juce::File& file)
{
    Cue cue;
    cue.name = name;
    cue.file = file;
    cue.durationSeconds = 30.0;
    cue.numChannels = 2;
    cue.audio.endSeconds = 30.0;
    return cue;
}

Cue group (GroupMode mode)
{
    Cue cue;
    cue.name = "G";
    cue.type = CueType::group;
    cue.group.mode = mode;
    return cue;
}

Cue control (const juce::String& name, ControlKind kind, const juce::Uuid& target)
{
    Cue cue;
    cue.name = name;
    cue.type = CueType::control;
    cue.control.kind = kind;
    cue.control.targetId = target;
    return cue;
}

template <typename T> T* findChild (juce::Component& root)
{
    if (auto* found = dynamic_cast<T*> (&root))
        return found;
    for (auto* child : root.getChildren())
        if (auto* found = findChild<T> (*child))
            return found;
    return nullptr;
}

// AuditFixUndoTests: drain native messages after destroying MainComponent and
// keep its engine, command manager and look-and-feel alive until then.
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
    const juce::File base = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File folder = base.getChildFile ("AuditFix0923Cue-" + juce::Uuid().toString());
    ScratchDirectory() { folder.createDirectory(); }
    ~ScratchDirectory()
    {
        if (folder.isAChildOf (base) && folder.getFileName().startsWith ("AuditFix0923Cue-"))
            folder.deleteRecursively();
    }
};

struct CartFixture
{
    ScratchDirectory scratch;
    const juce::File file = scratch.folder.getChildFile ("cart.enqueue");
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine { 0 };
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    juce::AudioBuffer<float> out { 2, blockSize };
    double renderedUntil = 0.0;

    static juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options result;
        result.storageFormat = juce::PropertiesFile::storeAsXML;
        result.millisecondsBeforeSaving = -1;
        return result;
    }

    CartFixture() : storage (scratch.folder.getChildFile ("test.settings"), options()), settings (storage)
    {
        engine.prepare (sampleRate, blockSize);
        main = std::make_unique<MainComponent> (engine, settings, commands);
        main->setLookAndFeel (&theme);
        main->setSize (1200, 900);
        renderedUntil = controller().clock();
    }

    ~CartFixture()
    {
        main.reset();
        drainMessages();
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        drainMessages();
        engine.shutdown();
        storage.saveIfNeeded();
    }

    CueController& controller() { return ReopenLastProjectTestAccess::controller (*main); }

    bool open (const Project& project)
    {
        if (ProjectSerializer::save (project, file).failed())
            return false;
        main->openProjectFile (file, false);
        for (int i = 0; i < 4; ++i)
            engine.renderBlock (out, blockSize);
        engine.reapIfNeeded();
        return main->getProjectFile() == file;
    }

    // MainComponent owns a private scheduler with its real clock. Keep that
    // scheduler and the installed cart callbacks intact: this one integration
    // scenario deliberately spends the actual 10 seconds, pumping JUCE timers
    // and rendering at the same rate. All controller-only cases use fake time.
    void renderUntil (double until)
    {
        while (controller().clock() < until)
        {
            const double now = controller().clock();
            while (renderedUntil + blockSeconds <= now)
            {
                engine.renderBlock (out, blockSize);
                renderedUntil += blockSeconds;
                engine.reapFinishedPlayers();
            }
            drainMessages();
            juce::Timer::callPendingTimersSynchronously();
            juce::Thread::sleep (1);
        }
        juce::Timer::callPendingTimersSynchronously();
    }
};
} // namespace

class AuditFix0923CueTests : public juce::UnitTest
{
public:
    AuditFix0923CueTests() : UnitTest ("AuditFix0923 Cue", "Enqueue") {}

    void runTest() override
    {
        const juce::TemporaryFile tone (".wav");
        beginTest ("audit0923 EB-1: cart right-click cancels hotkey pre-wait");
        if (! writeSine (tone.getFile()))
            return;
        cartStop (tone.getFile());

        beginTest ("audit0923 EB-2: repeated hotkey fade follows once (restart B)");
        repeatedFade (tone.getFile(), false);
        beginTest ("audit0923 EB-2: repeated hotkey fade keeps B playing (hard-stop B)");
        repeatedFade (tone.getFile(), true);

        beginTest ("audit0923 EB-3: pause/start control hotkeys resume playlist A");
        resumePlaylist (tone.getFile());

        beginTest ("EB-2 regression: scheduled fade restart keeps its own follow and unrelated work");
        scheduledFade (tone.getFile(), false);
        beginTest ("EB-2 regression: fade restart after pre/post-wait keeps its own follow");
        scheduledFade (tone.getFile(), true);

        beginTest ("EB-3 regression: timeline resumes paused children without restarting other children");
        resumeTimeline (tone.getFile(), false);
        beginTest ("EB-3 regression: nested timeline resumes descendants and preserves pending starts");
        resumeTimeline (tone.getFile(), true);
        beginTest ("EB-3 regression: start still advances an unpaused playlist");
        startUnpausedPlaylist (tone.getFile(), false);
        beginTest ("EB-3 regression: loaded children do not turn a group start into a resume");
        startUnpausedPlaylist (tone.getFile(), true);
    }

private:
    bool writeSine (const juce::File& file)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        expect (stream != nullptr, "Fixture: temporary WAV must be writable");
        if (stream == nullptr)
            return false;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
            .withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
        expect (writer != nullptr, "Fixture: WAV writer must be available");
        if (writer == nullptr)
            return false;
        const int samples = (int) (30.0 * sampleRate);
        juce::AudioBuffer<float> buffer (2, samples);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < samples; ++i)
                buffer.setSample (ch, i, 0.5f * (float) std::sin (
                    2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));
        const bool written = writer->writeFromAudioSampleBuffer (buffer, 0, samples);
        expect (written, "Fixture: WAV samples must be complete");
        return written;
    }

    void cartStop (const juce::File& tone)
    {
        CartFixture f;
        Project project;
        project.name = "Audit0923 EB-1";
        project.settings.autoBackup = false;
        project.settings.backupBeforeSave = false;
        auto& cartList = project.ensureMainList();
        cartList.isCart = true;
        cartList.cartRows = cartList.cartCols = 1;
        auto a = audio ("A", tone);
        a.preWaitSeconds = 10.0;
        const juce::KeyPress hotkey (juce::KeyPress::F7Key);
        a.hotkey = hotkey.getTextDescription();
        cartList.cues = { a };
        const bool opened = f.open (project);
        expect (opened, "Fixture: real MainComponent must open the cart project");
        if (! opened)
            return;
        auto* cart = findChild<CueCartView> (*f.main);
        expect (cart != nullptr && cart->isVisible(), "Fixture: the actual cart widget must be visible");
        if (cart == nullptr || ! cart->isVisible())
            return;
        expect ((bool) cart->onStop, "Fixture: MainComponent's stop callback must be installed");
        // The cart is visible inside the component tree, but the root has no
        // desktop peer: mouseDown cannot show a window or take OS focus.
        expect (! f.main->isShowing() && cart->getPeer() == nullptr,
                "Fixture: the integration test must stay off screen");
        auto& controller = f.controller();
        controller.startRecording();
        const double triggeredAt = controller.clock();
        expect (controller.handleHotkey (hotkey), "Use the real hotkey -> triggerCueById path");
        expect (controller.hasPendingFor (a.id), "Fixture: the cart hotkey must actually schedule A");
        f.renderUntil (triggeredAt + 2.0);
        expect (! f.engine.isPlaying (a.id), "Fixture: right-click takes place during the pre-wait");
        const int pendingBefore = controller.getNumPending();

        // Dispatch a right-button event to the real widget. Its onStop is the
        // untouched callback installed by MainComponent, not a test substitute.
        const auto point = cart->getLocalBounds().getCentre().toFloat();
        const auto eventTime = juce::Time::getCurrentTime();
        const juce::MouseEvent click (juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier),
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, cart, cart, eventTime, point, eventTime, 1, false);
        cart->mouseDown (click);
        cart->mouseUp (click);
        const int pendingAfter = controller.getNumPending();
        expect (! controller.hasPendingFor (a.id), "Right-click must cancel A's pending start");
        expectEquals (pendingAfter, 0, "No start may remain immediately after right-click");

        f.renderUntil (triggeredAt + 10.3);
        const auto starts = controller.stopRecording();
        expectEquals (startsOf (starts, a.id), 0, "A must never start after the cart stop");
        expect (! f.engine.isPlaying (a.id), "A must stay silent after the original deadline");
        logMessage ("OBSERVED EB-1: real MainComponent cart; pending at right-click="
            + juce::String (pendingBefore) + "->" + juce::String (pendingAfter)
            + "; A starts=" + juce::String (startsOf (starts, a.id))
            + "; A playing at 10.3s=" + juce::String ((int) f.engine.isPlaying (a.id))
            + "; A order=" + juce::String (f.engine.getStartOrder (a.id)));
    }

    void repeatedFade (const juce::File& tone, bool hardStop)
    {
        Fixture f;
        f.document.settings.doubleGoSeconds = 0.5;
        const auto a = audio ("A", tone);
        auto b = audio ("B", tone);
        if (hardStop)
            b.secondTrigger = SecondTriggerAction::hardStop;
        Cue fade;
        fade.name = "F";
        fade.type = CueType::fade;
        fade.fade.targetId = a.id;
        fade.fade.mainDb = -12.0;
        fade.fade.durationSeconds = 5.0;
        fade.continueMode = ContinueMode::autoFollow;
        const juce::KeyPress hotkey (juce::KeyPress::F8Key);
        fade.hotkey = hotkey.getTextDescription();
        for (const auto& cue : { a, fade, b })
            f.document.cues.add (cue);
        f.document.cues.setPlayheadIndex (0);
        expect (f.go() == CueController::GoResult::started);
        f.render (8);
        expect (f.engine.isPlaying (a.id), "Fixture: long A must be playing");
        expect (f.controller.handleHotkey (hotkey));
        expect (f.controller.getFadeRunner().isRunning (fade.id), "Fixture: first F must start");
        const int firstPending = f.controller.getNumPending();
        f.renderUntil (f.now + 1.0);
        const double restartedAt = f.now;
        expect (f.controller.handleHotkey (hotkey), "Retrigger through the hotkey after the 0.5s window");
        const int secondPending = f.controller.getNumPending();
        expectEquals (f.controller.getFadeRunner().getNumRunning(), 1, "Fixture: F must replace its running fade");
        f.renderUntil (restartedAt + 4.8);
        expect (f.controller.getFadeRunner().isRunning (fade.id));
        expect (! f.engine.isPlaying (b.id), "B must wait for the restarted fade to finish");
        f.renderUntil (restartedAt + 5.2); // includes blocks after a possible immediate stop of B
        const auto starts = f.controller.stopRecording();
        expectEquals (startsOf (starts, fade.id), 2, "Fixture: both hotkeys must actually start F");
        expect (! f.controller.getFadeRunner().isRunning (fade.id));
        expectEquals (startsOf (starts, b.id), 1, "The restarted fade must start B exactly once");
        expect (f.engine.isPlaying (b.id), "B must still play after the fade's follow");
        logMessage ("OBSERVED EB-2 B rule=" + juce::String (hardStop ? "hardStop" : "hardStopRestart")
            + ": F starts=" + juce::String (startsOf (starts, fade.id))
            + "; pending follows=" + juce::String (firstPending) + "->" + juce::String (secondPending)
            + "; B starts=" + juce::String (startsOf (starts, b.id))
            + "; B playing=" + juce::String ((int) f.engine.isPlaying (b.id))
            + "; B order=" + juce::String (f.engine.getStartOrder (b.id)));
    }

    void resumePlaylist (const juce::File& tone)
    {
        Fixture f;
        auto g = group (GroupMode::playlist);
        auto a = audio ("A", tone), b = audio ("B", tone);
        a.parentId = b.parentId = g.id;
        auto p = control ("P", ControlKind::pause, g.id);
        auto s = control ("S", ControlKind::start, g.id);
        const juce::KeyPress pauseKey (juce::KeyPress::F7Key), startKey (juce::KeyPress::F8Key);
        p.hotkey = pauseKey.getTextDescription();
        s.hotkey = startKey.getTextDescription();
        for (const auto& cue : { g, a, b, p, s })
            f.document.cues.add (cue);
        f.document.cues.setPlayheadIndex (0);
        expect (f.go() == CueController::GoResult::started);
        f.renderUntil (1.0);
        expect (f.engine.isPlaying (a.id) && ! f.engine.isPlaying (b.id), "Fixture: playlist must begin with A only");
        const auto before = f.engine.getStartOrder (a.id);
        expect (f.controller.handleHotkey (pauseKey));
        f.render (8);
        expect (f.engine.isPaused (a.id), "Fixture: P must pause A through G");
        const auto pausedPosition = f.engine.getVirtualPosition (a.id);
        expect (f.controller.handleHotkey (startKey), "Use S's hotkey; GO would resumeAll first");
        f.render (16);
        const auto after = f.engine.getStartOrder (a.id);
        const auto bOrder = f.engine.getStartOrder (b.id);
        expect (f.engine.isPlaying (a.id), "Start control must keep A alive");
        expect (! f.engine.isPaused (a.id), "Start control must resume A");
        expectEquals (after, before, "Resuming must preserve A's original instance");
        expect (f.engine.getVirtualPosition (a.id) > pausedPosition, "A must continue from its paused position");
        expectEquals (bOrder, (juce::int64) -1, "B must remain unstarted");
        expect (! f.engine.isPlaying (b.id), "Resume must not advance the playlist to B");
        logMessage ("OBSERVED EB-3: A order=" + juce::String (before) + "->" + juce::String (after)
            + "; A paused=" + juce::String ((int) f.engine.isPaused (a.id))
            + "; A position=" + juce::String (pausedPosition) + "->" + juce::String (f.engine.getVirtualPosition (a.id))
            + "; B order=" + juce::String (bOrder) + "; B playing=" + juce::String ((int) f.engine.isPlaying (b.id)));
    }

    void scheduledFade (const juce::File& tone, bool preWait)
    {
        Fixture f;
        const auto a = audio ("A", tone), b = audio ("B", tone);
        auto source = control ("Source", ControlKind::wait, juce::Uuid::null());
        source.control.seconds = 0.1;
        source.continueMode = ContinueMode::autoContinue;
        source.postWaitSeconds = 1.0;
        Cue fade;
        fade.name = "F";
        fade.type = CueType::fade;
        fade.fade.targetId = a.id;
        fade.fade.mainDb = -12.0;
        fade.fade.durationSeconds = 5.0;
        fade.continueMode = ContinueMode::autoFollow;
        const juce::KeyPress fadeKey (juce::KeyPress::F8Key);
        fade.hotkey = fadeKey.getTextDescription();
        auto duck = audio ("Unrelated duck", tone);
        duck.audio.endSeconds = 3.0;
        duck.duck.enabled = true;
        duck.duck.levelDb = -9.0;
        duck.duck.seconds = 0.01;
        duck.continueMode = ContinueMode::autoFollow;
        const auto followed = audio ("Unrelated follow", tone);
        auto delayed = audio ("Unrelated pre-wait", tone);
        delayed.preWaitSeconds = 8.0;
        for (const auto& cue : { a, source, fade, b, duck, followed, delayed })
            f.document.cues.add (cue);
        f.document.cues.setPlayheadIndex (0);
        expect (f.go() == CueController::GoResult::started);
        f.render (8);
        f.controller.fireSequence (f.document.cues.indexOf (duck.id));
        f.controller.fireSequence (f.document.cues.indexOf (delayed.id));

        const double scheduledAt = f.now + source.postWaitSeconds + (preWait ? 0.25 : 0.0);
        if (preWait)
        {
            expect (f.controller.handleHotkey (fadeKey));
            f.document.cues.update (f.document.cues.indexOf (fade.id), [] (Cue& c) { c.preWaitSeconds = 0.25; });
            f.controller.fireSequence (f.document.cues.indexOf (source.id));
        }
        else
        {
            // A first, direct run must leave the later scheduled start intact.
            f.controller.fireSequence (f.document.cues.indexOf (source.id));
            expect (f.controller.handleHotkey (fadeKey));
        }
        expect (f.controller.hasPendingFor (fade.id), "The scheduled restart must remain queued");
        f.renderUntil (scheduledAt + 0.1);
        expectEquals (f.controller.getFadeRunner().getNumRunning(), 1);
        expect (f.controller.hasPendingFor (duck.id, true), "Another cue's follow must survive the restart");
        expect (f.controller.hasPendingFor (delayed.id), "Another cue's pre-wait must survive the restart");
        expectWithinAbsoluteError (f.engine.getDuckDb (a.id), -9.0, 1.0e-9,
                                   "Another cue's duck must survive the restart");
        f.renderUntil (scheduledAt + 4.8);
        expect (! f.engine.isPlaying (b.id), "B must wait for the scheduled replacement fade");
        f.renderUntil (scheduledAt + 5.2);
        expect (f.engine.isPlaying (b.id));
        expect (f.engine.isPlaying (followed.id), "The unrelated follow must still fire");
        expectWithinAbsoluteError (f.engine.getDuckDb (a.id), 0.0, 1.0e-9,
                                   "The unrelated duck must release normally");
        expect (f.controller.hasPendingFor (delayed.id));
        f.renderUntil (9.0);
        const auto starts = f.controller.stopRecording();
        expectEquals (startsOf (starts, fade.id), 2);
        expectEquals (startsOf (starts, b.id), 1, "Only the scheduled run's follow may start B");
        expectEquals (startsOf (starts, followed.id), 1);
        expectEquals (startsOf (starts, delayed.id), 1);
    }

    void resumeTimeline (const juce::File& tone, bool nested)
    {
        Fixture f;
        const auto g = group (GroupMode::timeline);
        auto inner = group (GroupMode::timeline);
        inner.name = "Inner";
        inner.parentId = g.id;
        auto a = audio ("A", tone), b = audio ("B", tone), delayed = audio ("Delayed", tone);
        a.parentId = b.parentId = nested ? inner.id : g.id;
        delayed.parentId = g.id;
        delayed.preWaitSeconds = 10.0;
        const auto outside = audio ("Outside", tone);
        auto p = control ("P", ControlKind::pause, g.id);
        auto s = control ("S", ControlKind::start, g.id);
        const juce::KeyPress pauseKey (juce::KeyPress::F7Key), startKey (juce::KeyPress::F8Key);
        p.hotkey = pauseKey.getTextDescription();
        s.hotkey = startKey.getTextDescription();
        f.document.cues.add (g);
        if (nested)
            f.document.cues.add (inner);
        for (const auto& cue : { a, b, delayed, outside, p, s })
            f.document.cues.add (cue);
        f.document.cues.setPlayheadIndex (0);
        expect (f.engine.load (delayed));
        expect (f.go() == CueController::GoResult::started);
        expect (f.controller.fire (outside.id) == CueController::GoResult::started);
        f.renderUntil (1.0);
        expect (f.engine.isPlaying (a.id) && f.engine.isPlaying (b.id));
        const auto aOrder = f.engine.getStartOrder (a.id), bOrder = f.engine.getStartOrder (b.id);
        f.controller.pauseCue (outside.id);
        expect (f.controller.handleHotkey (pauseKey));
        f.render (8);
        expect (f.engine.isPaused (a.id) && f.engine.isPaused (b.id));
        expect (f.controller.resumeCue (b.id));   // leave a mixture of paused and running children
        f.render (8);
        const auto aPosition = f.engine.getVirtualPosition (a.id);
        const auto bPosition = f.engine.getVirtualPosition (b.id);
        const int pendingBefore = f.controller.getNumPending();
        expect (f.controller.handleHotkey (startKey));
        f.render (16);
        expect (f.engine.isPlaying (a.id) && ! f.engine.isPaused (a.id));
        expect (f.engine.isPlaying (b.id) && ! f.engine.isPaused (b.id));
        expectEquals (f.engine.getStartOrder (a.id), aOrder, "Paused descendants must keep their instances");
        expectEquals (f.engine.getStartOrder (b.id), bOrder, "Running descendants must not restart");
        expect (f.engine.getVirtualPosition (a.id) > aPosition);
        expect (f.engine.getVirtualPosition (b.id) > bPosition);
        expect (f.engine.isPaused (outside.id), "A group start must not resume cues outside the group");
        expect (! f.engine.isPlaying (delayed.id) && f.engine.isLoaded (delayed.id),
                "A loaded child must keep waiting for its scheduled start");
        expect (f.controller.hasPendingFor (delayed.id));
        expectEquals (f.controller.getNumPending(), pendingBefore);
        f.renderUntil (10.2);
        expect (f.engine.isPlaying (delayed.id));
        const auto starts = f.controller.stopRecording();
        expectEquals (startsOf (starts, a.id), 1);
        expectEquals (startsOf (starts, b.id), 1);
        expectEquals (startsOf (starts, delayed.id), 1);
    }

    void startUnpausedPlaylist (const juce::File& tone, bool loadedOnly)
    {
        Fixture f;
        const auto g = group (GroupMode::playlist);
        auto a = audio ("A", tone), b = audio ("B", tone);
        a.parentId = b.parentId = g.id;
        auto s = control ("S", ControlKind::start, g.id);
        const juce::KeyPress startKey (juce::KeyPress::F8Key);
        s.hotkey = startKey.getTextDescription();
        for (const auto& cue : { g, a, b, s })
            f.document.cues.add (cue);
        if (loadedOnly)
        {
            expect (f.engine.load (a));
            expect (f.engine.isLoaded (a.id) && ! f.engine.isPlaying (a.id));
        }
        else
        {
            f.document.cues.setPlayheadIndex (0);
            expect (f.go() == CueController::GoResult::started);
            f.render (8);
            expect (f.engine.isPlaying (a.id) && ! f.engine.isPaused (a.id));
        }
        expect (f.controller.handleHotkey (startKey));
        f.render (16);
        expect (f.engine.isPlaying (loadedOnly ? a.id : b.id));
        expect (! f.engine.isPlaying (loadedOnly ? b.id : a.id));
        expect (! f.engine.isPaused (loadedOnly ? a.id : b.id));
    }

};

static AuditFix0923CueTests auditFix0923CueTests;
} // namespace gocue::tests
