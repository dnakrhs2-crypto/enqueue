#include "MainComponentTestAccess.h"
#include "TestGainPlugin.h"
#include "app/Commands.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/KeyCapture.h"
#include "ui/PluginChainComponent.h"
#include "ui/ShortcutSettingsTab.h"

#include <array>
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

// ED-1 / ED-3 and their helpers come from audit0923_ref. Observe the real
// document/players without changing production access or replacing callbacks.
template <class Tag, typename Tag::Type member>
struct AuditMemberAccess { friend typename Tag::Type auditMember (Tag) { return member; } };
struct DocumentMember
{
    using Type = ProjectDocument MainComponent::*;
    friend Type auditMember (DocumentMember);
};
struct PlayersMember
{
    using Type = std::vector<std::unique_ptr<CuePlayer>> AudioEngine::*;
    friend Type auditMember (PlayersMember);
};
template struct AuditMemberAccess<DocumentMember, &MainComponent::document>;
template struct AuditMemberAccess<PlayersMember, &AudioEngine::players>;

// Deliver the JUCE part of a focus transition, after native focus acquisition.
// This uses JUCE's own loss/gain dispatch (including ancestor notifications),
// never calls KeyCapture's focus callbacks or acquires OS keyboard focus.
struct FocusMember
{
    using Type = juce::Component**;
    friend Type auditMember (FocusMember);
};
struct FocusGainMember
{
    using Type = void (juce::Component::*) (juce::Component::FocusChangeType);
    friend Type auditMember (FocusGainMember);
};
struct FocusLossMember
{
    using Type = void (juce::Component::*) (juce::Component::FocusChangeType);
    friend Type auditMember (FocusLossMember);
};
template struct AuditMemberAccess<FocusMember, &juce::Component::currentlyFocusedComponent>;
template struct AuditMemberAccess<FocusGainMember, &juce::Component::internalKeyboardFocusGain>;
template struct AuditMemberAccess<FocusLossMember, &juce::Component::internalKeyboardFocusLoss>;

void focusWithin (juce::Component& next)
{
    auto*& focused = *auditMember (FocusMember {});
    if (focused == &next) return;
    juce::Component::SafePointer<juce::Component> previous (focused);
    focused = &next;
    if (previous != nullptr) ((*previous).*auditMember (FocusLossMember {})) (juce::Component::focusChangedDirectly);
    (next.*auditMember (FocusGainMember {})) (juce::Component::focusChangedDirectly);
}

template <typename T, typename Predicate>
T* findChild (juce::Component& root, Predicate matches)
{
    if (auto* found = dynamic_cast<T*> (&root); found != nullptr && matches (*found)) return found;
    for (auto* child : root.getChildren())
        if (auto* found = findChild<T> (*child, matches)) return found;
    return nullptr;
}
template <typename T> T* findChild (juce::Component& root)
{
    return findChild<T> (root, [] (const T&) { return true; });
}

void drainMessages()
{
   #if JUCE_WINDOWS
    MSG message {};
    for (int n = 0; n < 2000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
   #endif
}
bool pumpUntil (const std::function<bool()>& ready, int timeoutMs = 3000)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    do
    {
        drainMessages();
        juce::Timer::callPendingTimersSynchronously();
        if (ready()) return true;
        juce::Thread::sleep (2);
    } while (juce::Time::getMillisecondCounterHiRes() < end);
    return ready();
}

struct Scratch
{
    Scratch() { folder.createDirectory(); }
    ~Scratch()
    {
        if (folder.isAChildOf (base) && folder.getFileName().startsWith ("AuditFix0923Engine-"))
            folder.deleteRecursively();
    }
    const juce::File base = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File folder = base.getChildFile ("AuditFix0923Engine-" + juce::Uuid().toString());
};

bool writeTone (const juce::File& file)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (! stream) return false;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate)
                                         .withNumChannels (2).withBitsPerSample (16));
    if (! writer) return false;
    juce::AudioBuffer<float> block (2, 44100);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < block.getNumSamples(); ++i)
            block.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));
    for (int second = 0; second < 30; ++second)
        if (! writer->writeFromAudioSampleBuffer (block, 0, block.getNumSamples())) return false;
    return true;
}

