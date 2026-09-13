// Included once by TestMain.cpp; no separate main and no hardware startup.
#include <juce_gui_extra/juce_gui_extra.h>
#include "TestSupport.h"
#include "AudioRenderFixtures.h"
#include "ui/MainComponent.h"
#include "ui/ExportDialog.h"
#include "support/CrashHandler.h"
#include <system_error>
#include <chrono>
#include <thread>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "dbghelp.lib")
#define RECORDER_HAS_WINSPARKLE 0
#include "../src/ui/MainComponent.cpp"
#include "../src/ui/ProjectDialogs.cpp"
#include "../src/ui/ExportDialog.cpp"
#include "../src/ui/CameraSettingsPanel.cpp"
#include "../src/ui/DemoAutomation.cpp"
#include "../src/ui/RecorderLookAndFeel.cpp"
#include "../src/app/RecorderUpdater.cpp"
#include "../src/support/CrashHandler.cpp"

namespace gocue::recorder
{
struct ShortcutExceptionTestAccess
{
    static RecorderSession& session(MainComponent& main) { return main.session; }
    static void placement(RecorderSession& session, TakeController::Config& config) { session.configurePlacement(config); } // tab rule + live markers/names in the placement edit
    static void stopTimers(MainComponent& main) { main.stopTimer(); main.exportDialog->stopTimer(); }
    static void tick(MainComponent& main) { main.timerCallback(); }
    static void refresh(MainComponent& main) { main.refresh(); }
    static void settings(MainComponent& main) { main.showSettings(); }
    static bool settingsVisible(MainComponent& main) { return main.settingsWindow && main.settingsWindow->isVisible(); }
    static SettingsForm& settingsForm(MainComponent& main) { return *static_cast<SettingsForm*>(main.settingsWindow->getContentComponent()); }
    static ExportDialog& exporting(MainComponent& main) { return *main.exportDialog; }
    static juce::TextButton& exportFocus(MainComponent& main) { return main.exportDialog->openFolder; }
    static juce::TextEditor& exportText(MainComponent& main) { return main.exportDialog->folder; }
    static void allowStartButton(MainComponent& main) { main.recordView.startButton.setEnabled(true); }
    static juce::String banner(MainComponent& main) { return main.banner; }
    static void publish(MainComponent& main) { main.publishLifecycle(); }
    static void checkForUpdates(MainComponent& main) { main.checkForUpdates(); }
    static void clearBanner(MainComponent& main) { main.banner = {}; }
    static juce::Rectangle<int> timelineBounds(MainComponent& main) { return main.timelineView.getBounds(); }
    static juce::String exceptionBanner(MainComponent& main) { return main.exceptionBanner; }
    static void releaseKey(MainComponent& main) { main.heldShortcut = {}; }
    static bool listenerOn(MainComponent& main, juce::Component* origin) { return main.shortcutFocus == origin; }
    static juce::AlertWindow* marker(MainComponent& main) { return main.markerWindow.getComponent(); }
    static void markerButton(MainComponent& main) { main.recordView.markerButton.onClick(); }
    static void timelineMarker(MainComponent& main) { main.timelineView.onAddMarkerRequested(); }
    static bool projectVisible(MainComponent& main) { return main.projectWindow && main.projectWindow->isVisible(); }
    static NewProjectForm& projectForm(MainComponent& main) { return *static_cast<NewProjectForm*>(main.projectWindow->getContentComponent()); }
    static void newProject(MainComponent& main) { main.newProjectDialog(); }
    static juce::Rectangle<int> importProgress(MainComponent& main) { return main.audioImporter.getBounds(); }
    static RecordView& recordView(MainComponent& main) { return main.recordView; }
    static const std::vector<Marker>& queuedMarkers(MainComponent& main) { return main.session.recordedMarkers; }
    static void cursor(MainComponent& main, Sample at) { main.session.cursor = at; }
    static void mockPreview(MainComponent& main)
    {
        std::promise<std::shared_ptr<ExportDialog::Preview>> promise;
        main.exportDialog->previewWork = promise.get_future(); promise.set_exception(std::make_exception_ptr(std::runtime_error("injected preview")));
    }
    static void openAudio(RecorderSession& session, unsigned Fs = 8000)
    {
        recorder_test::require(session.audio.openSynthetic(Fs, 80, 0, 2).wasOk(), "Open synthetic audio");
        session.device = session.audio.deviceInfo();
    }
    static void failLaunch(MainComponent& main)
    { main.beforeWorkerStart = [](const char*) { throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "injected UI launch"); }; }
    static bool startFile(MainComponent& main)
    { return main.startFileWork({}, [] { return MainComponent::FileResult{}; }); }
    static void failFile(MainComponent& main, std::exception_ptr error)
    {
        main.pendingFile.written = main.document.snapshot(); main.pendingFile.file = main.settings.getFile().getSiblingFile("failed.recorder");
        std::promise<MainComponent::FileResult> promise; main.fileWork = promise.get_future(); promise.set_exception(error);
    }
    static bool filePending(MainComponent& main) { return main.fileWork.valid(); }
    static void persist(MainComponent& main) { main.persistSettings(); }
    static bool settingsPending(MainComponent& main) { return main.settingsWork.valid() || main.settingsPending; }
    static void failSettings(MainComponent& main, std::exception_ptr error)
    {
        std::promise<juce::Result> promise; main.settingsWork = promise.get_future(); promise.set_exception(error);
    }
    static juce::Result failDeviceStart(RecorderSession& session)
    {
        session.beforeWorkerStart = [](const char*) { throw std::runtime_error("injected device launch"); };
        return session.startDeviceWork([] { return RecorderSession::DeviceResult{}; });
    }
    static void failDeviceGet(RecorderSession& session, std::exception_ptr error)
    {
        session.lifecycle->set(RecorderLifecycle::configuring, true); session.notice = "configuring";
        std::promise<RecorderSession::DeviceResult> promise; session.deviceWork = promise.get_future(); promise.set_exception(error);
    }
    static void failRelease(RecorderSession& session, bool launch)
    {
        if (launch) session.beforeWorkerStart = [](const char*) { throw std::runtime_error("injected release launch"); };
        else { std::promise<void> promise; session.releaseWork = promise.get_future(); promise.set_exception(std::make_exception_ptr(42)); }
    }
};
}

