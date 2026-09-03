#include "MainComponent.h"

#include "BackupDialog.h"
#include "SettingsDialog.h"
#include "app/Links.h"
#include "app/Updater.h"

namespace gocue::livemix
{

MainComponent::MainComponent (MixDocument& doc, LiveMixSettings& s)
    : document (doc), settings (s), engine (doc.getEngine()), topBar (doc), masterCard (doc), chainDrawer (doc, windows), fxDrawer (doc)
{
    setOpaque (true);
    addAndMakeVisible (topBar);

    viewport.setViewedComponent (&cardsHolder, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    addChannelButton.setButtonText (ko ("+ 마이크 채널 추가"));
    addChannelButton.setWantsKeyboardFocus (false);
    addChannelButton.setColour (juce::TextButton::buttonColourId, Palette::background);
    addChannelButton.onClick = [this]
    {
        if (document.addChannel().isNull())
            showStatus (ko ("마이크 채널은 최대 ") + juce::String (MixSession::maxChannels) + ko ("개입니다"), true);
    };
    cardsHolder.addAndMakeVisible (addChannelButton);

    addAndMakeVisible (masterCard);
    masterCard.onOpenChain = [this] { openChainFor (&engine.getMasterChain(), ko ("마스터")); };
    masterCard.onAddPlugin = [this] { addPluginTo (&engine.getMasterChain(), ko ("마스터"), &masterCard); };
    masterCard.onOpenPluginEditor = [this] (int slot)
    {
        auto& chain = engine.getMasterChain();

        if (slot < chain.getNumSlots() && chain.getSlot (slot).plugin != nullptr)
            windows.open (*chain.getSlot (slot).plugin, ko ("마스터") + " - " + chain.getSlot (slot).plugin->getName());
    };

    addChildComponent (chainDrawer);
    chainDrawer.onClose = [this] { showDrawer (Drawer::none); };
    chainDrawer.onOpenPluginManager = [this] { showPluginManager(); };
    chainDrawer.onChainEdited = [this] { refreshValues(); };
    addChildComponent (fxDrawer);
    fxDrawer.onClose = [this] { showDrawer (Drawer::none); };
    fxDrawer.onOpenChain = [this] (const juce::Uuid& id)
    {
        if (const auto* f = document.getSession().findFx (id))
            openChainFor (engine.getFxChain (id), "FX " + f->name);
    };
    fxDrawer.onAddPlugin = [this] (const juce::Uuid& id)
    {
        if (const auto* f = document.getSession().findFx (id))
            addPluginTo (engine.getFxChain (id), "FX " + f->name, &fxDrawer);
    };
    fxDrawer.onOpenPluginEditor = [this] (const juce::Uuid& id, int slot)
    {
        if (auto* chain = engine.getFxChain (id))
            if (slot < chain->getNumSlots() && chain->getSlot (slot).plugin != nullptr)
                windows.open (*chain->getSlot (slot).plugin, "FX - " + chain->getSlot (slot).plugin->getName());
    };

    topBar.onDeviceChosen = [this] (const juce::String& name) { chooseDevice (name); };
    topBar.onSessionMenu = [this] (juce::Component* anchor) { showSessionMenu (anchor); };
    topBar.onFxPanel = [this] { showDrawer (drawer == Drawer::fx ? Drawer::none : Drawer::fx); };
    topBar.onBackup = [this] { showBackupDialog(); };
    topBar.onSettings = [this] { showSettingsDialog(); };
    topBar.onHelpMenu = [this] (juce::Component* anchor) { showHelpMenu (anchor); };

    statusLeft.setFont (bodyFont (12.5f));
    statusLeft.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (statusLeft);
    statusRight.setFont (bodyFont (12.5f));
    statusRight.setColour (juce::Label::textColourId, Palette::dimText);
    statusRight.setJustificationType (juce::Justification::centredRight);
    statusRight.setText (ko ("최소화하면 트레이에서 계속 동작합니다"), juce::dontSendNotification);
    addAndMakeVisible (statusRight);

    windows.onChainChanged = [this] (PluginChain&) { document.markDirty(); };
    engine.forEachChain ([this] (PluginChain& chain) { chain.setListener (&windows); });

    document.onStructureChanged = [this]
    {
        engine.forEachChain ([this] (PluginChain& chain) { chain.setListener (&windows); });
        rebuildCards();
        lastChangeMs = juce::Time::getMillisecondCounterHiRes();
    };
    document.onValueChanged = [this]
    {
        refreshValues();
        lastChangeMs = juce::Time::getMillisecondCounterHiRes();
    };

    updateDeviceNames();
    rebuildCards();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    windows.closeAll();
    document.onStructureChanged = nullptr;
    document.onValueChanged = nullptr;
}

//==============================================================================
void MainComponent::rebuildCards()
{
    const auto& session = document.getSession();
    std::vector<std::unique_ptr<ChannelCard>> next;

    for (const auto& c : session.channels)
    {
        std::unique_ptr<ChannelCard> card;

        for (auto& existing : cards)
            if (existing != nullptr && existing->getChannelId() == c.id)
                card = std::move (existing);

        if (card == nullptr)
        {
            card = std::make_unique<ChannelCard> (document, c.id);
            card->onOpenChain = [this] (const juce::Uuid& id)
            {
                if (const auto* ch = document.getSession().findChannel (id))
                    openChainFor (engine.getChannelChain (id), ch->name);
            };
            card->onAddPlugin = [this] (const juce::Uuid& id)
            {
                if (const auto* ch = document.getSession().findChannel (id))
                    addPluginTo (engine.getChannelChain (id), ch->name, nullptr);
            };
            card->onRemove = [this] (const juce::Uuid& id)
            {
                juce::Component::SafePointer<MainComponent> safeThis (this);
                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                  .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                  .withTitle (ko ("채널 삭제"))
                                                  .withMessage (ko ("이 마이크 채널과 그 플러그인 설정이 지워집니다. 삭제할까요?"))
                                                  .withButton (ko ("삭제"))
                                                  .withButton (ko ("취소")),
                                              [safeThis, id] (int result)
                {
                    if (safeThis != nullptr && result == 1)
                        safeThis->document.removeChannel (id);
                });
            };
            card->onOpenPluginEditor = [this] (const juce::Uuid& id, int slot)
            {
                if (auto* chain = engine.getChannelChain (id))
                    if (slot < chain->getNumSlots() && chain->getSlot (slot).plugin != nullptr)
                        if (const auto* ch = document.getSession().findChannel (id))
                            windows.open (*chain->getSlot (slot).plugin, ch->name + " - " + chain->getSlot (slot).plugin->getName());
            };
            cardsHolder.addAndMakeVisible (*card);
        }

        card->setDeviceChannels (inputNames, outputNames);
        card->refresh();
        next.push_back (std::move (card));
    }

    cards = std::move (next);
    masterCard.setDeviceChannels (outputNames);
    fxDrawer.setDeviceChannels (outputNames);
    topBar.setFxCount ((int) session.fx.size());
    topBar.refresh();
    fxDrawer.refresh();

    // the chain drawer follows its owner; an owner that is gone closes it
    if (drawer == Drawer::chain)
    {
        PluginChain* chain = chainOwnerId.isNull() ? &engine.getMasterChain() : chainIsFx ? engine.getFxChain (chainOwnerId) : engine.getChannelChain (chainOwnerId);

        if (chain == nullptr)
            showDrawer (Drawer::none);
        else if (chain != chainDrawer.getChain())
            chainDrawer.setChain (chain, chainDrawer.getChain() != nullptr ? juce::String() : juce::String());
        else
            chainDrawer.refresh();
    }

    layoutCards();
}

void MainComponent::refreshValues()
{
    for (auto& card : cards)
        card->refresh();

    masterCard.refresh();
    fxDrawer.refresh();
    topBar.refresh();
    topBar.setFxCount ((int) document.getSession().fx.size());

    if (drawer == Drawer::chain)
        chainDrawer.refresh();

    layoutCards();
}

void MainComponent::refreshAll()
{
    updateDeviceNames();
    rebuildCards();
}

CardLayout MainComponent::layoutForWidth (int width) const
{
    if (width >= 1180)
        return CardLayout::wide;

    if (width >= 800)
        return CardLayout::medium;

    return CardLayout::narrow;
}

void MainComponent::layoutCards()
{
    const int width = juce::jmax (100, viewport.getMaximumVisibleWidth());
    const auto mode = layoutForWidth (width + 40);
    int y = 0;
    const int gap = 12;

    for (auto& card : cards)
    {
        card->setLayout (mode);
        const int h = card->getPreferredHeight();
        card->setBounds (0, y, width, h);
        y += h + gap;
    }

    addChannelButton.setBounds (0, y, width, 56);
    addChannelButton.setEnabled ((int) cards.size() < MixSession::maxChannels);
    y += 56 + gap;
    cardsHolder.setSize (width, juce::jmax (1, y));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    topBar.setBounds (area.removeFromTop (64));
    auto status = area.removeFromBottom (30);
    statusLeft.setBounds (status.reduced (16, 0).removeFromLeft (status.getWidth() / 2));
    statusRight.setBounds (status.reduced (16, 0).removeFromRight (status.getWidth() / 2));

    const int drawerW = juce::jmin (440, juce::jmax (320, getWidth() / 3));
    chainDrawer.setBounds (area.removeFromRight (drawer == Drawer::chain ? drawerW : 0).withRight (getWidth()).withWidth (drawerW));
    fxDrawer.setBounds (chainDrawer.getBounds());
    chainDrawer.setVisible (drawer == Drawer::chain);
    fxDrawer.setVisible (drawer == Drawer::fx);

    if (drawer != Drawer::none)
        area.setRight (getWidth() - drawerW);

    const int masterH = 176;
    masterCard.setBounds (area.removeFromBottom (masterH).reduced (16, 8));
    viewport.setBounds (area.reduced (16, 12));
    layoutCards();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    auto status = getLocalBounds().removeFromBottom (30);
    g.setColour (Palette::bar);
    g.fillRect (status);
    g.setColour (Palette::line);
    g.fillRect (status.removeFromTop (1));
    g.fillRect (juce::Rectangle<int> (0, getHeight() - 30 - 176 - 8, getWidth() - (drawer == Drawer::none ? 0 : juce::jmin (440, juce::jmax (320, getWidth() / 3))), 1));
}

//==============================================================================
void MainComponent::showDrawer (Drawer which)
{
    drawer = which;
    resized();
}

void MainComponent::openChainFor (PluginChain* chain, const juce::String& title)
{
    if (chain == nullptr)
        return;

    chainOwnerId = juce::Uuid::null();
    chainIsFx = false;

    for (const auto& c : document.getSession().channels)
        if (engine.getChannelChain (c.id) == chain)
            chainOwnerId = c.id;

    for (const auto& f : document.getSession().fx)
        if (engine.getFxChain (f.id) == chain)
        {
            chainOwnerId = f.id;
            chainIsFx = true;
        }

    chainDrawer.setChain (chain, title);
    showDrawer (Drawer::chain);
}

void MainComponent::addPluginTo (PluginChain* chain, const juce::String& title, juce::Component* anchor)
{
    if (chain == nullptr)
        return;

    openChainFor (chain, title);
    chainDrawer.showAddMenu (anchor != nullptr ? anchor : &chainDrawer);
}

void MainComponent::showPluginManager()
{
    auto& host = engine.getPluginHost();
    auto* list = new juce::PluginListComponent (host.getFormatManager(), host.getKnownPlugins(),
                                                juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("LiveMix").getChildFile ("scan.crashed"),
                                                nullptr, false);
    list->setSize (720, 480);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (list);
    options.dialogTitle = ko ("VST3 플러그인 관리");
    options.dialogBackgroundColour = Palette::card;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.componentToCentreAround = this;
    options.launchAsync();
}

//==============================================================================
void MainComponent::updateDeviceNames()
{
    inputNames.clear();
    outputNames.clear();
    juce::StringArray asio;
    juce::String current;

    for (auto* type : engine.getDeviceManager().getAvailableDeviceTypes())
    {
        if (! type->getTypeName().containsIgnoreCase ("ASIO"))
            continue;

        type->scanForDevices();
        asio = type->getDeviceNames (false);
    }

    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
    {
        current = device->getName();
        inputNames = device->getInputChannelNames();
        outputNames = device->getOutputChannelNames();
    }

    topBar.setDevices (asio, current);
}

void MainComponent::chooseDevice (const juce::String& name)
{
    auto& dm = engine.getDeviceManager();
    auto setup = dm.getAudioDeviceSetup();

    if (setup.outputDeviceName == name && setup.inputDeviceName == name)
        return;

    setup.outputDeviceName = name;
    setup.inputDeviceName = name;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;
    const auto error = dm.setAudioDeviceSetup (setup, true);

    if (error.isNotEmpty())
    {
        showStatus (ko ("장치를 열지 못했습니다: ") + error, true);
        updateDeviceNames();
        return;
    }

    engine.openAllChannels();
    deviceChanged();
}

void MainComponent::deviceChanged()
{
    settings.setAudioDeviceState (engine.getDeviceManager().createStateXml().get());
    updateDeviceNames();
    rebuildCards();

    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
        document.setDeviceInfo (device->getName(), device->getCurrentBufferSizeSamples(), device->getCurrentSampleRate());
}

//==============================================================================
void MainComponent::timerCallback()
{
    for (auto& card : cards)
        card->pushMeter (engine.readChannelMeter (card->getChannelId()));

    for (const auto& f : document.getSession().fx)
    {
        const auto m = engine.readFxMeter (f.id);

        if (drawer == Drawer::fx)
            fxDrawer.pushMeter (f.id, m);
    }

    masterCard.pushMeter (engine.readMasterMeter());

    const bool running = engine.isDeviceRunning();
    topBar.setStatus (engine.getSampleRate(), engine.getBlockSize(), engine.getLatencyMs(), engine.getDspLoad(), running);
    masterCard.setLatency (running ? engine.getLatencyMs() : 0.0, running ? engine.getBlockSize() : 0, engine.getSampleRate());

    const double now = juce::Time::getMillisecondCounterHiRes();

    if (now < statusUntilMs)
        statusLeft.setText (statusText, juce::dontSendNotification);
    else
        statusLeft.setText ((running ? ko ("오디오 동작 중") : ko ("오디오 멈춤 - 설정에서 ASIO 장치를 확인하세요")) + "   " + ko ("끊김 ") + juce::String (engine.getXRunCount()) + ko ("회"),
                            juce::dontSendNotification);

    // autosave: a while after the last change, when the session has a file
    if (document.isDirty() && document.hasFile() && lastChangeMs > 0.0 && now - lastChangeMs > settings.getAutosaveSeconds() * 1000.0)
    {
        lastChangeMs = -1.0;
        autosaveNow();
    }
}

void MainComponent::autosaveNow()
{
    if (! document.isDirty() || ! document.hasFile())
        return;

    const auto result = document.saveIfPossible();

    if (result.failed())
        showStatus (ko ("자동 저장 실패: ") + result.getErrorMessage(), true);
    else
        topBar.refresh();
}

void MainComponent::showStatus (const juce::String& text, bool error)
{
    statusText = text;
    statusUntilMs = juce::Time::getMillisecondCounterHiRes() + (error ? 8000.0 : 4000.0);
    statusLeft.setColour (juce::Label::textColourId, error ? Palette::danger : Palette::dimText);
    statusLeft.setText (text, juce::dontSendNotification);
}

//==============================================================================
juce::File MainComponent::defaultSessionFolder() const
{
    auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LiveMix");
    folder.createDirectory();
    return folder;
}

void MainComponent::newSession()
{
    autosaveNow();
    document.newSession();
    showStatus (ko ("새 세션"));
}

void MainComponent::openSession (const juce::File& file)
{
    autosaveNow();
    juce::StringArray warnings;
    const auto result = document.load (file, &warnings);

    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("세션 열기 실패"), result.getErrorMessage(), ko ("확인"));
        return;
    }

    settings.setLastSessionFile (file);
    settings.addRecentSession (file);
    showStatus (ko ("열림: ") + file.getFileName());

    if (! warnings.isEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("세션을 열었지만 확인이 필요합니다"), warnings.joinIntoString ("\n"), ko ("확인"));
}