Project projectWith (const juce::File& file, int count = 2)
{
    Project project;
    project.name = "AuditFix0923 Engine";
    project.ensureMainList();
    project.settings.autoBackup = false;
    project.settings.backupBeforeSave = false;
    project.settings.copyFilesIntoProject = false;
    project.settings.autoLoadNewCues = false;
    auto& patch = project.ensureDefaultPatch();
    patch.numCueOutputs = 2;
    patch.sanitise();
    for (int i = 0; i < count; ++i)
    {
        Cue cue;
        cue.name = i == 0 ? "A" : "B";
        cue.number = juce::String (i + 1);
        cue.file = file;
        cue.numChannels = 2;
        cue.durationSeconds = 30.0;
        cue.autoLoad = false;
        cue.levels.resize (2, 2);
        cue.levels.setDefaults();
        project.cues().push_back (cue);
    }
    return project;
}

struct Fixture
{
    static juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options result;
        result.millisecondsBeforeSaving = -1;
        return result;
    }
    Fixture() : storage (scratch.folder.getChildFile ("test.settings"), options()), settings (storage)
    {
        engine.prepare (sampleRate, blockSize, 2);
        main = std::make_unique<MainComponent> (engine, settings, commands);
        main->setLookAndFeel (&theme);
        main->setSize (1541, 980);
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
    ProjectDocument& document() { return (*main).*auditMember (DocumentMember {}); }
    CueController& controller() { return ReopenLastProjectTestAccess::controller (*main); }
    CueInspector& inspector() { return *findChild<CueInspector> (*main); }
    juce::TableListBox& list() { return *findChild<juce::TableListBox> (*findChild<CueTable> (*main)); }
    bool command (juce::CommandID id) { return main->perform (juce::ApplicationCommandTarget::InvocationInfo (id)); }
    bool key (const juce::KeyPress& keyPress) { return ReopenLastProjectTestAccess::keyboard (*main).keyPressed (keyPress, &list()); }
    void render (int blocks = 8)
    {
        for (int n = 0; n < blocks; ++n) engine.renderBlock (out, blockSize);
        engine.reapIfNeeded();
    }
    bool open (const Project& project)
    {
        const auto file = scratch.folder.getChildFile ("show.enqueue");
        if (ProjectSerializer::save (project, file).failed()) return false;
        main->openProjectFile (file, false);
        render();
        drainMessages();
        return main->getProjectFile() == file && ! document().isDirty();
    }
    bool tab (const juce::String& name)
    {
        auto* tabs = findChild<juce::TabbedComponent> (inspector());
        const int index = tabs != nullptr ? tabs->getTabNames().indexOf (name) : -1;
        if (index < 0) return false;
        tabs->setCurrentTabIndex (index);
        drainMessages();
        return true;
    }
    std::array<double, 2> rms()
    {
        render();
        std::array<double, 2> sums {};
        for (int b = 0; b < 16; ++b)
        {
            engine.renderBlock (out, blockSize);
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < blockSize; ++n)
                    sums[(size_t) ch] += (double) out.getSample (ch, n) * out.getSample (ch, n);
        }
        for (auto& value : sums) value = std::sqrt (value / (16 * blockSize));
        return sums;
    }
    void runFor (double seconds)
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        int rendered = 0;
        pumpUntil ([&]
        {
            const double elapsed = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
            while ((rendered + 1) * blockSize / sampleRate <= elapsed) { render (1); ++rendered; }
            return elapsed >= seconds;
        }, (int) (seconds * 1000.0) + 100);
    }
    Scratch scratch;
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine { 0 };
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    juce::AudioBuffer<float> out { 2, blockSize };
};

class MuteFormat final : public juce::AudioPluginFormat
{
public:
    juce::String getName() const override { return "Test"; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override {}
    bool fileMightContainThisPluginType (const juce::String& s) override { return s == "test://gain"; }
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
        callback (std::make_unique<TestGainPlugin> (0.0f), {});
    }
};

