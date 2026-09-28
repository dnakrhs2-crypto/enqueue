#include "TestSupport.h"
#include "app/RecorderSettings.h"
#include "app/CoupangShortcut.h"
#include "ui/MainComponent.h"
#include "ui/UpdateNotice.h"

namespace gocue::recorder
{
struct UpdateNoticeTestAccess
{
    static void stopTimer(MainComponent& main) { main.stopTimer(); }
    static void importStarting(MainComponent& main, bool busy) { main.importStarting = busy; }
    static void settingsPending(MainComponent& main, bool busy) { main.settingsPending = busy; }
    static void startupOpen(MainComponent& main, bool busy) { main.promptAfterStartupOpen = busy; }
    static void configured(MainComponent& main, const UserSettings& snapshot)
    { main.session.onConfigured(juce::Result::ok(), snapshot); }
    static void fileWork(MainComponent& main, bool busy)
    {
        if (busy)
        {
            std::promise<MainComponent::FileResult> work;
            main.fileWork = work.get_future();
            work.set_value({}); // even completed work must be collected before the notice
        }
        else main.fileWork = {};
    }
};
}

int runRecorderUpdateTests()
{
    using namespace gocue;
    using namespace gocue::recorder;
    using recorder_test::require;
    recorder_test::Suite tests;
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("RecorderUpdate-" + juce::Uuid().toString());
    tests.test("lastRunVersion survives the PropertySet XML round trip without changing validation", [&]
    {
        RecorderSettings settings(root.getChildFile("roundtrip"));
        require(settings.load().wasOk() && settings.get().lastRunVersion.isEmpty(), "Fresh settings");
        auto state = settings.get(); state.lastRunVersion = "0.1.12";
        require(state.validate().wasOk() && settings.set(state).wasOk(), "Version validation");
        require(settings.save().get().wasOk(), "Settings save");
        RecorderSettings reopened(root.getChildFile("roundtrip"));
        require(reopened.load().wasOk() && reopened.get().lastRunVersion == "0.1.12", "Version round trip");
        state.lastRunVersion = "legacy-version";
        require(state.validate().wasOk(), "Version must not change validation");
    });
    tests.test("A legacy settings file without the version key still loads with an empty version", [&]
    {
        RecorderSettings settings(root.getChildFile("legacy"));
        require(settings.save().get().wasOk(), "Legacy fixture save");
        const auto xml = juce::parseXML(settings.getFile());
        require(xml != nullptr, "Legacy XML");
        juce::PropertySet properties; properties.restoreFromXml(*xml);
        properties.removeValue("lastRunVersion");
        require(settings.getFile().replaceWithText(properties.createXml("RECORDER_SETTINGS")->toString()), "Remove version key");
        RecorderSettings reopened(root.getChildFile("legacy"));
        require(reopened.load().wasOk() && reopened.get().lastRunVersion.isEmpty(), "Legacy settings load");
    });
    tests.test("Tally update decision distinguishes legacy settings from a first installation", [&]
    {
        for (bool existed : {false, true})
            for (bool shortcut : {false, true})
                for (const auto& previous : juce::StringArray{"", "2.0", "1.0"})
                {
                    const auto decision = CoupangShortcut::decideUpdate(previous, "2.0", shortcut, existed);
                    const bool announce = previous == "1.0" || (previous.isEmpty() && existed);
                    require(decision.announce == announce, "Tally announce decision");
                    require(decision.offerShortcut == (announce && !shortcut), "Tally offer decision");
                }
    });
    tests.test("Tally alert uses the shared mouse-only action with OK owning Enter and Esc", [&]
    {
        RecorderLookAndFeel style;
        auto alert = UpdateNotice::create(style, ProductIdentity::displayName() + ko("가 2.0(으)로 업데이트되었습니다."), true);
        require(alert->getNumButtons() == 2, "Two buttons");
        auto* action = alert->getButton(1);
        require(action->getCommandID() == 2 && !action->getWantsKeyboardFocus(), "Mouse-only action");
        for (int key : {juce::KeyPress::returnKey, juce::KeyPress::escapeKey})
        {
            require(alert->getButton(0)->isRegisteredForShortcut(juce::KeyPress(key)), "OK key");
            require(!action->isRegisteredForShortcut(juce::KeyPress(key)), "No action key");
        }
        for (const auto letter : CoupangShortcut::buttonText)
            require(!action->isRegisteredForShortcut(juce::KeyPress(int(letter))), "No letter shortcut");
    });
    tests.test("Tally waits for every lifecycle activity but permits unsaved idle edits", [&]
    {
        RecorderSettings settings(root.getChildFile("gates"));
        RecorderDocument document;
        MainComponent main(document, settings);
        UpdateNoticeTestAccess::stopTimer(main);
        const auto lifecycle = main.lifecycleState();
        require(main.canShowUpdateNotice(), "Idle notice");
        lifecycle->set(RecorderLifecycle::unsaved, true);
        require(main.canShowUpdateNotice(), "Unsaved idle notice");
        for (const auto activity : {RecorderLifecycle::recording, RecorderLifecycle::dubbing, RecorderLifecycle::exporting,
            RecorderLifecycle::recovering, RecorderLifecycle::finalizing, RecorderLifecycle::fileWork,
            RecorderLifecycle::configuring, RecorderLifecycle::closing})
        {
            lifecycle->set(activity, true);
            require(!main.canShowUpdateNotice(), "Busy notice deferred");
            lifecycle->set(activity, false);
        }
        auto capture = std::make_shared<std::atomic<bool>>(true);
        lifecycle->bindCaptureBlocker(capture);
        require(!main.canShowUpdateNotice(), "Capture notice deferred");
        capture->store(false);
        require(main.canShowUpdateNotice(), "Idle after capture");
    });
    tests.test("Tally waits for uncollected file work, import startup, settings and startup project work", [&]
    {
        RecorderSettings settings(root.getChildFile("work"));
        RecorderDocument document;
        MainComponent main(document, settings);
        UpdateNoticeTestAccess::stopTimer(main);
        for (const auto setBusy : {UpdateNoticeTestAccess::importStarting, UpdateNoticeTestAccess::settingsPending,
                                  UpdateNoticeTestAccess::startupOpen, UpdateNoticeTestAccess::fileWork})
        {
            setBusy(main, true);
            require(!main.canShowUpdateNotice(), "Pending work notice deferred");
            setBusy(main, false);
            require(main.canShowUpdateNotice(), "Collected work notice ready");
        }
    });
    tests.test("An asynchronous device settings snapshot cannot roll back lastRunVersion", [&]
    {
        const auto settingsRoot = root.getChildFile("device-snapshot");
        RecorderSettings settings(settingsRoot);
        auto previous = settings.get(); previous.lastRunVersion = "1.0";
        require(settings.set(previous).wasOk(), "Previous version");
        RecorderDocument document;
        MainComponent main(document, settings);
        UpdateNoticeTestAccess::stopTimer(main);
        auto current = settings.get(); current.lastRunVersion = "2.0";
        require(settings.set(current).wasOk(), "Current version");
        UpdateNoticeTestAccess::configured(main, previous);
        require(settings.get().lastRunVersion == "2.0", "Callback preserved current version");
        require(settings.save().get().wasOk(), "Version save after callback");
        RecorderSettings reopened(settingsRoot);
        require(reopened.load().wasOk() && reopened.get().lastRunVersion == "2.0", "Persisted current version");
    });
    tests.test("Tally waits for another modal without displaying a native window", [&]
    {
        RecorderSettings settings(root.getChildFile("modal"));
        RecorderDocument document;
        MainComponent main(document, settings);
        UpdateNoticeTestAccess::stopTimer(main);
        juce::Component modal;
        modal.enterModalState(false, nullptr, false);
        require(!main.canShowUpdateNotice(), "Modal notice deferred");
        modal.exitModalState(0);
        require(main.canShowUpdateNotice(), "Modal cleared");
    });
    if (root.isAChildOf(parent) && root.getFileName().startsWith("RecorderUpdate-")) root.deleteRecursively();
    return tests.result("RecorderUpdateTests");
}