void MainComponent::openSessionDialog()
{
    chooser = std::make_unique<juce::FileChooser> (ko ("세션 열기"), document.hasFile() ? document.getFile().getParentDirectory() : defaultSessionFolder(), "*.livemix");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file.existsAsFile())
            openSession (file);
    });
}

bool MainComponent::saveSession()
{
    if (! document.hasFile())
    {
        saveSessionAs();
        return false;
    }

    const auto result = document.saveIfPossible();

    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("저장 실패"), result.getErrorMessage(), ko ("확인"));
        return false;
    }

    settings.setLastSessionFile (document.getFile());
    settings.addRecentSession (document.getFile());
    showStatus (ko ("저장됨: ") + document.getFile().getFileName());
    return true;
}

void MainComponent::saveSessionAs()
{
    const auto suggested = (document.hasFile() ? document.getFile().getParentDirectory() : defaultSessionFolder())
                               .getChildFile ((document.getSession().name.isNotEmpty() ? document.getSession().name : ko ("세션")) + MixSession::fileExtension);
    chooser = std::make_unique<juce::FileChooser> (ko ("세션 저장"), suggested, "*.livemix");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();

        if (file == juce::File())
            return;

        if (! file.hasFileExtension (MixSession::fileExtension))
            file = file.withFileExtension (MixSession::fileExtension);

        if (document.getSession().name.isEmpty() || document.getSession().name == ko ("새 세션"))
            document.setSessionName (file.getFileNameWithoutExtension());

        const auto result = document.save (file);

        if (result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("저장 실패"), result.getErrorMessage(), ko ("확인"));
            return;
        }

        settings.setLastSessionFile (file);
        settings.addRecentSession (file);
        showStatus (ko ("저장됨: ") + file.getFileName());
    });
}