#if JUCE_WINDOWS
// JUCE's logical tree is showing, so the real asynchronous focus checks run.
// Its native peer is a disabled child of a never-shown, nonactivating host:
// neither constructing it nor KeyCapture::start can display or focus a window.
struct HiddenDesktop
{
    explicit HiddenDesktop (juce::Component& content) : root (content)
    {
        host = CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Audit hidden host",
                                WS_POPUP | WS_DISABLED, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
        if (host == nullptr) return;
        root.setVisible (true); // no peer yet
        root.addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresKeyPresses, host);
        if (auto* peer = root.getPeer())
        {
            EnableWindow ((HWND) peer->getNativeHandle(), FALSE);
            peer->setVisible (false); // keep the logical tree showing, the native window hidden
        }
    }
    ~HiddenDesktop()
    {
        root.giveAwayKeyboardFocus();
        root.setVisible (false);
        root.removeFromDesktop();
        if (host != nullptr) DestroyWindow (host);
    }
    bool isHidden() const
    {
        auto* peer = root.getPeer();
        return host != nullptr && peer != nullptr && ! IsWindowVisible (host)
            && ! IsWindowVisible ((HWND) peer->getNativeHandle()) && ! IsWindowEnabled ((HWND) peer->getNativeHandle());
    }
    juce::Component& root;
    HWND host = nullptr;
};
#endif

class AuditFix0923EngineTests final : public juce::UnitTest
{
public:
    AuditFix0923EngineTests() : UnitTest ("AuditFix0923 Engine", "Enqueue") {}
    void runTest() override
    {
        firstInsert();
        chainOwnership();
        childFocus();
        focusExceptions();
    }
private:
    bool require (bool condition, const juce::String& reason)
    {
        expect (condition, "Fixture prerequisite: " + reason);
        return condition;
    }

    void firstInsert()
    {
        beginTest ("audit0923 ED-1: first inspector insert mutes B already started by hotkey");
        Fixture f;
        const auto tone = f.scratch.folder.getChildFile ("tone.wav");
        if (! require (writeTone (tone), "create long temporary audio")) return;
        auto project = projectWith (tone);
        const juce::KeyPress hotkey (juce::KeyPress::F7Key);
        project.cues()[1].hotkey = hotkey.getTextDescription();
        const auto id = project.cues()[1].id;
        if (! require (f.open (project), "open saved A-selected project")) return;
        if (! require (f.document().cues.getSelectedIndex() == 0 && f.command (CommandIDs::saveProject), "save while A selected")) return;
        f.main->openProjectFile (f.main->getProjectFile(), false);
        f.render();
        drainMessages();
        if (! require (f.document().cues.getSelectedIndex() == 0 && f.engine.findCueChain (id) == nullptr, "B never selected, no chain after reopening")) return;
        if (! require (f.key (hotkey) && f.engine.isPlaying (id), "real hotkey starts B")) return;
        if (! require (f.document().cues.getSelectedIndex() == 0 && f.engine.findCueChain (id) == nullptr, "hotkey leaves A selected and B chainless")) return;
        const auto before = f.rms();
        if (! require (before[0] > 0.3, "B audibly playing before insertion")) return;
        auto& host = f.engine.getPluginHost();
        host.getFormatManager().addFormat (std::make_unique<MuteFormat>());
        juce::PluginDescription description;
        { TestGainPlugin prototype (0.0f); prototype.fillInPluginDescription (description); }
        host.getKnownPlugins().addType (description);
        f.list().selectRowsBasedOnModifierKeys (1, {}, false);
        if (! require (f.document().cues.getSelectedIndex() == 1 && f.tab (ko ("플러그인")), "select B and open inspector Plugins tab")) return;
        auto* strip = findChild<PluginChainComponent> (f.inspector());
        auto* add = strip != nullptr ? findChild<juce::TextButton> (*strip, [] (const auto& b) { return b.getButtonText() == ko ("+ 플러그인"); }) : nullptr;
        if (! require (add != nullptr && add->isEnabled() && (bool) strip->performEdit, "real inspector plugin edit callback")) return;
        auto* chain = f.engine.findCueChain (id);
        if (! require (chain != nullptr && chain == strip->getChain() && chain->getNumSlots() == 0, "selection creates B's first empty chain")) return;

        // The reference's menu completion also opens a native plugin editor.
        // Deliver its edit at the public inspector callback instead: real host,
        // chain, document transaction and audio, without a popup/editor window.
        juce::String error;
        auto instance = host.createInstance (description, sampleRate, blockSize, error);
        if (! require (instance != nullptr, "create mute plugin through the real host: " + error)) return;
        strip->performEdit (ko ("플러그인 추가"), [&] { chain->addPlugin (std::move (instance)); });
        strip->refresh();
        auto* plugin = dynamic_cast<TestGainPlugin*> (chain->getSlot (0).plugin.get());
        if (! require (plugin != nullptr && plugin->gain == 0.0f && plugin->prepareCount > 0
                       && ! chain->getSlot (0).bypassed.load() && ! chain->getSlot (0).faulted.load()
                       && strip->getChain() == chain, "inspector displays a prepared, active, non-faulted mute TestGain instance")) return;
        const int initialCalls = plugin->processCount;
        const auto after = f.rms();
        const int liveCalls = plugin->processCount - initialCalls;
        if (! require (f.command (CommandIDs::hardStopAll), "stop B via app command")) return;
        f.render();
        if (! require (f.command (CommandIDs::preview) && f.engine.isPlaying (id), "restart B through app preview")) return;
        const int restartStartCalls = plugin->processCount;
        const auto restarted = f.rms();
        const int restartCalls = plugin->processCount - restartStartCalls;
        logMessage ("OBS ED-1: RMS " + juce::String (before[0], 6) + " -> first insert " + juce::String (after[0], 6)
                    + " -> restart " + juce::String (restarted[0], 6) + "; process calls live/restart=" + juce::String (liveCalls) + "/" + juce::String (restartCalls));
        expectWithinAbsoluteError (after[0], 0.0, 1.0e-7, "first inspector mute insert must affect already playing B");
        expectGreaterThan (liveCalls, 0, "B must process the newly attached insert without restarting");
        expectWithinAbsoluteError (restarted[0], 0.0, 1.0e-7, "positive control: restart is muted");
        expectGreaterThan (restartCalls, 0, "positive control: plugin processes after restart");
    }

