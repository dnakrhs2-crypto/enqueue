#include "MainComponent.h"

#include "BackupDialog.h"
#include "SettingsDialog.h"
#include "app/Links.h"
#include "app/Updater.h"

#include <cmath>

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

    noticeLabel.setFont (bodyFont (14.0f));
    noticeLabel.setJustificationType (juce::Justification::centredLeft);
    noticeLabel.setMinimumHorizontalScale (1.0f);
    addChildComponent (noticeLabel);
    noticeClose.setWantsKeyboardFocus (false);
    noticeClose.setTooltip (ko ("닫기"));
    noticeClose.onClick = [this] { hideNotice(); };
    addChildComponent (noticeClose);

    windows.onChainChanged = [this] (PluginChain&) { document.markDirty(); };
    engine.forEachChain ([this] (PluginChain& chain) { chain.setListener (&windows); });

    document.onStructureChanged = [this]
    {
        engine.forEachChain ([this] (PluginChain& chain) { chain.setListener (&windows); });
        rebuildCards();

        if (document.isDirty())   // a load announces too: that must not arm the autosave
            lastChangeMs = juce::Time::getMillisecondCounterHiRes();
    };
    document.onValueChanged = [this]
    {
        refreshValues();

        if (document.isDirty())   // a save announces too
            lastChangeMs = juce::Time::getMillisecondCounterHiRes();
    };

    updateDeviceNames();
    rebuildCards();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    backup.cancel();
    SettingsDialog::closeIfOpen();
    pluginManagerDialog.deleteAndZero();
    juce::ModalComponentManager::getInstance()->cancelAllModalComponents();   // open alerts refer to this window and its document
    windows.closeAll();
    engine.forEachChain ([] (PluginChain& chain) { chain.setListener (nullptr); });   // the window manager dies here: no chain may call it afterwards
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
            chainDrawer.setChain (chain, titleForChainOwner());   // the same owner, a rebuilt chain (a file was opened)
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

    if (noticeVisible)
    {
        // as many lines as the text needs (up to five), the close button on the right
        const int textWidth = juce::jmax (100, area.getWidth() - 32 - 44);
        const int lines = juce::jlimit (1, 5, (int) std::ceil (juce::GlyphArrangement::getStringWidth (noticeLabel.getFont(), noticeLabel.getText()) / (float) textWidth)
                                             + noticeLabel.getText().length() - noticeLabel.getText().replace ("\n", "").length());
        auto bar = area.removeFromTop (14 + lines * 20);
        noticeClose.setBounds (bar.removeFromRight (44).reduced (8, (bar.getHeight() - 28) / 2));
        noticeLabel.setBounds (bar.reduced (16, 4));
    }

    noticeLabel.setVisible (noticeVisible);
    noticeClose.setVisible (noticeVisible);
    auto status = area.removeFromBottom (30);
    statusLeft.setBounds (status.reduced (16, 0).removeFromLeft (status.getWidth() / 2));
    statusRight.setBounds (status.reduced (16, 0).removeFromRight (status.getWidth() / 2));

    const int drawerW = juce::jmin (440, juce::jmax (320, getWidth() / 3));
    const auto drawerArea = area.withLeft (getWidth() - drawerW);   // the right edge, over the cards and the master
    chainDrawer.setBounds (drawerArea);
    fxDrawer.setBounds (drawerArea);
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

    if (noticeVisible)
    {
        auto bar = noticeLabel.getBounds().getUnion (noticeClose.getBounds()).expanded (16, 4).withX (0).withWidth (getWidth());
        g.setColour (noticeIsError ? Palette::danger.withAlpha (0.18f) : Palette::accent.withAlpha (0.18f));
        g.fillRect (bar);
        g.setColour (noticeIsError ? Palette::danger : Palette::accent);
        g.fillRect (bar.removeFromLeft (4));
    }
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
    if (which != Drawer::chain && chainDrawer.getChain() != nullptr)
        chainDrawer.setChain (nullptr, {});   // a closed drawer keeps no chain: nothing deferred may reach one that is gone

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

