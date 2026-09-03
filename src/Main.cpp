#include "app/AppSettings.h"
#include "app/Updater.h"
#include "audio/AudioEngine.h"
#include "audio/PluginHost.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/MainComponent.h"
#include "ui/UiUtils.h"

#include <juce_gui_extra/juce_gui_extra.h>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue
{

#if JUCE_WINDOWS
namespace
{
    std::function<void()> escapeHandler;
    HHOOK escapeHook = nullptr;

    LRESULT CALLBACK escapeHookProc (int code, WPARAM wParam, LPARAM lParam)
    {
        // a key-down that is not an auto-repeat (bit 30: previous state, bit 31: transition)
        if (code == HC_ACTION && wParam == VK_ESCAPE && (lParam & 0xC0000000) == 0)
            juce::MessageManager::callAsync ([] { if (escapeHandler) escapeHandler(); });

        return CallNextHookEx (nullptr, code, wParam, lParam);
    }
}
#endif

class GoCueApplication : public juce::JUCEApplication,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    GoCueApplication() = default;

    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return false; }   // a project file opened from Explorer lands in the running window

    void initialise (const juce::String& commandLine) override
    {
        lookAndFeel = std::make_unique<GoCueLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        settings = std::make_unique<AppSettings>();
        engine = std::make_unique<AudioEngine>();

        // safe mode (Shift held at launch, or --safe-mode): the saved device is not opened (a hung driver would keep
        // the window from ever appearing) and plugins are not instantiated; the operator picks a device by hand
        safeMode = juce::ArgumentList ("Enqueue", commandLine).containsOption ("--safe-mode")
                   || juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
        PluginHost::setSafeMode (safeMode);

        const auto savedDeviceState = safeMode ? nullptr : settings->getAudioDeviceState();
        const auto deviceError = engine->initialise (savedDeviceState.get());
        const auto savedOutputName = savedDeviceState != nullptr ? savedDeviceState->getStringAttribute ("audioOutputDeviceName") : juce::String();
        const auto openedOutputName = engine->getDeviceManager().getAudioDeviceSetup().outputDeviceName;
        engine->getDeviceManager().addChangeListener (this);

        auto& host = engine->getPluginHost();
        host.loadKnownPluginsFromXml (settings->getPluginList().get());
        host.onKnownPluginsChanged = [this]
        {
            if (engine != nullptr && settings != nullptr)
                settings->setPluginList (engine->getPluginHost().createKnownPluginsXml().get());
        };

        commandManager.registerAllCommandsForTarget (this);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine, *settings, commandManager);
        announceVersionChange();

        if (deviceError.isNotEmpty())
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle (ko ("오디오 장치를 열지 못했습니다"))
                                              .withMessage (deviceError + "\n\n" + ko ("메뉴 [오디오 > 오디오 출력 설정]에서 장치를 선택하세요."))
                                              .withButton (ko ("확인")),
                                          [] (int) {});
        }

        const bool deviceFallback = savedOutputName.isNotEmpty() && savedOutputName != openedOutputName;
        mainWindow->getMainComponent().setAutoStartOnOpenAllowed (! safeMode && ! deviceFallback);   // a quiet launch: nothing starts by itself
        installEscapeHook();

        mainWindow->getMainComponent().openProjectFromCommandLine (commandLine);

        if (const auto reopen = settings->getReopenProjectAfterUpdate(); reopen != juce::File())
        {
            settings->setReopenProjectAfterUpdate ({});   // once

            if (! mainWindow->getMainComponent().getProjectFile().existsAsFile() && reopen.existsAsFile())
                mainWindow->getMainComponent().openProjectFile (reopen, false);   // the show that was open when the update closed the app, without its auto-start
        }

        if (safeMode)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, ko ("안전 모드"),
                                                    ko ("Shift를 누른 채 실행해 안전 모드로 시작했습니다. 저장된 오디오 장치와 플러그인을 불러오지 않았습니다.\n메뉴 [오디오 > 오디오 출력 설정]에서 장치를 고르세요."), ko ("확인"));
        else if (deviceFallback)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("저장된 오디오 장치를 열지 못했습니다"),
                                                    ko ("저장된 출력 장치: ") + savedOutputName + "\n" + ko ("지금 열린 장치: ") + (openedOutputName.isNotEmpty() ? openedOutputName : ko ("없음"))
                                                        + "\n\n" + ko ("공연 전에 [오디오 > 오디오 출력 설정]에서 장치를 확인하세요."), ko ("확인"));

        Updater::Callbacks updaterCallbacks;
        updaterCallbacks.canShutdown = [this]
        {
            // the installer must never close a running show or unsaved work
            return mainWindow == nullptr
                || (! mainWindow->getMainComponent().hasUnsavedChanges() && mainWindow->getMainComponent().isIdleForInterruptions());
        };
        updaterCallbacks.requestShutdown = [this]
        {
            juce::MessageManager::callAsync ([this]
            {
                if (mainWindow != nullptr && settings != nullptr)
                    settings->setReopenProjectAfterUpdate (mainWindow->getMainComponent().getProjectFile());   // back after the update

                if (auto* app = juce::JUCEApplication::getInstance())
                    app->systemRequestedQuit();
            });
        };
        Updater::initialise ("Gomtwigim", getApplicationName(), getApplicationVersion(), std::move (updaterCallbacks));
        launchedAt = juce::Time::getCurrentTime();
        startTimer (30 * 1000);   // update checks at idle moments only (see timerCallback)
    }

    /** First run of a new version: say so (the silent auto-update shows nothing else). A fresh install stays quiet. */
    void announceVersionChange()
    {
        const auto previous = settings->getLastRunVersion();
        const auto current = getApplicationVersion();

        if (previous == current)
            return;

        settings->setLastRunVersion (current);

        if (previous.isEmpty())
            return;

        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                ko ("업데이트 완료"),
                                                ko ("앤큐가 ") + previous + " → " + current + ko ("(으)로 업데이트되었습니다.\n바뀐 점은 도움말 > 사용 설명서와 GitHub 릴리스 노트에 있습니다."),
                                                ko ("확인"));
    }

    void shutdown() override
    {
        removeEscapeHook();
        stopTimer();
        Updater::shutdown();

        if (engine != nullptr)
            engine->getDeviceManager().removeChangeListener (this);

        saveDeviceState();

        if (mainWindow != nullptr && settings != nullptr)
            settings->setWindowState (mainWindow->getWindowStateAsString());

        mainWindow = nullptr;

        if (engine != nullptr)
            engine->shutdown();

        engine = nullptr;

        if (settings != nullptr)
            settings->flush();

        settings = nullptr;

        juce::PopupMenu::dismissAllActiveMenus();             // a menu open at shutdown still references the look and feel
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);   // after every window is gone
        lookAndFeel = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
        {
            auto& main = mainWindow->getMainComponent();
            main.confirmReplaceProjectThen ([this, &main] { main.fireCloseCueThen ([this] { quit(); }); }, ko ("종료할까요?"));   // a running show is never closed by one click
        }
        else
        {
            quit();
        }
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow == nullptr)
            return;

        // the running window takes the file (with the usual unsaved-changes question) and comes to the front
        mainWindow->setMinimised (false);
        mainWindow->toFront (true);
        mainWindow->getMainComponent().openProjectFromCommandLine (commandLine);
    }