    void chainOwnership()
    {
        beginTest ("ED-1: late chain belongs to current playback and LOAD, never retired instances; removal and clear detach");
        Scratch scratch;
        const auto tone = scratch.folder.getChildFile ("ownership.wav");
        if (! require (writeTone (tone), "temporary audio")) return;
        const auto cue = projectWith (tone, 1).cues()[0];
        AudioEngine engine (0);
        engine.prepare (sampleRate, blockSize, 2);
        auto& players = engine.*auditMember (PlayersMember {});
        juce::AudioBuffer<float> block (2, blockSize);
        if (! require (engine.play (cue) && engine.play (cue) && engine.load (cue) && engine.load (cue), "overlap retired/current playback and LOAD")) return;
        auto& chain = engine.getCueChain (cue.id);
        auto plugin = std::make_unique<TestGainPlugin> (0.0f, 0.2);
        auto* probe = plugin.get();
        chain.addPlugin (std::move (plugin));
        expectEquals ((int) players.size(), 4);
        for (const auto& player : players)
            expect (player->getChain() == (player->isStopPending() ? nullptr : &chain), "only live instances own the newly created chain");
        engine.renderBlock (block, blockSize);
        expectEquals (probe->processCount, 1, "only the latest playing instance processes; LOAD is silent");
        if (! require (engine.play (cue), "GO starts the latest LOAD")) return;
        const int before = probe->processCount;
        engine.renderBlock (block, blockSize);
        expectEquals (probe->processCount - before, 1, "GO hands the chain to the loaded instance exactly once");
        engine.renderBlock (block, blockSize);
        expectWithinAbsoluteError (block.getRMSLevel (0, 0, blockSize), 0.0f, 1.0e-7f);
        expectWithinAbsoluteError (chain.getTailSeconds(), 0.2, 1.0e-6, "insertion refreshes the tail cache");
        probe->tail = 0.3;
        probe->setLatencyLive (32);
        expect (engine.consumePluginStateChanges());
        expectEquals (chain.getLatencySamples(), 32);
        expectWithinAbsoluteError (chain.getTailSeconds(), 0.3 + 32.0 / sampleRate, 1.0e-6, "state changes refresh tail including latency");
        engine.removeCueChain (cue.id);
        for (const auto& player : players) expect (player->getChain() == nullptr);
        expect (engine.findCueChain (cue.id) == nullptr);
        auto& recreated = engine.getCueChain (cue.id);
        recreated.addPlugin (std::make_unique<TestGainPlugin> (0.0f));
        engine.renderBlock (block, blockSize);
        expectWithinAbsoluteError (block.getRMSLevel (0, 0, blockSize), 0.0f, 1.0e-7f, "recreated chain takes effect without restarting");
        engine.clearCueChains();
        for (const auto& player : players) expect (player->getChain() == nullptr);
        expect (engine.findCueChain (cue.id) == nullptr);
        engine.renderBlock (block, blockSize);
        expectGreaterThan (block.getRMSLevel (0, 0, blockSize), 0.3f, "clearing restores dry playback");
        engine.stop (cue.id);
        engine.getCueChain (cue.id);
        for (const auto& player : players) expect (player->getChain() == nullptr, "a pending stop never gains a new chain");
        engine.shutdown();
    }