juce::String MainComponent::titleForChainOwner() const
{
    if (chainOwnerId.isNull())
        return ko ("마스터");

    if (chainIsFx)
    {
        if (const auto* f = document.getSession().findFx (chainOwnerId))
            return "FX " + f->name;
    }
    else if (const auto* ch = document.getSession().findChannel (chainOwnerId))
    {
        return ch->name;
    }

    return {};
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
    if (pluginManagerDialog != nullptr)
    {
        pluginManagerDialog->toFront (true);
        return;
    }

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
    pluginManagerDialog = options.launchAsync();   // closed with this component: it refers to the engine's plugin list
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
        if (device->getTypeName().containsIgnoreCase ("ASIO"))   // another type's device (never opened by us) stays out of the pickers
        {
            current = device->getName();
            inputNames = device->getInputChannelNames();
            outputNames = device->getOutputChannelNames();
        }
    }

    topBar.setDevices (asio, current);
}

void MainComponent::chooseDevice (const juce::String& name)
{
    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice(); device != nullptr && device->getName() == name && engine.isDeviceRunning())
        return;

    const auto error = engine.openDevice (name);   // the ASIO type, every channel and the callback, whatever ran before (safe mode included)

    if (error.isNotEmpty())
    {
        showStatus (error, true);
        updateDeviceNames();
        return;
    }

    deviceChosen();
}

void MainComponent::deviceChanged()
{
    // any change of the device manager (a pick, a fallback, a hot-plug): names, pickers, the saved state - not the
    // session, which keeps asking for the device it was saved with until the operator picks another one
    settings.setAudioDeviceState (engine.getDeviceManager().createStateXml().get());
    updateDeviceNames();
    rebuildCards();
}

void MainComponent::deviceChosen()
{
    deviceChanged();

    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
        if (device->getTypeName().containsIgnoreCase ("ASIO"))
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

    // a knob turned in a plugin editor: the file is out of date (the views need no refresh for that)
    if (document.pollPluginEdits())
        lastChangeMs = now;

    // autosave: a while after the last change, when the session has a file
    if (document.isDirty() && document.hasFile() && lastChangeMs > 0.0 && now - lastChangeMs > settings.getAutosaveSeconds() * 1000.0)
    {
        lastChangeMs = -1.0;

        if (! autosaveNow())
            lastChangeMs = now;   // a failed write is tried again after another interval (the status line says why)
    }
}

bool MainComponent::autosaveNow()
{
    if (! document.isDirty() || ! document.hasFile())
        return true;   // nothing to write

    const auto result = document.saveIfPossible();

    if (result.failed())
    {
        showStatus (ko ("자동 저장 실패: ") + result.getErrorMessage(), true);
        return false;
    }

    topBar.refresh();
    return true;
}

void MainComponent::showNotice (const juce::String& text, bool error)
{
    noticeVisible = true;
    noticeIsError = error;
    noticeLabel.setText (text, juce::dontSendNotification);
    noticeLabel.setColour (juce::Label::textColourId, Palette::text);
    resized();
    repaint();
}

void MainComponent::hideNotice()
{
    if (! noticeVisible)
        return;

    noticeVisible = false;
    resized();
    repaint();
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

void MainComponent::withSessionSecured (std::function<void()> action)
{
    document.pollPluginEdits();   // a knob turned since the last timer tick counts

    if (! document.isDirty())
    {
        action();
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);

    if (document.hasFile())
    {
        const auto result = document.saveIfPossible();

        if (result.wasOk())
        {
            topBar.refresh();
            action();
            return;
        }

        auto* alert = new juce::AlertWindow (ko ("저장 실패"),
                                             ko ("세션을 저장하지 못했습니다:\n") + result.getErrorMessage() + ko ("\n\n저장하지 않은 변경을 버리고 계속할까요?"),
                                             juce::MessageBoxIconType::WarningIcon, this);
        alert->addButton (ko ("버리고 계속"), 1);
        alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, action] (int r)
        {
            if (safeThis != nullptr && r == 1)
                action();
        }), true);
        return;
    }

    auto* alert = new juce::AlertWindow (ko ("저장하지 않은 세션"), ko ("이 세션은 아직 파일로 저장되지 않았습니다. 저장할까요?"),
                                         juce::MessageBoxIconType::QuestionIcon, this);
    alert->addButton (ko ("저장"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("저장 안 함"), 2);
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, action] (int r)
    {
        if (safeThis == nullptr || r == 0)
            return;

        if (r == 2)
        {
            action();
            return;
        }

        safeThis->saveSessionAs ([safeThis, action] (bool saved)
        {
            if (safeThis != nullptr && saved)
                action();
        });
    }), true);
}

