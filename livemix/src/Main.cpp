#include "LiveMixSettings.h"
#include "MixDocument.h"
#include "MixEngine.h"
#include "app/Updater.h"
#include "ui/LiveMixLookAndFeel.h"
#include "ui/MainComponent.h"

#include <juce_gui_extra/juce_gui_extra.h>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <shellapi.h>
#endif

namespace gocue::livemix
{

/** The tray icon: the red ON tile. Left click / double click brings the window back, right click opens the menu. */
class TrayIcon : public juce::SystemTrayIconComponent,
                 private juce::Timer
{
public:
    explicit TrayIcon (std::function<void (int)> onMenu) : menuHandler (std::move (onMenu))
    {
        // drawn at the size the notification area shows (16 px at 100 %, 24 at 150 %, ...): a larger icon is
        // scaled down by the shell and comes out as a black tile (its transparency is lost on the way)
        double scale = 1.0;

        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            scale = display->scale;

        const int size = juce::jlimit (16, 64, juce::roundToInt (16.0 * scale));
        juce::Image image (juce::Image::ARGB, size, size, true);
        juce::Graphics g (image);
        g.setColour (Palette::brand);
        g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) size, (float) size), (float) size * 0.19f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions ((float) size * 0.5f, juce::Font::bold)));
        g.drawText ("ON", image.getBounds(), juce::Justification::centred, false);
        setIconImage (image, image);
        setIconTooltip ("LiveMix");
        useExecutableIcon();
        startTimer (15000);   // an Explorer restart makes JUCE re-add its own (black) picture: ours goes back within a few seconds
    }

    ~TrayIcon() override
    {
       #if JUCE_WINDOWS
        if (shellIcon != nullptr)
            DestroyIcon (shellIcon);
       #endif
    }

    /** Windows 11's notification area draws an icon made from a bitmap in memory (JUCE's HICON) as a black square;
        the app's own icon resource, extracted the way the shell does it, is drawn right. JUCE's tray entry is kept -
        this only swaps its picture for the executable's small icon. */
    void useExecutableIcon()
    {
       #if JUCE_WINDOWS
        HICON smallIcon = nullptr;
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
        ExtractIconExW (exe.toWideCharPointer(), 0, nullptr, &smallIcon, 1);

        if (smallIcon == nullptr)
            return;

        NOTIFYICONDATA data = {};
        data.cbSize = sizeof (data);
        data.hWnd = (HWND) getWindowHandle();
        data.uID = (UINT) (juce::pointer_sized_int) data.hWnd;   // the id JUCE registered the entry under
        data.uFlags = NIF_ICON;
        data.hIcon = smallIcon;

        if (! Shell_NotifyIcon (NIM_MODIFY, &data))
        {
            DestroyIcon (smallIcon);
            return;
        }

        shellIcon = smallIcon;
       #endif
    }

    void timerCallback() override
    {
       #if JUCE_WINDOWS
        if (shellIcon == nullptr)
            return;

        NOTIFYICONDATA data = {};
        data.cbSize = sizeof (data);
        data.hWnd = (HWND) getWindowHandle();
        data.uID = (UINT) (juce::pointer_sized_int) data.hWnd;
        data.uFlags = NIF_ICON;
        data.hIcon = shellIcon;
        Shell_NotifyIcon (NIM_MODIFY, &data);   // idempotent
       #endif
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
            menu.addItem (5, ko ("마이크 뮤트그룹 뮤트/해제"));
            menu.addItem (6, ko ("FX 뮤트그룹 뮤트/해제"));
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
   #if JUCE_WINDOWS
    HICON shellIcon = nullptr;
   #endif
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
        engine->setSkipChainWhenOff (settings->getSkipPluginsWhenOff());
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

        // a session named on the command line that could not be opened keeps its notice: loading the last session
        // instead would look as if the double-clicked file had opened
        const bool sessionNamed = main.openFromCommandLine (commandLine);

        if (! sessionNamed && ! document->hasFile())
        {
            const auto last = settings->getLastSessionFile();

            if (last.existsAsFile())
                main.openSession (last);
            else
                createDefaultSession (main);
        }

        if (! document->hasAppliedGraph())
            document->applyToEngine();   // every path above failed to bring a session in: the in-memory default runs (its notice stays)

        // in the window, not a modal alert: a modal would freeze the frame (no resizing) and the mic buttons until
        // dismissed. Its own line under a session warning, never replacing it.
        if (safeMode)
            main.setStartupNote (ko ("안전 모드로 시작했습니다 (Shift): 저장된 ASIO 장치와 플러그인을 불러오지 않았습니다. 플러그인 설정은 세션에 그대로 남습니다. 설정에서 장치를 고르세요."), false, true);
        else if (deviceError.isNotEmpty())
            main.setStartupNote (ko ("ASIO 장치를 열지 못했습니다: ") + deviceError + " " + ko ("설정에서 장치를 고르거나 오디오 인터페이스 연결을 확인하세요."), true, false);

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
        document->applyToEngine();   // the constructor left the graph empty on purpose

        if (document->save (file).wasOk())
        {
            settings->setLastSessionFile (file);
            settings->addRecentSession (file);
        }
    }

    void shutdown() override
    {
        stopTimer();

        // a second instance: initialise() never ran (JUCE handed its command line to the running instance and quits
        // through here) - there is nothing to close, and settings is null
        if (settings == nullptr)
            return;

        Updater::shutdown();
        tray = nullptr;

        if (mainWindow != nullptr)
        {
            settings->setWindowState (mainWindow->getWindowStateAsString());
            mainWindow->getMainComponent().saveIfDirty();
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

            if (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->toFront (true);   // a question that was open stays in front of the window that just came forward
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
            setResizable (true, false);
            setResizeLimits (420, 720, 10000, 10000);   // before the content: its corner grip takes this constrainer. 420 wide: a tall (portrait) window; 720 high: the stacked master leaves room for the mics
            auto* content = new MainComponent (document, settings);
            mainComponent = content;
            setContentOwned (content, true);
            addKeyListener (content);   // Ctrl+S / Ctrl+Shift+S / Ctrl+N from anywhere in the window

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

        ~MainWindow() override
        {
            if (mainComponent != nullptr)
                removeKeyListener (mainComponent);   // before the content goes
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

        void minimisationStateChanged (bool isNowMinimised) override
        {
            // the native title bar's minimise button never reaches minimiseButtonPressed(): the window is already
            // minimised when this arrives, so it goes to the tray from here (not inside the peer's callback)
            if (isNowMinimised && prefs.getMinimiseToTray())
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainWindow> (this)]
                {
                    if (safe != nullptr && safe->isMinimised())
                        safe->app.hideToTray();
                });
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
            case 5: if (mainWindow != nullptr) mainWindow->getMainComponent().getMuteGroups().toggle (MuteGroups::Group::mic); break;
            case 6: if (mainWindow != nullptr) mainWindow->getMainComponent().getMuteGroups().toggle (MuteGroups::Group::fx); break;
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