    void childFocus()
    {
        beginTest ("audit0923 ED-3: child MIDI editor loses focus and releases panic capture");
       #if JUCE_WINDOWS
        const auto foreground = GetForegroundWindow();
        const auto nativeFocus = GetFocus();
        Fixture f;
        const auto tone = f.scratch.folder.getChildFile ("playing.wav");
        if (! require (writeTone (tone), "temporary playing audio")) return;
        auto project = projectWith (tone, 1);
        project.settings.panicSeconds = 0.0;
        const auto cue = project.cues()[0];
        if (! require (f.open (project), "open real project")) return;
        auto& service = f.main->getShortcutService();
        MidiTrigger panic;
        panic.number = 60;
        panic.channel = 1;
        panic.debounceMs = 0;
        if (! require (service.setMidiTriggers ("transport.panicAll", { panic }).wasOk(), "register MIDI panic")) return;
        auto midi = ReopenLastProjectTestAccess::midiCallbacks (*f.main);
        constexpr uint64_t port = 0xa0923;
        midi.connection (port, 1, true);
        const auto send = [&] (const juce::MidiMessage& message)
        {
            MidiInputEvent event;
            MidiInputEvent::copyMessage (message, event);
            event.input = port;
            event.connection = 1;
            event.routing = service.getInputGeneration();
            event.observedTimeMs = juce::Time::getMillisecondCounterHiRes();
            event.eventID = InputInvocation::nextEventID();
            midi.receive (event, "Audit MIDI", true);
        };
        const auto tapPanic = [&]
        {
            send (juce::MidiMessage::noteOff (1, 60));
            send (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
            send (juce::MidiMessage::noteOff (1, 60));
            f.runFor (1.3);
        };
        if (! require (f.controller().fire (cue.id) == CueController::GoResult::started, "start audio before learning")) return;
        f.render();
        if (! require (f.engine.isPlaying (cue.id), "audio really playing")) return;

        // Use the real settings tab with the real main-component MIDI router,
        // avoiding WorkspaceSettingsDialog's visible native window factory.
        ShortcutSettingsTab tab (service, f.document());
        tab.setLookAndFeel (&f.theme);
        tab.setSize (900, 720);
        HiddenDesktop desktop (tab);
        if (! require (desktop.isHidden() && tab.isShowing(), "hidden, disabled peer with a logically showing component tree")) return;
        auto* learn = findChild<juce::TextButton> (tab, [] (const auto& b) { return b.getButtonText() == ko ("학습") && b.isShowing(); });
        if (! require (learn != nullptr && (bool) learn->onClick, "real learn-row button")) return;
        const auto learnClick = learn->onClick;
        learnClick();
        auto* capture = findChild<KeyCapture> (tab);
        if (! require (capture != nullptr && capture->isCapturing(), "learning started by settings UI")) return;
        focusWithin (*capture);
        send (juce::MidiMessage::controllerEvent (1, 64, 0));
        send (juce::MidiMessage::controllerEvent (1, 64, 127));
        send (juce::MidiMessage::controllerEvent (1, 64, 0));
        auto* high = findChild<juce::TextEditor> (*capture, [] (const auto& e) { return e.getComponentID() == "midiHigh"; });
        auto* low = findChild<juce::TextEditor> (*capture, [] (const auto& e) { return e.getComponentID() == "midiLow"; });
        if (! require (high != nullptr && low != nullptr && high->isShowing() && high->isEnabled(), "learned CC exposes numeric editors")) return;
        focusWithin (*high);
        drainMessages();
        expect (capture->isCapturing() && high->hasKeyboardFocus (false), "capture retained from parent to child");
        focusWithin (*low);
        drainMessages();
        expect (capture->isCapturing() && low->hasKeyboardFocus (false), "capture retained between numeric children");
        focusWithin (*high);
        drainMessages();
        // Same public JUCE peer boundary used by WM_KILLFOCUS, without SetFocus.
        tab.getPeer()->handleFocusLoss();
        if (! require (! capture->hasKeyboardFocus (true), "focus leaves the entire learning widget")) return;
        expect (capture->isCapturing(), "child focus cancellation waits for the asynchronous recheck");
        drainMessages();
        const bool widgetCapture = capture->isCapturing(), serviceCapture = service.isCapturing();
        tapPanic();
        const bool stillPlaying = f.engine.isPlaying (cue.id);
        logMessage ("OBS ED-3: widget capture=" + juce::String ((int) widgetCapture)
                    + "; service capture=" + juce::String ((int) serviceCapture)
                    + "; playing after MIDI panic=" + juce::String ((int) stillPlaying));
        expect (! widgetCapture && ! serviceCapture, "Leaving the learning widget must cancel capture");
        expect (! stillPlaying, "Previously registered MIDI panic must stop the playing audio");
        if (capture->isCapturing())
        {
            auto* cancel = findChild<juce::TextButton> (*capture, [] (const auto& b) { return b.getButtonText() == ko ("취소"); });
            if (! require (cancel != nullptr && (bool) cancel->onClick, "learning Cancel button")) return;
            const auto cancelClick = cancel->onClick;
            cancelClick();
        }
        if (! f.engine.isPlaying (cue.id)) { f.controller().fire (cue.id); f.render(); }
        if (! require (f.engine.isPlaying (cue.id), "control audio playing before post-cancel panic")) return;
        tapPanic();
        expect (! service.isCapturing() && ! f.engine.isPlaying (cue.id), "Control: cancel restores the same MIDI panic");
        expect (desktop.isHidden() && GetForegroundWindow() == foreground && GetFocus() == nativeFocus,
                "test leaves every native window hidden and OS focus unchanged");
       #endif
    }

    void focusExceptions()
    {
        beginTest ("ED-3: asynchronous focus recheck preserves returned focus, popup, submission and a new capture generation");
       #if JUCE_WINDOWS
        Fixture f;
        auto& service = f.main->getShortcutService();
        KeyCapture capture (service, [] { return false; });
        capture.setSize (600, 370);
        HiddenDesktop desktop (capture);
        if (! require (desktop.isHidden(), "hidden capture peer")) return;
        auto* high = findChild<juce::TextEditor> (capture, [] (const auto& e) { return e.getComponentID() == "midiHigh"; });
        if (! require (high != nullptr, "numeric child for focus exceptions")) return;
        const auto start = [&]
        {
            MidiTrigger cc;
            cc.kind = MidiTrigger::Kind::cc;
            cc.number = 64;
            capture.start ("GO", false, cc);
            focusWithin (*high);
        };
        start();
        capture.getPeer()->handleFocusLoss();
        expect (capture.isCapturing(), "focus cancellation is deferred");
        focusWithin (*high);
        drainMessages();
        expect (capture.isCapturing() && service.isCapturing(), "returned focus survives asynchronous recheck");

        juce::Component popup; // a modal marker with no desktop peer or native window
        popup.enterModalState (false);
        capture.getPeer()->handleFocusLoss();
        drainMessages();
        expect (capture.isCapturing() && service.isCapturing(), "popup focus keeps learning active");
        popup.exitModalState (0);
        focusWithin (*high);
        drainMessages();

        KeyCapture::Completion completion;
        capture.onSubmit = [&] (const auto&, auto done) { completion = std::move (done); };
        auto* submit = findChild<juce::TextButton> (capture, [] (const auto& b) { return b.getButtonText() == ko ("등록"); });
        if (! require (submit != nullptr && submit->isEnabled() && (bool) submit->onClick, "ready registration button")) return;
        submit->onClick();
        if (! require ((bool) completion, "asynchronous submission is pending")) return;
        capture.getPeer()->handleFocusLoss();
        drainMessages();
        expect (capture.isCapturing() && service.isCapturing(), "submitting capture survives focus loss");
        completion (juce::Result::ok());
        expect (! capture.isCapturing() && ! service.isCapturing());

        start();
        capture.getPeer()->handleFocusLoss();
        capture.start ("new generation");
        drainMessages();
        expect (capture.isCapturing() && service.isCapturing(), "an old deferred loss cannot cancel a new generation");
        capture.cancel();
       #endif
    }
};

AuditFix0923EngineTests auditFix0923EngineTests;
} // namespace
} // namespace gocue::tests