namespace shortcut_exception_tests
{
using namespace gocue::recorder;
using recorder_test::require;
using Access = ShortcutExceptionTestAccess;
MarkerColourSwatches& markerSwatches(juce::AlertWindow& prompt)
{
    require(prompt.getNumCustomComponents() == 1, "Marker dialog must contain its colour swatches");
    auto* colours = dynamic_cast<MarkerColourSwatches*>(prompt.getCustomComponent(0));
    require(colours != nullptr, "Marker custom component is not the colour palette"); return *colours;
}
juce::Button& markerSwatch(juce::AlertWindow& prompt, const juce::String& hex)
{
    for (auto* child : markerSwatches(prompt).getChildren())
        if (auto* button = dynamic_cast<juce::Button*>(child); button && button->getComponentID() == hex) return *button;
    throw std::runtime_error("Requested dialog swatch missing");
}
struct Folder
{
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("recorder-keys-" + newId());
    ~Folder()
    {
        if (root.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory)
            && root.getFileName().startsWith("recorder-keys-")) root.deleteRecursively();
    }
};
void pump()
{
    MSG message{};
    for (unsigned n = 0; n < 100 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n)
    { TranslateMessage(&message); DispatchMessageW(&message); }
}
template<class F> void until(F condition)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!condition()) { require(std::chrono::steady_clock::now() < end, "Exception regression timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
juce::String labels(const juce::Component& component)
{
    juce::String result;
    if (const auto* label = dynamic_cast<const juce::Label*>(&component)) result += label->getText();
    for (auto* child : component.getChildren()) result += labels(*child);
    return result;
}
void snapshot(juce::Component& component, const char* name)
{
    const auto path = juce::SystemStats::getEnvironmentVariable("RECORDER_TEST_SCREENSHOT_ROOT", {});
    if (!juce::File::isAbsolutePath(path)) return;
    const juce::File folder(path); require(folder.createDirectory().wasOk(), "Screenshot directory");
    RecorderLookAndFeel theme;
    auto* previous = &component.getLookAndFeel(); component.setLookAndFeel(&theme);
    const auto image = component.createComponentSnapshot(component.getLocalBounds());
    component.setLookAndFeel(previous);
    juce::FileOutputStream output(folder.getChildFile(name));
    require(output.openedOk() && juce::PNGImageFormat().writeImageToStream(image, output), "Write UI screenshot");
}
struct CameraFixture
{
    CameraFixture()
    {
        exception_test::cameraWorker = []
        { std::promise<std::vector<CameraDevice>> promise; auto future = promise.get_future(); promise.set_value({}); return future; };
    }
    ~CameraFixture() { exception_test::cameraWorker = {}; }
};
class Video final : public ITakeVideoStream
{
public:
    void prepare(const juce::File& file, NvencProfile, Rational, const AVCodecContext&) override
    { require(file.replaceWithText("synthetic shortcut fixture, not encoded media"), "Video fixture file"); }
    void startAt(ClockMapping, std::int64_t, unsigned, std::function<std::int64_t()>) override {}
    void offer(const VideoSurface&) noexcept override {}
    void audioPacket(const AVPacket&) override {}
    bool ready() const noexcept override { return true; }
    void sourceFailed(std::int64_t n) noexcept override { end = n; }
    void endAt(std::int64_t n) noexcept override { end = n; }
    void audioDone() noexcept override {}
    void finish() override {}
    bool failed() const noexcept override { return false; }
    std::int64_t availableSamples() const noexcept override { return end; }
    bool thumbnailReady() const noexcept override { return false; }
    juce::var report() const override { return {}; }
private:
    std::int64_t end = 0;
};
struct MainFixture
{
    Folder folder;
    RecorderDocument document;
    RecorderSettings settings{folder.root};
    MainComponent main{document, settings, [] { return std::make_unique<Video>(); }};
    RecorderSession& session = Access::session(main);
    Sample position = 0;
    std::uint64_t sequence = 0;
    std::int64_t firstQpc = qpcNow();
    MainFixture() { Access::stopTimers(main); document.newProject("shortcut fixture", 8000, {30, 1}); }
    void feed()
    {
        float left[80]{}, right[80]{}; float* output[]{left, right};
        BlockStamp stamp{}; stamp.flags = samplePositionValid | latenciesValid; stamp.sequence = sequence++;
        stamp.samplePosition = position; stamp.sampleRate = 8000; stamp.numSamples = 80; stamp.callbackQpc = firstQpc + position * qpcFrequency() / 8000;
        session.audioEngine().processBlock(stamp, nullptr, 0, nullptr, output, 2); position += 80;
    }
    void begin()
    {
        Access::openAudio(session); for (int i = 0; i < 4; ++i) feed();
        until([&] { return session.audioEngine().clockReady(); });
        TakeController::Config config; config.projectDirectory = folder.root; config.synthetic = true; config.projectFps = 30;
        config.cameraMode.width = 1920; config.cameraMode.height = 1080; config.cameraMode.fps = {30, 1};
        ShortcutExceptionTestAccess::placement(session, config); // 0.1.6: the session reserves the position (record tab = active end) and folds live markers into the placement edit
        auto& take = session.takeController(); require(take.prepare(config).wasOk(), "Prepare synthetic recording");
        until([&] { take.tick(); return take.state() == TakeController::State::armed; });
        require(take.start(position + 81).wasOk(), "Start synthetic recording");
        until([&] { return session.audioEngine().startCommitted(); });
        until([&] { feed(); take.tick(); return take.state() == TakeController::State::recording; });
    }
    void finish()
    {
        auto& take = session.takeController();
        if (take.state() == TakeController::State::recording) require(take.stop().wasOk(), "Finish synthetic recording");
        until([&] { feed(); take.tick(); return take.shutdownComplete(); });
    }
};
void focus(MainComponent& main, juce::Component& target)
{
    pump(); // finish posted form text updates before assigning the test focus
    target.setEnabled(true); target.grabKeyboardFocus();
    until([&] { pump(); return juce::Component::getCurrentlyFocusedComponent() == &target && Access::listenerOn(main, &target); });
}
class ThrowingTimer final : public juce::Timer
{
public:
    int calls = 0;
private:
    void timerCallback() override
    { stopTimer(); ++calls; throw std::runtime_error("injected unexpected timer exception"); }
};
}

int runUnexpectedJuceExceptionTests(bool timer)
{
    using namespace shortcut_exception_tests;
    recorder_test::Suite suite;
    juce::ScopedJuceInitialiser_GUI gui;
    bool continued = false;
    suite.test(timer ? "Unexpected timer callback" : "Unexpected callAsync callback", [&]
    {
        if (timer)
        {
            ThrowingTimer callback; callback.startTimer(1);
            until([&] { pump(); return callback.calls == 1; });
        }
        else
        {
            require(juce::MessageManager::callAsync([] { throw std::runtime_error("injected unexpected async exception"); }), "Queue injection");
        }
        require(juce::MessageManager::callAsync([&] { continued = true; }), "Queue continuation after injection");
        until([&] { pump(); return continued; });
    });
    suite.test("Exception dispatch continued", [&] { require(continued, "Message loop did not continue"); });
    return suite.result("unhandled-injection");
}

int runShortcutExceptionTests()
{
    using namespace shortcut_exception_tests;
    recorder_test::Suite suite;
    juce::ScopedJuceInitialiser_GUI gui;
    suite.test("Shared Space routes by recording state, suppresses repeats, and leaves text input alone", []
    {
        const juce::KeyPress space(juce::KeyPress::spaceKey);
        RecorderShortcuts bindings;
        require(shortcutCommand(bindings, space, nullptr, true) == RecorderCommand::recordStop, "Recording must select stop");
        require(shortcutCommand(bindings, space, nullptr, false) == RecorderCommand::playStop, "Idle must select play/stop");
        juce::TextEditor text; juce::Component child; text.addChildComponent(child);
        require(!shortcutCommand(bindings, space, &text, true) && !shortcutCommand(bindings, space, &child, false), "Text consumes Space");
        // 0.1.6: Play at/after the active end does nothing (the cursor may mark the next take), so the idle check needs
        // a timeline with content and the cursor at its start.
        recorder_audio_fixture::Fixture source(8000); auto project = source.project;
        project.tracks.erase(project.tracks.begin(), project.tracks.begin() + 2);
        MainFixture f; require(f.document.adopt(project, f.folder.root.getChildFile("project.recorder"), {}).wasOk(), "Existing timeline fixture");
        f.session.projectChanged(); f.session.scrub(0, true);
        require(f.main.routeShortcut(space, &f.main) && f.session.playing(), "Idle Space did not request playback");
        require(f.main.routeShortcut(space, &f.main) && f.session.playing(), "Held Space toggled playback twice");
        Access::releaseKey(f.main);
        require(f.main.routeShortcut(space, &f.main) && !f.session.playing(), "Released then pressed Space did not stop playback");
    });
    suite.test("Shortcut settings capture accepts the shared stop key and explains it", []
    {
        UserSettings settings; settings.shortcuts.keys[std::size_t(RecorderCommand::recordStop)] = "F8";
        ShortcutSettingsPanel panel(settings);
        juce::Button* stop = nullptr;
        for (auto* child : panel.getChildren()) if (auto* button = dynamic_cast<juce::Button*>(child);
            button && button->getTitle() == RecorderShortcuts::name(RecorderCommand::recordStop)) stop = button;
        require(stop != nullptr, "Record stop capture exists"); stop->onClick();
        require(static_cast<juce::Component*>(stop)->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)), "Capture did not accept Space");
        require(panel.validate().wasOk() && panel.read(settings).shortcuts[RecorderCommand::recordStop] == "spacebar", "Shared key rejected");
        require(labels(panel).contains(ko("녹화 중엔 정지")), "Shared-key hint missing");
    });
    suite.test("Marker button confirms the captured cursor and preserves the entered UTF-8 name", []
    {
        MainFixture f; Access::cursor(f.main, 2345); Access::markerButton(f.main);
        auto* prompt = Access::marker(f.main); require(prompt && prompt->isCurrentlyModal(), "Asynchronous marker prompt missing");
        require(markerSwatches(*prompt).selected() == "#4c8dff", "Marker dialog does not default to blue");
        require(prompt->getDescription().contains(ko("마커 이름을 입력하세요. 위치 ") + formatMarkerTime(2345, f.document.getProject().Fs)), "Marker prompt omitted the captured time");
        auto* editor = prompt->getTextEditor("markerName"); focus(f.main, *editor);
        require(editor->getText() == ko("마커 1") && editor->getHighlightedRegion().getLength() == editor->getText().length(), "Default name must be fully selected");
        snapshot(*prompt, "marker-prompt.png");
        const auto name = ko("  도입 · 후렴 → 끝  "); editor->setText(name); Access::cursor(f.main, 9876);
        prompt->triggerButtonClick(ko("확인")); until([&] { pump(); return Access::marker(f.main) == nullptr; });
        require(f.document.getProject().markers.size() == 1 && f.document.getProject().markers[0].sample == 2345
            && f.document.getProject().markers[0].name == name && f.document.getProject().markers[0].colour == "#4c8dff", "Confirm changed the captured sample, name or default colour");
    });
    suite.test("Marker dialog colour swatch supports keyboard selection and confirms the chosen colour", []
    {
        MainFixture f; Access::cursor(f.main, 72000); Access::markerButton(f.main);
        auto* prompt = Access::marker(f.main); require(prompt != nullptr, "Marker prompt missing");
        require(markerSwatches(*prompt).selected() == "#4c8dff", "Default blue swatch missing");
        auto& red = markerSwatch(*prompt, "#e0443a"); focus(f.main, red);
        require(red.getWantsKeyboardFocus() && red.getTitle() == ko("빨강"), "Dialog colour is not keyboard accessible");
        require(red.getPeer()->handleKeyPress(juce::KeyPress::returnKey, 0), "Swatch did not accept keyboard activation");
        until([&] { pump(); require(Access::marker(f.main) == prompt, "Keyboard colour selection confirmed the dialog"); return markerSwatches(*prompt).selected() == "#e0443a"; });
        require(Access::marker(f.main) == prompt && f.document.getProject().markers.empty(), "Choosing colour confirmed the dialog");
        prompt->triggerButtonClick(ko("확인")); until([&] { pump(); return Access::marker(f.main) == nullptr; });
        require(f.document.getProject().markers.size() == 1 && f.document.getProject().markers[0].sample == 72000
            && f.document.getProject().markers[0].colour == "#e0443a", "Confirmed marker lost the picked colour or captured position");
    });
    suite.test("Timeline marker hook and M share one prompt; Enter defaults, Escape and cancel discard", []
    {
        MainFixture f; Access::timelineMarker(f.main); const juce::Component::SafePointer<juce::AlertWindow> first(Access::marker(f.main));
        Access::markerButton(f.main); Access::timelineMarker(f.main); f.main.routeShortcut(juce::KeyPress('M'), &f.main);
        require(first && Access::marker(f.main) == first.getComponent() && f.document.getProject().markers.empty(), "Repeated request opened or added twice");
        auto* editor = first->getTextEditor("markerName"); focus(f.main, *editor); editor->setText("   ");
        require(editor->getPeer()->handleKeyPress(juce::KeyPress::returnKey, 0), "Enter did not reach marker confirmation");
        until([&] { pump(); return Access::marker(f.main) == nullptr; });
        require(f.document.getProject().markers.size() == 1 && f.document.getProject().markers[0].name == ko("마커 1"), "Empty name fallback failed");
        Access::releaseKey(f.main); require(f.main.routeShortcut(juce::KeyPress('M'), &f.main), "M did not request marker prompt");
        auto* second = Access::marker(f.main); require(second && second->getTextEditorContents("markerName") == ko("마커 2"), "M default sequence");
        editor = second->getTextEditor("markerName"); focus(f.main, *editor);
        editor->getPeer()->handleKeyPress(juce::KeyPress::escapeKey, 0);
        until([&] { pump(); return Access::marker(f.main) == nullptr; });
        Access::markerButton(f.main); Access::marker(f.main)->triggerButtonClick(ko("취소"));
        until([&] { pump(); return Access::marker(f.main) == nullptr; });
        require(f.document.getProject().markers.size() == 1, "Cancel or Escape added a marker");
    });
    suite.test("Recording continues while naming a marker, Space stays text, and placement uses request time", []
    {
        recorder_audio_fixture::Fixture source(8000); auto project = source.project;
        project.tracks.erase(project.tracks.begin(), project.tracks.begin() + 2);
        MainFixture f; require(f.document.adopt(project, f.folder.root.getChildFile("project.recorder"), {}).wasOk(), "Existing timeline fixture");
        f.begin();
        require(f.session.takeController().placementSample() > 0, "Recording fixture needs a nonzero placement");
        const auto at = f.session.takeController().placementSample() + f.session.elapsed();
        Access::markerButton(f.main); auto* prompt = Access::marker(f.main); require(prompt != nullptr, "Recording marker prompt missing");
        markerSwatch(*prompt, "#2bb5b5").onClick();
        auto* editor = prompt->getTextEditor("markerName"); focus(f.main, *editor); editor->setText(ko("녹화 지점"));
        editor->getPeer()->handleKeyPress(juce::KeyPress::spaceKey, ' ');
        require(!f.main.routeShortcut(juce::KeyPress(juce::KeyPress::spaceKey), editor), "Marker text leaked Space");
        for (unsigned n = 0; n < 40; ++n) { f.feed(); Access::tick(f.main); pump(); }
        require(f.session.takeController().state() == TakeController::State::recording
            && f.session.takeController().placementSample() + f.session.elapsed() > at, "Modal prompt blocked recording ticks");
        const auto name = editor->getText(); prompt->triggerButtonClick(ko("확인"));
        until([&] { pump(); return Access::marker(f.main) == nullptr; });
        const auto& queued = Access::queuedMarkers(f.main);
        require(queued.size() == 1 && queued[0].sample == at && queued[0].name == name && queued[0].colour == "#2bb5b5", "Live marker lost request time, name or colour");
        require(f.document.getProject().markers.empty(), "Live marker published before placement");
        f.finish(); Access::tick(f.main);
        require(f.document.getProject().markers.size() == 1 && f.document.getProject().markers[0].sample == at
            && f.document.getProject().markers[0].name == name && f.document.getProject().markers[0].colour == "#2bb5b5", "Placed take lost the named marker or colour");
    });
    suite.test("Project replacement and shutdown dismiss marker prompts and invalidate pending confirmation", []
    {
        MainFixture f; Access::markerButton(f.main);
        const juce::Component::SafePointer<juce::AlertWindow> old(Access::marker(f.main));
        old->triggerButtonClick(ko("확인")); f.document.newProject("replacement", 8000, {30, 1}); pump();
        require(!old && !Access::marker(f.main) && f.document.getProject().markers.empty(), "Stale confirmation reached replacement project");
        Access::markerButton(f.main); const juce::Component::SafePointer<juce::AlertWindow> closing(Access::marker(f.main));
        bool closed = false; f.main.requestClose([&] { closed = true; });
        until([&] { pump(); Access::tick(f.main); return closed; });
        require(!closing && f.document.getProject().markers.empty(), "Closing retained or confirmed marker prompt");
    });
    suite.test("Startup without a project opens a focused form, cancel keeps recording gated, and layout fits", []
    {
        MainFixture f; f.main.initialiseProject({}, true, false);
        require(Access::projectVisible(f.main), "First startup did not request a project");
        auto& form = Access::projectForm(f.main); focus(f.main, form.name);
        snapshot(form, "new-project-form.png");
        require(form.folder.getText().isEmpty() && form.open.getButtonText() == ko("기존 프로젝트 열기…"), "First-run folder or open button wrong");
        form.cancel.onClick(); Access::refresh(f.main);
        require(!Access::projectVisible(f.main) && !f.session.readyToRecord() && labels(Access::recordView(f.main)).contains(ko("프로젝트 > 새 프로젝트")), "Cancel must keep project guidance and record gate");
        for (const int width : {960, 1180, 1600})
        {
            f.main.setSize(width, 780); auto& view = Access::recordView(f.main); const auto progress = Access::importProgress(f.main);
            require(progress.getX() > view.markerButton.getRight() && progress.getWidth() >= 400
                && progress.getRight() <= view.getWidth() - 12, "Compact import overlaps buttons or has no usable width");
            require(view.importButton.getY() == view.settingsButton.getY()
                && view.timelineTab.getRight() < view.importButton.getX() && view.importButton.getRight() < view.settingsButton.getX(), "Import is not between the view tabs and settings");
        }
        require(Access::recordView(f.main).stopButton.getTooltip() == ko("녹화 정지 · spacebar"), "Stop tooltip encoding/default");
        require(RecorderUpdater::aboutText().startsWith(juce::String::fromUTF8(RECORDER_DISPLAY_NAME)), "About identity encoding");
    });
    suite.test("Failed recent startup opens a form with existing parent folder; suppressed modes stay quiet", []
    {
        for (const bool interactive : {true, false})
        {
            MainFixture f; require(f.folder.root.createDirectory().wasOk(), "Recent folder fixture");
            f.settings.rememberProject(f.folder.root.getChildFile("missing.recorder"));
            f.main.initialiseProject({}, interactive, false);
            until([&] { Access::tick(f.main); pump(); return !Access::filePending(f.main); });
            require(Access::projectVisible(f.main) == interactive, "Recent failure startup policy wrong");
            if (interactive) require(Access::projectForm(f.main).folder.getText() == f.folder.root.getFullPathName(), "Recent parent not prefilled");
            require(f.session.deviceInfo().sampleRate == 0, "Isolated startup opened audio");
        }
        MainFixture isolated; isolated.main.initialiseProject({}, false, false);
        require(!Access::projectVisible(isolated.main), "Test/automation startup opened a form");
        MainFixture explicitPath; explicitPath.main.initialiseProject(explicitPath.folder.root.getChildFile("absent.recorder"), true, false);
        until([&] { Access::tick(explicitPath.main); pump(); return !Access::filePending(explicitPath.main); });
        require(!Access::projectVisible(explicitPath.main), "Explicit open incorrectly fell back to startup prompt");
    });
    suite.test("Successful recent startup skips the form and a pending device connection does not block a new form", []
    {
        MainFixture recent; const auto path = recent.folder.root.getChildFile("project.recorder");
        require(recent.document.saveCheckpoint(path).wasOk(), "Recent checkpoint fixture");
        recent.settings.rememberProject(path); recent.main.initialiseProject({}, true, false);
        until([&] { Access::tick(recent.main); pump(); return !Access::filePending(recent.main); });
        require(recent.document.getFile() == path && !Access::projectVisible(recent.main), "Successful recent open displayed a startup prompt");
        MainFixture connecting; Access::failDeviceGet(connecting.session, std::make_exception_ptr(std::runtime_error("synthetic startup connection")));
        connecting.main.initialiseProject({}, true, false);
        require(connecting.session.configuring() && Access::projectVisible(connecting.main), "Startup form waited for device completion");
        Access::tick(connecting.main);
        require(!connecting.session.configuring() && Access::projectVisible(connecting.main), "Device callback hid the project form");
    });
    suite.test("Focused nonmodal export control forwards Space to the live take stop", []
    {
        MainFixture f; f.begin();
        Access::settings(f.main); require(!Access::settingsVisible(f.main), "Settings opened during recording");
        auto& exporting = Access::exporting(f.main); exporting.show();
        auto& button = Access::exportFocus(f.main); focus(f.main, button);
        require(!f.main.isParentOf(&button) && exporting.ownsShortcutOrigin(&button), "Export must be a separate owned top-level window");
        require(button.getPeer()->handleKeyPress(juce::KeyPress::spaceKey, 0), "Native peer did not handle Space");
        require(f.session.takeController().state() == TakeController::State::stopping, "Space did not call TakeController::stop");
        f.finish();
    });
    suite.test("Visible settings reject F9 and permit Space from main and settings controls", []
    {
        CameraFixture camera;
        for (bool settingsFocus : {true, false})
        {
            MainFixture f; Access::settings(f.main); require(Access::settingsVisible(f.main), "Settings fixture not visible");
            snapshot(Access::settingsForm(f.main), "settings-form.png");
            f.begin(); Access::allowStartButton(f.main);
            require(!f.main.routeShortcut(juce::KeyPress(juce::KeyPress::F9Key), &f.main), "F9 escaped settings gate");
            require(f.session.takeController().state() == TakeController::State::recording, "Ignored F9 changed recording");
            if (settingsFocus)
            {
                auto& button = Access::settingsForm(f.main).close; focus(f.main, button);
                require(button.getPeer()->handleKeyPress(juce::KeyPress::spaceKey, 0), "Settings peer did not route Space");
            }
            else require(f.main.routeShortcut(juce::KeyPress(juce::KeyPress::spaceKey), &f.main), "Settings visibility blocked main Space routing");
            require(f.session.takeController().state() == TakeController::State::stopping, "Space did not stop while settings remained visible"); f.finish();
        }
    });
    suite.test("Export text input and shortcut capture focus do not execute recording keys", []
    {
        MainFixture f; f.begin(); auto& exporting = Access::exporting(f.main); exporting.show();
        auto& editor = Access::exportText(f.main); focus(f.main, editor);
        editor.getPeer()->handleKeyPress(juce::KeyPress::spaceKey, 0);
        require(f.session.takeController().state() == TakeController::State::recording, "Text input stopped recording");
        juce::Component child; editor.addChildComponent(child);
        require(!f.main.routeShortcut(juce::KeyPress(juce::KeyPress::spaceKey), &child), "Text editor descendant escaped exclusion");
        ShortcutSettingsPanel captures(f.settings.get()); exporting.addAndMakeVisible(captures);
        juce::Button* capture = nullptr;
        for (auto* c : captures.getChildren()) if (auto* button = dynamic_cast<juce::Button*>(c); button && button->getButtonText() == "F9") capture = button;
        require(capture && capture->onClick, "Capture button fixture"); capture->onClick();
        require(!f.main.routeShortcut(juce::KeyPress(juce::KeyPress::spaceKey), capture), "Capture widget executed stop");
        juce::TextButton unrelated;
        require(!f.main.routeShortcut(juce::KeyPress(juce::KeyPress::spaceKey), &unrelated), "Unowned window routed a shortcut"); f.finish();
    });
    suite.test("Export preview preparation retains its recording start gate", []
    {
        MainFixture f; Access::mockPreview(f.main); Access::allowStartButton(f.main);
        require(!f.main.routeShortcut(juce::KeyPress(juce::KeyPress::F9Key), &f.main), "Preview allowed F9");
        require(!f.main.routeShortcut(juce::KeyPress(juce::KeyPress::spaceKey), &f.main) && !f.session.showingPlayback(), "Preview allowed competing timeline ASIO playback");
        require(Access::exporting(f.main).previewActive() && Access::banner(f.main).isEmpty(), "F9 cancelled preview or attempted record");
    });
    suite.test("Unhandled recording error remains visible with stop and save guidance", []
    {
        MainFixture f; f.begin(); const auto report = f.folder.root.getChildFile("sample-exception.txt");
        f.main.showUnhandledException(report); f.main.showError("later device banner"); Access::refresh(f.main);
        const auto visible = labels(f.main);
        require(visible.contains(ko("예상치 못한 오류가 기록됐습니다: sample-exception.txt")), "Report banner hidden by normal status");
        require(visible.contains(ko("녹화를 정지하고 프로젝트를 저장하세요")), "Recording recovery guidance missing"); f.finish();
    });
    suite.test("A notice that appears after layout gets its 26 px row in both tabs; clearing it gives the row back", []
    {
        // The notice row is 0 px tall while empty. MainComponent re-applies identical bounds on every refresh, which never
        // reaches RecordView::resized(), so the view itself re-runs its layout when the notice appears or clears.
        MainFixture f; auto armed = f.settings.get(); armed.physicalInputs = {0, -1, -1, -1, -1, -1, -1, -1}; armed.microphoneArmed[0] = true; // no armed microphone would itself fill the notice row
        require(f.settings.set(armed).wasOk() && f.document.saveCheckpoint(f.folder.root.getChildFile("layout/project.recorder")).wasOk(), "Saved project fixture with an armed microphone");
        f.main.setSize(960, 640); auto& view = Access::recordView(f.main);
        for (const bool timelineTab : {false, true})
        {
            if (timelineTab) view.timelineTab.onClick(); else Access::refresh(f.main); // the tab click refreshes; an empty project prepares no playback
            require(view.noticeBounds().getHeight() == 0, ("Baseline notice present: " + labels(f.main)).toRawUTF8());
            const auto plain = view.timelineBounds(); require(plain.getHeight() > 100 && Access::timelineBounds(f.main) == plain, "Baseline lower-area layout");
            f.main.showError("later device banner"); Access::refresh(f.main);
            require(view.noticeBounds().getHeight() == 26 && labels(f.main).contains("later device banner"), "Notice row did not appear at the same window size");
            require(view.noticeBounds().getBottom() <= view.timelineBounds().getY() && Access::timelineBounds(f.main) == view.timelineBounds(), "Lower area not re-laid out under the notice row");
            // Record tab: the camera cards absorb the row. Timeline tab: the timeline itself gives up the room.
            require(!timelineTab || view.timelineBounds().getHeight() < plain.getHeight(), "Timeline tab did not make room for the notice");
            Access::clearBanner(f.main); Access::refresh(f.main);
            require(view.noticeBounds().getHeight() == 0 && view.timelineBounds() == plain && Access::timelineBounds(f.main) == plain, "Cleared notice did not give the row back");
        }
    });
    suite.test("An untouched app allows updates; unsaved recorded work blocks them and the reason names it", []
    {
        // RecorderDocument starts dirty and newProject() keeps it dirty, so the empty default project must not count
        // as unsaved work for the update gate (the CEO hit '…복구와 저장이 끝난 뒤' without ever creating a project).
        MainFixture f; auto& lifecycle = *f.session.lifecycleState();
        require(f.document.isDirty() && f.document.getFile() == juce::File(), "Fixture is the empty never-saved project");
        Access::publish(f.main); require(lifecycle.canShutdown(), "Empty default project blocked the updater");
        Access::checkForUpdates(f.main); Access::refresh(f.main);
        require(!Access::banner(f.main).contains(ko("끝난 뒤")) && !Access::banner(f.main).contains(ko("저장하지 않은")), ("Empty project refused the update: " + Access::banner(f.main)).toRawUTF8());
        // Recording and importing both need a project folder, so unsaved work in practice is a saved project with edits.
        const auto file = f.folder.root.getChildFile("gate/project.recorder"); require(f.document.saveCheckpoint(file).wasOk(), "Save the project");
        Access::publish(f.main); require(!f.document.isDirty() && lifecycle.canShutdown(), "Freshly saved project blocked the updater");
        Marker marker; marker.sample = 8000; marker.name = "m"; require(f.document.addMarker(marker).wasOk() && f.document.isDirty(), "Edit the saved project");
        Access::publish(f.main); require(!lifecycle.canShutdown(), "Unsaved edits must block the updater");
        Access::checkForUpdates(f.main); Access::refresh(f.main);
        require(Access::banner(f.main).contains(ko("저장하지 않은 변경")) && !Access::banner(f.main).contains(ko("복구")), "Unsaved reason must name saving only");
        require(f.document.saveCheckpoint(file).wasOk(), "Save again");
        Access::publish(f.main); require(lifecycle.canShutdown(), "Saved project still blocked the updater");
        using L = RecorderLifecycle;
        const auto text = MainComponent::updateBlockedText(L::recording | L::fileWork | L::unsaved, false);
        require(text.contains(ko("녹화")) && text.contains(ko("저장")) && text.contains(ko("저장하지 않은 변경")) && !text.contains(ko("더빙")), "Blocked text must list only the active blockers");
        require(MainComponent::updateBlockedText(0, true).contains(ko("오디오 장치 사용")), "Capture ownership must be named");
    });
    suite.test("Device work launch and future exceptions clear configuring and notify failure", []
    {
        RecorderDocument document; RecorderSession session(document); int failures = 0;
        session.onConfigured = [&](const juce::Result& r, const UserSettings&) { require(r.failed(), "Failure callback claimed success"); ++failures; };
        require(Access::failDeviceStart(session).failed(), "Device launch threw or succeeded");
        for (const auto& error : {std::make_exception_ptr(std::runtime_error("injected device future")), std::make_exception_ptr(42)})
        { Access::failDeviceGet(session, error); session.tick(); }
        require(failures == 3 && !session.configuring() && !(session.lifecycleState()->snapshot() & RecorderLifecycle::configuring), "Configuring lifecycle remained set");
        require(session.notice.isEmpty() && session.error.isNotEmpty(), "Configuration failure banner missing");
    });
    suite.test("Shutdown release launch and future failures still release synthetic resources", []
    {
        for (bool launch : {true, false})
        {
            RecorderDocument document; RecorderSession session(document); Access::openAudio(session);
            session.requestShutdown(); require(session.readyForShutdownCommit(), "Shutdown commit fixture"); session.releaseForShutdown();
            Access::failRelease(session, launch); session.tick();
            require(session.shutdownComplete() && session.audioEngine().deviceInfo().sampleRate == 0, "Failed release stranded devices");
            require(session.error.isNotEmpty(), "Release failure was silent");
        }
    });
    suite.test("UI file and settings launch failures show a banner and release lifecycle gates", []
    {
        MainFixture f; Access::failLaunch(f.main);
        require(!Access::startFile(f.main) && !Access::filePending(f.main), "Failed file launch retained work");
        require(Access::banner(f.main).contains("injected UI launch"), "File launch banner missing");
        Access::persist(f.main); require(!Access::settingsPending(f.main), "Failed settings launch retained pending state");
        require(Access::banner(f.main).contains("injected UI launch") && !(f.session.lifecycleState()->snapshot() & RecorderLifecycle::fileWork), "Settings launch banner/lifecycle wrong");
    });
    suite.test("UI file and settings futures contain standard and nonstandard exceptions", []
    {
        for (const auto& error : {std::make_exception_ptr(std::runtime_error("injected future")), std::make_exception_ptr(42)})
        {
            MainFixture f; Access::failFile(f.main, error); Access::tick(f.main);
            require(!Access::filePending(f.main) && Access::banner(f.main).isNotEmpty(), "Failed file future escaped or stayed pending");
            Access::failSettings(f.main, error); Access::tick(f.main);
            require(!Access::settingsPending(f.main) && !(f.session.lifecycleState()->snapshot() & RecorderLifecycle::fileWork), "Failed settings future retained lifecycle gate");
        }
    });
    suite.test("Camera enumeration launch and future failures display status without hardware", []
    {
        CameraFixture reset;
        exception_test::cameraWorker = []() -> std::future<std::vector<CameraDevice>> { throw std::runtime_error("injected camera launch"); };
        CameraSettingsPanel launch({}, RecorderProject{});
        require(!launch.scanning() && labels(launch).contains("injected camera launch"), "Camera launch stuck scanning");
        for (const auto& error : {std::make_exception_ptr(std::runtime_error("injected camera future")), std::make_exception_ptr(42)})
        {
            exception_test::cameraWorker = [error] { std::promise<std::vector<CameraDevice>> promise; auto future = promise.get_future(); promise.set_exception(error); return future; };
            CameraSettingsPanel future({}, RecorderProject{});
            until([&] { pump(); return !future.scanning(); });
            require(labels(future).contains(ko("카메라 목록을 읽을 수 없습니다")), "Camera future failure was silent");
        }
    });
    suite.test("Saturated transport pause returns a failed Result and detaches playback", []
    {
        recorder_audio_fixture::Fixture source(8000); auto project = source.project;
        project.tracks.erase(project.tracks.begin(), project.tracks.begin() + 2); // metadata-only cameras are not opened
        RecorderDocument document; require(document.adopt(project, source.root.getChildFile("project.recorder"), {}).wasOk(), "Playback fixture");
        RecorderSession session(document); Access::openAudio(session); session.enterTimeline(true);
        until([&] { session.tick(); return session.showingPlayback() || session.error.isNotEmpty(); });
        require(session.showingPlayback(), "Synthetic playback preparation failed");
        float left[80]{}, right[80]{}; float* output[]{left, right}; BlockStamp stamp{}; stamp.sampleRate = 8000; stamp.numSamples = 80; stamp.flags = samplePositionValid; stamp.callbackQpc = qpcNow();
        session.audioEngine().processBlock(stamp, nullptr, 0, nullptr, output, 2); // acknowledge transport generation, then stop consuming commands
        auto result = juce::Result::ok();
        for (unsigned n = 0; n < 128 && result.wasOk(); ++n) result = session.pause();
        require(result.failed() && result.getErrorMessage().contains("Transport command queue full"), "Queue saturation escaped or was not exercised");
        require(!session.showingPlayback() && !session.playing(), "Rejected pause left ASIO playback attached");
    });
    suite.test("JUCE message queue exceptions write reports and later callbacks continue", []
    {
        Folder folder; std::vector<juce::File> reports; int later = 0;
        recorder_test::ExpectedUnhandledExceptions expected(2, [&](const std::exception* e, const juce::String& file, int line)
        { CrashHandler::handleException(e, file, line, [&](const juce::File& report) { reports.push_back(report); }, folder.root); });
        require(juce::MessageManager::callAsync([] { throw std::runtime_error("injected queued exception"); }), "Queue first callback");
        require(juce::MessageManager::callAsync([&] { ++later; }), "Queue continuation");
        require(juce::MessageManager::callAsync([] { throw 42; }), "Queue unknown exception");
        require(juce::MessageManager::callAsync([&] { ++later; }), "Queue final continuation");
        until([&] { pump(); return later == 2; });
        require(expected.calls() == 2 && reports.size() == 2 && reports[0] != reports[1], "Exception reports lost/overwritten");
        for (const auto& report : reports)
        {
            require(report.existsAsFile() && report.getFileName().startsWith("Recorder-" + ProductIdentity::version()) && report.getFileName().endsWith("-exception.txt"), "Report filename/file missing");
            const auto text = report.loadFileAsString().replace("\r\n", "\n");
            require(text.contains("file:") && text.contains("juce_Messaging_windows.cpp") && text.contains("line:") && text.contains("stack:\n") && text.fromFirstOccurrenceOf("stack:\n", false, false).trim().isNotEmpty(), "Report source/line/backtrace missing");
        }
        require(reports[0].loadFileAsString().contains("injected queued exception") && reports[1].loadFileAsString().contains("Unknown non-standard exception"), "Exception what() missing");
    });
    for (const auto* injection : {"inject-unhandled-async", "inject-unhandled-timer"})
        suite.test(injection, [injection]
        {
            juce::ChildProcess child;
            require(child.start(juce::StringArray{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(), "--suite", injection}), "Start exception injection subprocess");
            require(child.waitForProcessToFinish(15000), "Exception injection subprocess timed out");
            const auto output = child.readAllProcessOutput();
            require(child.getExitCode() == 1, "Unhandled GUI exception did not fail the process");
            require(output.contains("unhandled-injection: 1 passed, 1 failed") && output.contains("1 unexpected JUCE exceptions"), "GUI exception was not counted against its test and process");
            require(output.contains("injected unexpected") && output.contains("PASS Exception dispatch continued"), "Wrong failure or callbacks did not continue");
        });
    suite.test("Ctrl+S saves the project by default and stays out of text fields", []
    {
        require(RecorderShortcuts{}[RecorderCommand::saveProject] == "ctrl + S" && RecorderShortcuts{}.validate().wasOk(), "Default save shortcut");
        MainFixture f;
        const auto file = f.folder.root.getChildFile("save/project.recorder"); require(f.document.saveCheckpoint(file).wasOk(), "Save the project");
        Marker marker; marker.sample = 8000; marker.name = "saved-by-ctrl-s"; require(f.document.addMarker(marker).wasOk() && f.document.isDirty(), "Edit the saved project");
        const juce::KeyPress save('S', juce::ModifierKeys::ctrlModifier, 0);
        juce::TextEditor typing; require(!f.main.routeShortcut(save, &typing) && f.document.isDirty(), "Ctrl+S inside a text field must not save");
        require(f.main.routeShortcut(save, &f.main), "Ctrl+S was not handled"); Access::releaseKey(f.main);
        until([&] { Access::tick(f.main); return !f.document.isDirty(); });
        RecorderProject reloaded;
        require(RecorderSerializer::readCheckpoint(file, reloaded).wasOk() && reloaded.markers.size() == 1 && reloaded.markers[0].name == "saved-by-ctrl-s", "Ctrl+S did not write the edit to disk");
    });
    suite.test("A crashing test process exits with the exception code, reports the test and shows no dialog", []
    {
        const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        const auto crashDirectory = exe.getSiblingFile("crash");
        const auto before = crashDirectory.findChildFiles(juce::File::findFiles, false, "RecorderTests-*.txt");
        juce::ChildProcess child;
        require(child.start(juce::StringArray{exe.getFullPathName(), "--suite", "inject-access-violation"}), "Start crash fixture");
        // Without the guard a "memory could not be read" box would hold the child here until somebody clicks it.
        require(child.waitForProcessToFinish(15000), "Crashed fixture did not exit within 15 s (a dialog is probably waiting)");
        const auto output = child.readAllProcessOutput();
#ifdef __SANITIZE_ADDRESS__
        std::cout << "  (AddressSanitizer build: the sanitizer's own SEGV handler ends the child, so only the prompt exit is checked)\n";
        return;
#endif
        require(child.getExitCode() == 0xC0000005u, "Exit status is not STATUS_ACCESS_VIOLATION");
        require(output.contains("inject-access-violation: faulting deliberately")
            && output.contains("RecorderTests CRASH (unhandled exception) in test: Deliberate access violation")
            && output.contains("exception 0xC0000005 at"), "Crash report missing from the fixture output");
        const auto after = crashDirectory.findChildFiles(juce::File::findFiles, false, "RecorderTests-*.txt");
        require(after.size() == before.size() + 1, "Exactly one new crash report expected");
        juce::File report; for (const auto& f : after) if (!before.contains(f)) report = f;
        const auto dump = report.withFileExtension("dmp");
        require(report.loadFileAsString().contains("in test: Deliberate access violation") && dump.getSize() > 4096, "Report text or minidump missing");
        require(report.deleteFile() && dump.deleteFile(), "Fixture cleanup");
    });
    suite.test("Exception reporter handles unwritable destination and notification reentry", []
    {
        Folder folder; require(folder.root.createDirectory().wasOk(), "Reporter fixture root");
        const auto blocked = folder.root.getChildFile("file-not-directory"); require(blocked.replaceWithText("fixture"), "Blocked destination fixture");
        const auto nested = folder.root.getChildFile("nested-reports");
        int notifications = 0, nestedNotifications = 0;
        juce::File returned = blocked;
        CrashHandler::handleException(nullptr, "source.cpp", 123, [&](const juce::File& report)
        {
            ++notifications; returned = report;
            CrashHandler::handleException(nullptr, "nested.cpp", 1, [&](const juce::File&) { ++nestedNotifications; }, nested);
        }, blocked);
        require(notifications == 1 && returned == juce::File(), "Failed report notification/path wrong");
        require(nestedNotifications == 0 && !nested.exists(), "Reentrant reporter wrote a file or invoked notification");
        juce::File laterReport;
        CrashHandler::handleException(nullptr, "later.cpp", 2, [&](const juce::File& report)
        { ++nestedNotifications; laterReport = report; }, nested);
        require(nestedNotifications == 1 && laterReport.existsAsFile() && laterReport.getParentDirectory() == nested, "Reporter reentry guard did not reset");
        require(CrashHandler::directory() == ProductIdentity::settingsDirectory().getChildFile("crash"), "Production report directory wrong");
    });
    return suite.result("shortcut-exceptions");
}