void MainComponent::openFromCommandLine (const juce::String& commandLine)
{
    const juce::ArgumentList args ("LiveMix", commandLine);

    for (const auto& arg : args.arguments)
    {
        const auto file = arg.resolveAsFile();

        if (file.existsAsFile() && file.hasFileExtension (MixSession::fileExtension))
        {
            openSession (file);
            return;
        }
    }
}

void MainComponent::showSessionMenu (juce::Component* anchor)
{
    juce::PopupMenu menu;
    menu.addItem (1, ko ("새 세션"));
    menu.addItem (2, ko ("열기..."));
    menu.addItem (3, ko ("저장"));
    menu.addItem (4, ko ("다른 이름으로 저장..."));
    menu.addSeparator();
    menu.addItem (5, ko ("세션 이름 바꾸기..."));

    const auto recent = settings.getRecentSessions();

    if (! recent.isEmpty())
    {
        juce::PopupMenu recentMenu;

        for (int i = 0; i < recent.size(); ++i)
            recentMenu.addItem (100 + i, juce::File (recent[i]).getFileNameWithoutExtension());

        menu.addSeparator();
        menu.addSubMenu (ko ("최근 세션"), recentMenu);
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor), [safeThis, recent] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        auto& self = *safeThis;

        switch (result)
        {
            case 1: self.newSession(); break;
            case 2: self.openSessionDialog(); break;
            case 3: self.saveSession(); break;
            case 4: self.saveSessionAs(); break;
            case 5:
            {
                auto* alert = new juce::AlertWindow (ko ("세션 이름"), ko ("이 세션의 이름 (창 제목과 백업 파일에 씁니다)"), juce::MessageBoxIconType::NoIcon);
                alert->addTextEditor ("name", self.document.getSession().name, ko ("이름"));
                alert->addButton (ko ("확인"), 1, juce::KeyPress (juce::KeyPress::returnKey));
                alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
                alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert] (int r)
                {
                    if (safeThis != nullptr && r == 1)
                        safeThis->document.setSessionName (alert->getTextEditorContents ("name"));
                }), true);
                break;
            }

            default:
                if (result >= 100 && result - 100 < recent.size())
                    self.openSession (juce::File (recent[result - 100]));
                break;
        }
    });
}

