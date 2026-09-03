#include "LiveMixSettings.h"
#include "MixDocument.h"
#include "MixEngine.h"
#include "app/Updater.h"
#include "ui/LiveMixLookAndFeel.h"
#include "ui/MainComponent.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace gocue::livemix
{

/** The tray icon: the red ON tile. Left click / double click brings the window back, right click opens the menu. */
class TrayIcon : public juce::SystemTrayIconComponent
{
public:
    explicit TrayIcon (std::function<void (int)> onMenu) : menuHandler (std::move (onMenu))
    {
        juce::Image image (juce::Image::ARGB, 64, 64, true);
        juce::Graphics g (image);
        g.setColour (Palette::brand);
        g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, 64.0f, 64.0f), 12.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (30.0f, juce::Font::bold)));
        g.drawText ("ON", image.getBounds(), juce::Justification::centred, false);
        setIconImage (image, image);
        setIconTooltip ("LiveMix");
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem (1, ko ("LiveMix 열기"));
            menu.addSeparator();
            menu.addItem (2, ko ("마이크 전부 ON"));
            menu.addItem (3, ko ("마이크 전부 OFF"));
            menu.addSeparator();
            menu.addItem (4, ko ("종료"));
            menu.showMenuAsync (juce::PopupMenu::Options(), [this] (int result) { if (result != 0 && menuHandler) menuHandler (result); });
        }
        else if (menuHandler)
        {
            menuHandler (1);
        }
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (menuHandler)
            menuHandler (1);
    }

private:
    std::function<void (int)> menuHandler;
};

