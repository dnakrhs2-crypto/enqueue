#include "MainComponentTestAccess.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/KeyCapture.h"
#include "ui/ShortcutSettingsTab.h"

#include <cmath>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
namespace
{
#if JUCE_WINDOWS
constexpr double sampleRate = 44100.0;
constexpr int blockSize = 512;

// Only ED-3 and its required helpers are adapted from
// audit0923_ref/Audit0923EnqMoreTests.cpp. Use the r2 hidden-peer event path
// instead of the reference's visible windows and native focus acquisition.
template <class Tag, typename Tag::Type member>
struct AuditMemberAccess { friend typename Tag::Type auditMember (Tag) { return member; } };
struct DocumentMember
{
    using Type = ProjectDocument MainComponent::*;
    friend Type auditMember (DocumentMember);
};
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
template struct AuditMemberAccess<DocumentMember, &MainComponent::document>;
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
    MSG message {};
    for (int n = 0; n < 2000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
}
bool pumpUntil (const std::function<bool()>& ready, int timeoutMs = 1000)
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
void pumpFor (int durationMs)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + durationMs;
    pumpUntil ([&] { return juce::Time::getMillisecondCounterHiRes() >= end; }, durationMs + 100);
}

struct Scratch
{
    Scratch() { folder.createDirectory(); }
    ~Scratch()
    {
        if (folder.isAChildOf (base) && folder.getFileName().startsWith ("AuditFix0923CaptureFocus-"))
            folder.deleteRecursively();
    }
    const juce::File base = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File folder = base.getChildFile ("AuditFix0923CaptureFocus-" + juce::Uuid().toString());
};

bool writeTone (const juce::File& file)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (! stream) return false;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate)
                                         .withNumChannels (2).withBitsPerSample (16));
    if (! writer) return false;
    juce::AudioBuffer<float> data (2, 44100);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < data.getNumSamples(); ++i)
            data.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));
    for (int second = 0; second < 30; ++second)
        if (! writer->writeFromAudioSampleBuffer (data, 0, data.getNumSamples())) return false;
    return true;
}

Project projectWith (const juce::File& file)
{
    Project project;
    project.name = "AuditFix0923 Capture Focus";
    project.settings.autoBackup = false;
    project.settings.backupBeforeSave = false;
    project.settings.copyFilesIntoProject = false;
    project.settings.autoLoadNewCues = false;
    project.settings.panicSeconds = 0.0;
    auto& patch = project.ensureDefaultPatch();
    patch.numCueOutputs = 2;
    patch.sanitise();
    Cue cue;
    cue.name = "Playing";
    cue.number = "1";
    cue.file = file;
    cue.numChannels = 2;
    cue.durationSeconds = 30.0;
    cue.autoLoad = false;
    cue.levels.resize (2, 2);
    cue.levels.setDefaults();
    project.ensureMainList().cues.push_back (cue);
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
    AppSettings settings; // Explicit temporary storage, never the user's AppData.
    AudioEngine engine { 0 }; // No physical audio device or disk-read thread.
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    juce::AudioBuffer<float> out { 2, blockSize };
};