void MainComponent::showHelpMenu (juce::Component* anchor)
{
    juce::PopupMenu menu;
    menu.addItem (1, ko ("커뮤니티 (카카오톡 오픈채팅)"));
    menu.addItem (2, ko ("업데이트 확인..."), Updater::isAvailable());
    menu.addSeparator();
    menu.addItem (3, ko ("LiveMix 정보"));

    juce::Component::SafePointer<MainComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor), [safeThis] (int result)
    {
        if (safeThis == nullptr)
            return;

        if (result == 1)
            juce::URL (Links::feedbackChat).launchInDefaultBrowser();
        else if (result == 2)
            Updater::checkForUpdatesWithUI();
        else if (result == 3)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "LiveMix " + juce::JUCEApplication::getInstance()->getApplicationVersion(),
                                                    ko ("방송용 라이브 마이크 VST3 호스트\n곰튀김\n\n") + "VST is a trademark of Steinberg Media Technologies GmbH.", ko ("확인"));
    });
}

void MainComponent::showBackupDialog()
{
    if (! document.hasFile())
    {
        showStatus (ko ("먼저 세션을 저장하세요 (세션 > 저장)"), true);
        saveSessionAs();
        return;
    }

    autosaveNow();
    BackupDialog::show (document, settings, this, [this] (const juce::String& message, bool error) { showStatus (message, error); });
}

void MainComponent::showSettingsDialog()
{
    SettingsDialog::show (engine, settings, this, [this] { deviceChanged(); });
}

} // namespace gocue::livemix