class LiveMixApplication : public juce::JUCEApplication,
                           private juce::ChangeListener,
                           private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "LiveMix"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        lookAndFeel = std::make_unique<LiveMixLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        settings = std::make_unique<LiveMixSettings>();
        engine = std::make_unique<MixEngine>();
        auto& host = engine->getPluginHost();
        host.loadKnownPluginsFromXml (settings->getPluginList().get());
        host.onKnownPluginsChanged = [this]
        {
            if (engine != nullptr && settings != nullptr)
                settings->setPluginList (engine->getPluginHost().createKnownPluginsXml().get());
        };

        // safe mode (Shift held at launch, or --safe-mode): the saved device is not opened (a hung driver would keep the
        // window from ever appearing) and plugins are not instantiated (a plugin that crashes on load would otherwise
        // take every launch down with it); the session's plugin settings survive untouched
        safeMode = juce::ArgumentList ("LiveMix", commandLine).containsOption ("--safe-mode")
                   || juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
        PluginHost::setSafeMode (safeMode);

        const std::unique_ptr<juce::XmlElement> savedDevice = safeMode ? nullptr : settings->getAudioDeviceState();
        const auto deviceError = safeMode ? juce::String() : engine->initialise (savedDevice.get());
        engine->getDeviceManager().addChangeListener (this);

        document = std::make_unique<MixDocument> (*engine);
        mainWindow = std::make_unique<MainWindow> (*this, *document, *settings);
        tray = std::make_unique<TrayIcon> ([this] (int item) { trayMenu (item); });

        auto& main = mainWindow->getMainComponent();
        main.setSafeMode (safeMode);
        main.openFromCommandLine (commandLine);

        if (! document->hasFile())
        {
            const auto last = settings->getLastSessionFile();

            if (last.existsAsFile())
                main.openSession (last);
            else
                createDefaultSession (main);
        }

        if (safeMode)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, ko ("안전 모드"),
                                                    ko ("Shift를 누른 채 실행해 안전 모드로 시작했습니다. 저장된 ASIO 장치와 플러그인을 불러오지 않았습니다 (플러그인 설정은 세션에 그대로 남습니다).\n설정에서 장치를 고르세요."), ko ("확인"));
        else if (deviceError.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("ASIO 장치를 열지 못했습니다"),
                                                    deviceError + "\n\n" + ko ("설정에서 장치를 고르거나 오디오 인터페이스 연결을 확인하세요."), ko ("확인"));

        Updater::Callbacks callbacks;
        callbacks.canShutdown = [this]
        {
            // the installer must not close unsaved work or cut a backup upload
            return document == nullptr
                || (! document->isDirty() && (mainWindow == nullptr || ! mainWindow->getMainComponent().isUploadingBackup()));
        };
        callbacks.requestShutdown = [this]
        {
            juce::MessageManager::callAsync ([this]
            {
                if (auto* app = juce::JUCEApplication::getInstance())
                    app->systemRequestedQuit();
            });
        };
        Updater::initialise ("Gomtwigim", getApplicationName(), getApplicationVersion(), std::move (callbacks), "Software\\Gomtwigim\\LiveMix\\WinSparkle");
        launchedAt = juce::Time::getCurrentTime();
        startTimer (30 * 1000);

        if (settings->getLastRunVersion() != getApplicationVersion())
            settings->setLastRunVersion (getApplicationVersion());
    }

    void createDefaultSession (MainComponent& main)
    {
        auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LiveMix");
        folder.createDirectory();
        const auto file = folder.getChildFile (ko ("기본 세션") + juce::String (MixSession::fileExtension));

        if (file.existsAsFile())
        {
            main.openSession (file);
            return;
        }

        document->setSessionName (ko ("기본 세션"));

        if (document->save (file).wasOk())
        {
            settings->setLastSessionFile (file);
            settings->addRecentSession (file);
        }
    }

    void shutdown() override
    {
        stopTimer();
        Updater::shutdown();
        tray = nullptr;

        if (mainWindow != nullptr)
        {
            settings->setWindowState (mainWindow->getWindowStateAsString());
            mainWindow->getMainComponent().autosaveNow();
        }

        mainWindow = nullptr;

        if (engine != nullptr)
        {
            engine->getDeviceManager().removeChangeListener (this);
            settings->setAudioDeviceState (engine->getDeviceManager().createStateXml().get());
            engine->shutdown();
        }

        document = nullptr;
        engine = nullptr;
        settings->saveIfNeeded();
        settings = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (mainWindow == nullptr || document == nullptr)
        {
            quit();
            return;
        }

        if (document->isDirty())
            showWindow();   // the question below must be seen, also from the tray

        mainWindow->getMainComponent().withSessionSecured ([this] { quit(); });
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow != nullptr)
        {
            showWindow();
            mainWindow->getMainComponent().openFromCommandLine (commandLine);
        }
    }

    void showWindow()
    {
        if (mainWindow == nullptr)
            return;

        mainWindow->setVisible (true);
        mainWindow->setMinimised (false);
        mainWindow->toFront (true);
    }

    void hideToTray()
    {
        if (mainWindow != nullptr)
            mainWindow->setVisible (false);
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (LiveMixApplication& a, MixDocument& document, LiveMixSettings& settings)
            : DocumentWindow ("LiveMix", Palette::background, DocumentWindow::allButtons), app (a), prefs (settings)
        {
            setUsingNativeTitleBar (true);
            auto* content = new MainComponent (document, settings);
            mainComponent = content;
            setContentOwned (content, true);
            setResizable (true, false);
            setResizeLimits (720, 560, 10000, 10000);

            if (! restoreWindowStateFromString (settings.getWindowState()))
                centreWithSize (1440, 900);

            setVisible (true);
            document.onValueChanged = [this, original = document.onValueChanged, &document]
            {
                if (original)
                    original();

                setName ("LiveMix - " + document.getDisplayName() + (document.isDirty() ? " *" : ""));
            };
            setName ("LiveMix - " + document.getDisplayName());
        }

        MainComponent& getMainComponent() { return *mainComponent; }

        void closeButtonPressed() override
        {
            if (prefs.getCloseToTray())
                app.hideToTray();
            else
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        void minimiseButtonPressed() override
        {
            if (prefs.getMinimiseToTray())
                app.hideToTray();
            else
                setMinimised (true);
        }

    private:
        LiveMixApplication& app;
        LiveMixSettings& prefs;
        MainComponent* mainComponent = nullptr;
    };

private:
    void trayMenu (int item)
    {
        switch (item)
        {
            case 1: showWindow(); break;
            case 2: if (document != nullptr) document->setAllChannelsOn (true); break;
            case 3: if (document != nullptr) document->setAllChannelsOn (false); break;
            case 4: systemRequestedQuit(); break;
            default: break;
        }
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (mainWindow != nullptr)
            mainWindow->getMainComponent().deviceChanged();
    }

    void timerCallback() override
    {
        // the update check: once 20 s after the launch, then daily
        const auto now = juce::Time::getCurrentTime();

        if (lastQuietCheck == juce::Time() ? (now - launchedAt).inSeconds() >= 20.0 : (now - lastQuietCheck).inHours() >= 24.0)
        {
            lastQuietCheck = now;
            Updater::checkQuietly();
        }
    }

    std::unique_ptr<LiveMixLookAndFeel> lookAndFeel;
    std::unique_ptr<LiveMixSettings> settings;
    std::unique_ptr<MixEngine> engine;
    std::unique_ptr<MixDocument> document;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<TrayIcon> tray;
    juce::Time launchedAt, lastQuietCheck;
    bool safeMode = false;
};

} // namespace gocue::livemix

START_JUCE_APPLICATION (gocue::livemix::LiveMixApplication)
