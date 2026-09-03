#include "app/AppSettings.h"
#include "app/Updater.h"
#include "audio/AudioEngine.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/MainComponent.h"
#include "ui/UiUtils.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace gocue
{

class GoCueApplication : public juce::JUCEApplication,
                         private juce::ChangeListener
{
public:
    GoCueApplication() = default;

    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return false; }   // a .gocue opened from Explorer lands in the running window

    void initialise (const juce::String& commandLine) override
    {
        lookAndFeel = std::make_unique<GoCueLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        settings = std::make_unique<AppSettings>();
        engine = std::make_unique<AudioEngine>();

        const auto savedDeviceState = settings->getAudioDeviceState();
        const auto deviceError = engine->initialise (savedDeviceState.get());
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

        mainWindow->getMainComponent().openProjectFromCommandLine (commandLine);

        Updater::Callbacks updaterCallbacks;
        updaterCallbacks.canShutdown = [this]
        {
            return mainWindow == nullptr || ! mainWindow->getMainComponent().hasUnsavedChanges();
        };
        updaterCallbacks.requestShutdown = []
        {
            juce::MessageManager::callAsync ([]
            {
                if (auto* app = juce::JUCEApplication::getInstance())
                    app->systemRequestedQuit();
            });
        };
        Updater::initialise ("GoCue", getApplicationName(), getApplicationVersion(), std::move (updaterCallbacks));
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
                                                ko ("GoCue가 ") + previous + " → " + current + ko ("(으)로 업데이트되었습니다.\n바뀐 점은 도움말 > 사용 설명서와 GitHub 릴리스 노트에 있습니다."),
                                                ko ("확인"));
    }

    void shutdown() override
    {
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
            main.confirmDiscardChangesThen ([this, &main] { main.fireCloseCueThen ([this] { quit(); }); });
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
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
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
                // "GoCue 0.8.5 - show": the version is always in sight, so an update is never in doubt
                setName (title.startsWith ("GoCue") ? "GoCue " + JUCEApplication::getInstance()->getApplicationVersion() + title.substring (5) : title);
            };
            setContentOwned (content, true);
            mainComponent = content;

            setResizable (true, false);
            setResizeLimits (860, 640, 10000, 10000);   // room for the transport, a few rows and the inspector's minimum

            const auto state = settings.getWindowState();

            if (state.isEmpty() || ! restoreWindowStateFromString (state))
                centreWithSize (1100, 720);

            setName ("GoCue " + JUCEApplication::getInstance()->getApplicationVersion() + " - " + ko ("제목 없음"));
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
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace gocue

START_JUCE_APPLICATION (gocue::GoCueApplication)