void MainComponent::newSession()
{
    withSessionSecured ([this]
    {
        document.newSession();
        showStatus (ko ("새 세션"));
    });
}

void MainComponent::openSession (const juce::File& file)
{
    withSessionSecured ([this, file] { loadSession (file); });
}

void MainComponent::loadSession (const juce::File& file)
{
    juce::StringArray warnings, pluginErrors;
    const auto result = document.load (file, &warnings, &pluginErrors);

    if (result.failed())
    {
        showNotice (ko ("세션 열기 실패: ") + result.getErrorMessage(), true);
        return;
    }

    hideNotice();
    settings.setLastSessionFile (file);
    settings.addRecentSession (file);
    showStatus (ko ("열림: ") + file.getFileName());

    if (safeMode)
    {
        if (! pluginErrors.isEmpty())   // the parser's own warnings (skipped entries) stay: they are data the operator must know about
            warnings.add (ko ("안전 모드: 플러그인과 세션의 장치를 불러오지 않았습니다 (설정은 세션에 그대로 남습니다)"));
    }
    else
    {
        warnings.addArray (pluginErrors);

        // the device the session was saved with: a show saved for interface B must not run on A without a word
        const auto deviceWarning = engine.openSessionDevice (document.getSession().device);

        if (deviceWarning.isNotEmpty())
        {
            warnings.insert (0, deviceWarning);
            deviceChanged();
        }
        else
        {
            deviceChosen();   // the buffer / rate the device really runs at
        }
    }

    if (! warnings.isEmpty())
        showNotice (ko ("세션을 열었지만 확인이 필요합니다: ") + warnings.joinIntoString ("\n"), true);
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

void MainComponent::saveSessionAs (std::function<void (bool)> then)
{
    const auto suggested = (document.hasFile() ? document.getFile().getParentDirectory() : defaultSessionFolder())
                               .getChildFile ((document.getSession().name.isNotEmpty() ? document.getSession().name : ko ("세션")) + MixSession::fileExtension);
    chooser = std::make_unique<juce::FileChooser> (ko ("세션 저장"), suggested, "*.livemix");
    juce::Component::SafePointer<MainComponent> safeThis (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safeThis, then] (const juce::FileChooser& fc)
    {
        if (safeThis == nullptr)
            return;

        auto& self = *safeThis;
        auto file = fc.getResult();

        if (file == juce::File())
        {
            if (then)
                then (false);

            return;
        }

        if (! file.hasFileExtension (MixSession::fileExtension))
            file = file.withFileExtension (MixSession::fileExtension);

        if (self.document.getSession().name.isEmpty() || self.document.getSession().name == ko ("새 세션"))
            self.document.setSessionName (file.getFileNameWithoutExtension());

        const auto result = self.document.save (file);

        if (result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("저장 실패"), result.getErrorMessage(), ko ("확인"));

            if (then)
                then (false);

            return;
        }

        self.settings.setLastSessionFile (file);
        self.settings.addRecentSession (file);
        self.showStatus (ko ("저장됨: ") + file.getFileName());

        if (then)
            then (true);
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
    juce::Component::SafePointer<MainComponent> safeThis (this);

    if (! document.hasFile())
    {
        showStatus (ko ("먼저 세션을 저장하세요 (세션 > 저장)"), true);
        saveSessionAs ([safeThis] (bool saved)
        {
            if (safeThis != nullptr && saved)
                safeThis->showBackupDialog();
        });
        return;
    }

    document.pollPluginEdits();

    if (! autosaveNow())
    {
        showStatus (ko ("세션을 저장하지 못해 백업을 시작하지 않았습니다"), true);   // an upload of the stale file would pass for a backup
        return;
    }

    BackupDialog::show (document, settings, backup, this, [safeThis] (const juce::String& message, bool error)
    {
        if (safeThis != nullptr)
            safeThis->showStatus (message, error);
    });
}

void MainComponent::showSettingsDialog()
{
    SettingsDialog::show (engine, settings, this, [this] { deviceChosen(); });
}

} // namespace gocue::livemix