private:
    /** An update check runs 20 s after launch and then once a day, but only while nothing plays and show mode is
        off: the update dialog must not appear in the middle of a show. */
    void timerCallback() override
    {
        if (mainWindow == nullptr || ! Updater::isAvailable() || ! mainWindow->getMainComponent().isIdleForInterruptions())
            return;

        const auto now = juce::Time::getCurrentTime();

        if ((now - launchedAt).inSeconds() < 20.0)
            return;

        if (lastQuietCheck != juce::Time() && (now - lastQuietCheck).inHours() < 24.0)
            return;

        lastQuietCheck = now;
        Updater::checkQuietly();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (engine != nullptr)
        {
            const auto error = engine->enforceOutputLimit();   // a non-ASIO type never keeps more than outputs 1-2

            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        ko ("오디오 출력"),
                                                        ko ("장치를 출력 1-2로 다시 열지 못했습니다.\n") + error
                                                            + "\n\n" + ko ("메뉴 [오디오 > 오디오 출력 설정]에서 장치를 다시 선택하세요."),
                                                        ko ("확인"));
        }

        saveDeviceState();
    }

    void saveDeviceState()
    {
        if (engine == nullptr || settings == nullptr)
            return;

        if (const auto xml = engine->getDeviceManager().createStateXml())
            settings->setAudioDeviceState (xml.get());
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, AudioEngine& engine, AppSettings& settings,
                    juce::ApplicationCommandManager& commands)
            : DocumentWindow (name, Palette::background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);

            auto* content = new MainComponent (engine, settings, commands);
            content->onWindowTitleChanged = [this] (const juce::String& title)
            {
                // "Enqueue 0.9.0 - show": the version is always in sight, so an update is never in doubt
                setName (title.startsWith ("Enqueue") ? "Enqueue " + JUCEApplication::getInstance()->getApplicationVersion() + title.substring (7) : title);
            };
            setContentOwned (content, true);
            mainComponent = content;

            setResizable (true, false);
            setResizeLimits (860, 640, 10000, 10000);   // room for the transport, a few rows and the inspector's minimum

            const auto state = settings.getWindowState();

            if (state.isEmpty() || ! restoreWindowStateFromString (state))
                centreWithSize (1100, 720);

            setName ("Enqueue " + JUCEApplication::getInstance()->getApplicationVersion() + " - " + ko ("제목 없음"));
            setVisible (true);
        }

        MainComponent& getMainComponent() { return *mainComponent; }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        MainComponent* mainComponent = nullptr;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    juce::ApplicationCommandManager commandManager;
    std::unique_ptr<GoCueLookAndFeel> lookAndFeel;
    juce::Time launchedAt, lastQuietCheck;
    bool safeMode = false;

    // Esc from anywhere: a plugin editor's own window takes the keyboard and JUCE never sees the key, so a
    // thread-local keyboard hook watches every window of this thread. The press is not consumed (a dialog still
    // cancels), the panic itself runs from the message queue.
    void installEscapeHook()
    {
       #if JUCE_WINDOWS
        escapeHandler = [this]
        {
            if (mainWindow != nullptr)
                mainWindow->getMainComponent().panicFromAnywhere();
        };

        escapeHook = SetWindowsHookExW (WH_KEYBOARD, escapeHookProc, nullptr, GetCurrentThreadId());
       #endif
    }

    void removeEscapeHook()
    {
       #if JUCE_WINDOWS
        if (escapeHook != nullptr)
        {
            UnhookWindowsHookEx (escapeHook);
            escapeHook = nullptr;
        }

        escapeHandler = nullptr;
       #endif
    }
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace gocue

START_JUCE_APPLICATION (gocue::GoCueApplication)