// The disabled peer is parented to a never-shown, nonactivating host.
// Only JUCE's logical visibility is true; no native window can be shown or focused.
struct HiddenDesktop
{
    explicit HiddenDesktop (juce::Component& content) : root (content)
    {
        host = CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Audit hidden host",
                                WS_POPUP | WS_DISABLED, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
        if (host == nullptr) return;
        root.setVisible (true); // no peer yet, and the host will never be shown
        root.addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresKeyPresses, host);
        if (auto* peer = root.getPeer())
        {
            EnableWindow ((HWND) peer->getNativeHandle(), FALSE);
            peer->setVisible (false);
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

class AuditFix0923CaptureFocusTests final : public juce::UnitTest
{
public:
    AuditFix0923CaptureFocusTests() : UnitTest ("AuditFix0923 Capture Focus", "Enqueue") {}
    void runTest() override
    {
       #if JUCE_WINDOWS
        inactivePopupPanic (false);
        inactivePopupPanic (true);
        foregroundPopupAndSubmission();
       #else
        beginTest ("ED-3 hidden-peer focus transitions require Windows");
        logMessage ("The native focus fixture is Windows-only.");
       #endif
    }
private:
   #if JUCE_WINDOWS
    bool require (bool condition, const juce::String& reason)
    {
        expect (condition, "Fixture prerequisite: " + reason);
        return condition;
    }
    void inactivePopupPanic (bool afterPopupDeferral)
    {
        beginTest (afterPopupDeferral ? "ED-3: app deactivation during popup retry releases capture and MIDI panic while the modal remains"
                                     : "ED-3: app deactivation before the asynchronous check releases capture and MIDI panic while the modal remains");
        bool appForeground = true;
        const juce::ScopedValueSetter<std::function<bool()>> foregroundCheck (KeyCapture::foregroundProcessCheck, [&] { return appForeground; });
        const auto foreground = GetForegroundWindow();
        const auto nativeFocus = GetFocus();
        Fixture f;
        const auto tone = f.scratch.folder.getChildFile ("playing.wav");
        if (! require (writeTone (tone), "temporary playing audio")) return;
        const auto project = projectWith (tone);
        const auto cue = project.cues()[0];
        if (! require (f.open (project), "open real temporary project")) return;
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

        ShortcutSettingsTab tab (service, f.document());
        tab.setLookAndFeel (&f.theme);
        tab.setSize (900, 720);
        HiddenDesktop desktop (tab);
        if (! require (desktop.isHidden() && tab.isShowing(), "hidden peer with a logically showing settings tree")) return;
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
        if (! require (high != nullptr && high->isShowing() && high->isEnabled(), "learned CC exposes a numeric editor")) return;
        focusWithin (*high);
        drainMessages();
        expect (capture->isCapturing() && high->hasKeyboardFocus (false), "child editing retains capture");

        juce::Component popup, outside;
        HiddenDesktop outsideDesktop (outside);
        if (! require (outsideDesktop.isHidden(), "second hidden component tree")) return;
        popup.enterModalState (false); // peerless context-menu marker, kept modal through the panic
        if (! require (juce::Component::getCurrentlyModalComponent() == &popup, "popup is modal before focus leaves")) return;
        tab.getPeer()->handleFocusLoss();
        focusWithin (outside);
        if (! require (! capture->hasKeyboardFocus (true), "focus leaves the entire learning widget")) return;
        expect (capture->isCapturing() && service.isCapturing(), "cancellation waits for asynchronous delivery");
        if (afterPopupDeferral)
        {
            pumpFor (100);
            expect (capture->isCapturing() && service.isCapturing(), "foreground popup defers cancellation through timer retries");
        }
        appForeground = false; // No popup dismissal or further focus notification.
        expect (pumpUntil ([&] { return ! capture->isCapturing() && ! service.isCapturing(); }),
                "app deactivation cancels widget and service capture even with a retained modal");
        expect (juce::Component::getCurrentlyModalComponent() == &popup, "popup is still modal after the asynchronous cancellation check");
        expect (outside.hasKeyboardFocus (false), "focus never returns to the capture");
        tapPanic();
        expect (! capture->isCapturing() && ! service.isCapturing(), "capture remains released after MIDI input");
        expect (! f.engine.isPlaying (cue.id), "previously registered MIDI panic stops the real playing audio");
        expect (juce::Component::getCurrentlyModalComponent() == &popup, "panic executes before the modal is dismissed");
        popup.exitModalState (0);
        capture->cancel();
        expect (desktop.isHidden() && outsideDesktop.isHidden() && GetForegroundWindow() == foreground && GetFocus() == nativeFocus,
                "all native windows remain hidden and OS focus is unchanged");
    }

    void foregroundPopupAndSubmission()
    {
        beginTest ("ED-3: foreground internal popup selection and focus return preserve learning");
        bool appForeground = true;
        const juce::ScopedValueSetter<std::function<bool()>> foregroundCheck (KeyCapture::foregroundProcessCheck, [&] { return appForeground; });
        const auto foreground = GetForegroundWindow();
        const auto nativeFocus = GetFocus();
        Fixture f;
        auto& service = f.main->getShortcutService();
        KeyCapture capture (service, [] { return false; });
        capture.setSize (600, 370);
        HiddenDesktop desktop (capture);
        if (! require (desktop.isHidden(), "hidden capture peer")) return;
        MidiTrigger cc;
        cc.kind = MidiTrigger::Kind::cc;
        cc.number = 64;
        capture.start ("GO", false, cc);
        auto* high = findChild<juce::TextEditor> (capture, [] (const auto& e) { return e.getComponentID() == "midiHigh"; });
        auto* preset = findChild<juce::ComboBox> (capture, [] (const auto& c) { return c.getComponentID() == "midiPreset"; });
        if (! require (high != nullptr && preset != nullptr, "real numeric editor and preset combo")) return;
        focusWithin (*high);
        drainMessages();
        juce::Component popup;
        popup.enterModalState (false, juce::ModalCallbackFunction::create ([&] (int result)
        {
            preset->setSelectedId (result, juce::sendNotificationSync);
            focusWithin (*high);
        }));
        capture.getPeer()->handleFocusLoss();
        pumpFor (100);
        expect (juce::Component::getCurrentlyModalComponent() == &popup && ! capture.hasKeyboardFocus (true), "popup owns the modal while capture focus is outside");
        expect (capture.isCapturing() && service.isCapturing(), "foreground internal popup preserves learning through timer rechecks");
        popup.exitModalState (2);
        pumpFor (100);
        expect (capture.isCapturing() && service.isCapturing() && high->hasKeyboardFocus (false), "popup selection and focus return preserve learning");
        const auto* selected = std::get_if<MidiTrigger> (&capture.model().candidate());
        expect (preset->getSelectedId() == 2 && selected != nullptr && selected->number == 64 && selected->edge == MidiTrigger::Edge::both,
                "real combo callback applies the toggle preset to the learned rule");

        beginTest ("ED-3: submitting exception survives background activation with and without a retained popup");
        KeyCapture::Completion completion;
        capture.onSubmit = [&] (const auto&, auto done) { completion = std::move (done); };
        auto* submit = findChild<juce::TextButton> (capture, [] (const auto& b) { return b.getButtonText() == ko ("등록"); });
        if (! require (submit != nullptr && submit->isEnabled() && (bool) submit->onClick, "ready registration button")) return;
        popup.enterModalState (false);
        capture.getPeer()->handleFocusLoss(); // Queued before submitting becomes true.
        submit->onClick();
        if (! require ((bool) completion, "asynchronous submission is pending")) return;
        appForeground = false;
        pumpFor (100);
        expect (capture.isCapturing() && service.isCapturing() && juce::Component::getCurrentlyModalComponent() == &popup,
                "submitting capture survives app deactivation and a retained modal");
        popup.exitModalState (0);
        capture.getPeer()->handleFocusLoss();
        pumpFor (100);
        expect (capture.isCapturing() && service.isCapturing(), "submitting capture also survives app deactivation without a popup");
        completion (juce::Result::ok());
        expect (! capture.isCapturing() && ! service.isCapturing(), "successful completion releases capture");
        expect (desktop.isHidden() && GetForegroundWindow() == foreground && GetFocus() == nativeFocus,
                "popup selection and submission leave native windows hidden and OS focus unchanged");
    }
   #endif
};

AuditFix0923CaptureFocusTests auditFix0923CaptureFocusTests;
} // namespace
} // namespace gocue::tests
