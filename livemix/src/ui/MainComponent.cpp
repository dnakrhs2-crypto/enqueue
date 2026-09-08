#include "MainComponent.h"

#include "BackupDialog.h"
#include "SettingsDialog.h"
#include "app/Links.h"
#include "app/Updater.h"

#include <cmath>

namespace gocue::livemix
{

MainComponent::MainComponent (MixDocument& doc, LiveMixSettings& s)
    : document (doc), settings (s), engine (doc.getEngine()), topBar (doc), menuBar (this), masterCard (doc), chainDrawer (doc, windows), fxDrawer (doc)
{
    setOpaque (true);
    addAndMakeVisible (menuBar);
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
    masterCard.onAddPlugin = [this] { addPluginTo (&engine.getMasterChain(), ko ("마스터"), &masterCard.getAddPluginButton()); };
    masterCard.onOpenLoudness = [this] { showLoudnessWindow(); };
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
    chainDrawer.onStatus = [this] (const juce::String& text, bool error) { showStatus (text, error); };
    chainDrawer.onPresetSaved = [this] { if (pluginManagerWindow != nullptr) pluginManagerWindow->refreshPresets(); };
    fxDrawerViewport.setViewedComponent (&fxDrawer, false);
    fxDrawerViewport.setScrollBarsShown (true, false);
    addChildComponent (fxDrawerViewport);
    fxDrawer.onClose = [this] { showDrawer (Drawer::none); };
    fxDrawer.onPreferredHeightChanged = [this] { if (drawer == Drawer::fx) layoutFxDrawer(); };
    fxDrawer.onOpenChain = [this] (const juce::Uuid& id)
    {
        if (const auto* f = document.getSession().findFx (id))
            openChainFor (engine.getFxChain (id), "FX " + f->name);
    };
    fxDrawer.onAddPlugin = [this] (const juce::Uuid& id)
    {
        if (const auto* f = document.getSession().findFx (id))
            addPluginTo (engine.getFxChain (id), "FX " + f->name, &fxDrawer.getAddPluginButton());
    };
    fxDrawer.onOpenPluginEditor = [this] (const juce::Uuid& id, int slot)
    {
        if (auto* chain = engine.getFxChain (id))
            if (slot < chain->getNumSlots() && chain->getSlot (slot).plugin != nullptr)
                windows.open (*chain->getSlot (slot).plugin, "FX - " + chain->getSlot (slot).plugin->getName());
    };

    topBar.onDeviceChosen = [this] (const juce::String& name) { chooseDevice (name); };
    topBar.onFxPanel = [this] { showDrawer (drawer == Drawer::fx ? Drawer::none : Drawer::fx); };
    topBar.onPluginManager = [this] { showPluginManager(); };
    topBar.onHeightChanged = [this] { resized(); };

    statusLeft.setFont (bodyFont (12.5f));
    statusLeft.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (statusLeft);
    statusRight.setFont (bodyFont (12.5f));
    statusRight.setColour (juce::Label::textColourId, Palette::dimText);
    statusRight.setJustificationType (juce::Justification::centredRight);
    statusRight.setText (ko ("최소화하면 트레이에서 계속 동작합니다"), juce::dontSendNotification);
    addAndMakeVisible (statusRight);

    noticeText.setMultiLine (true, true);
    noticeText.setReadOnly (true);
    noticeText.setCaretVisible (false);
    noticeText.setScrollbarsShown (true);
    noticeText.setPopupMenuEnabled (true);   // the text can be copied (a driver's error, a path)
    noticeText.setFont (bodyFont (14.0f));
    noticeText.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    noticeText.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    noticeText.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    noticeText.setColour (juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    noticeText.setColour (juce::TextEditor::textColourId, Palette::text);
    addChildComponent (noticeText);
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
        if (sessionGeneration != document.getSessionGeneration())
        {
            sessionGeneration = document.getSessionGeneration();
            muteGroups.reset();   // a new session is observed only after its runtime groups have been released
        }
        else
            muteGroups.apply();   // rebuilt nodes start unmuted: the groups' state goes back in

        if (controlServer != nullptr)
            controlServer->documentChanged (ControlServer::ChangeKind::structure);
    };
    document.onValueChanged = [this]
    {
        refreshValues();
        if (controlServer != nullptr)
            controlServer->documentChanged (ControlServer::ChangeKind::values);
    };

    muteGroups.onChanged = [this]
    {
        muteGroupsChanged();
        if (controlServer != nullptr)
            controlServer->muteGroupsChanged();
    };
    hotkeys.onHotkey = [this] (int id)
    {
        switch (id)
        {
            case 1: muteGroups.toggle (MuteGroups::Group::mic); break;
            case 2: muteGroups.toggle (MuteGroups::Group::fx); break;
            case 3: if (onToggleWindow) onToggleWindow(); break;
            default: break;
        }
    };
    registerHotkeys();

    updateDeviceNames();
    rebuildCards();
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    detachControlServer();   // also covers a window destroyed independently of the app's normal shutdown
    backup.cancel();
    SettingsDialog::closeIfOpen();
    BackupDialog::closeIfOpen();   // its content refers to the document and the backup thread
    pluginManagerWindow.reset();
    loudnessWindow.reset();
    pluginGroupsWindow.reset();
    juce::ModalComponentManager::getInstance()->cancelAllModalComponents();   // open alerts refer to this window and its document
    windows.closeAll();
    engine.forEachChain ([] (PluginChain& chain) { chain.setListener (nullptr); });   // the window manager dies here: no chain may call it afterwards
    document.onStructureChanged = nullptr;
    document.onValueChanged = nullptr;
}

void MainComponent::attachControlServer (ControlServer* server)
{
    if (controlServer == server)
        return;

    detachControlServer();
    controlServer = server;
}

void MainComponent::detachControlServer()
{
    if (controlServer != nullptr)
        controlServer->stop();   // joins workers and invalidates queued message callbacks before MuteGroups dies

    controlServer = nullptr;
    onExternalControlEnabled = nullptr;
}

ControlServer::Status MainComponent::getExternalControlStatus() const
{
    return controlServer != nullptr ? controlServer->getStatus() : ControlServer::Status {};
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
            card->onAddPlugin = [this, anchor = card.get()] (const juce::Uuid& id)
            {
                if (const auto* ch = document.getSession().findChannel (id))
                    addPluginTo (engine.getChannelChain (id), ch->name, &anchor->getAddPluginButton());   // the menu next to the button
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
            card->onOpenPluginGroups = [this] (const juce::Uuid& id) { showPluginGroups (id); };
            card->setGroupMuted (muteGroups.isMuted (MuteGroups::Group::mic));
            cardsHolder.addAndMakeVisible (*card);
        }

        card->setDeviceChannels (inputNames, outputNames);
        card->refresh();
        next.push_back (std::move (card));
    }

    cards = std::move (next);
    masterCard.setDeviceChannels (outputNames);
    fxDrawer.setDeviceChannels (outputNames);

    if (pluginGroupsWindow != nullptr)
        pluginGroupsWindow->refresh();   // its channel may be gone, its chain rebuilt
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
    muteGroups.apply();   // a '뮤트그룹' chip may have changed while its group is muted

    for (auto& card : cards)
        card->refresh();

    masterCard.refresh();

    if (masterCard.getUnfoldedHeight (masterCard.getWidth()) != masterUnfoldedH)
        resized();   // more or fewer chip rows: the master's height (folded or not), and the room above it

    fxDrawer.refresh();

    if (pluginGroupsWindow != nullptr)
        pluginGroupsWindow->refresh();
    layoutFxDrawer();
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
        const int h = card->getPreferredHeight (width);
        card->setBounds (0, y, width, h);
        y += h + gap;
    }

    addChannelButton.setBounds (0, y, width, 56);
    addChannelButton.setEnabled ((int) cards.size() < MixSession::maxChannels);
    y += 56 + gap;
    cardsHolder.setSize (width, juce::jmax (1, y));

    // the new height may have brought the scrollbar (or taken it): the width changed, so once more at that width
    if (! relayingOutCards && juce::jmax (100, viewport.getMaximumVisibleWidth()) != width)
    {
        const juce::ScopedValueSetter<bool> once (relayingOutCards, true);
        layoutCards();
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    menuBar.setBounds (area.removeFromTop (30));
    topBar.setBounds (area.removeFromTop (topBar.preferredHeight (getWidth())));   // two or three rows in a narrow window

    if (noticeVisible)
    {
        // the height the text really takes at this width (measured by the editor itself, no scrollbar), up to about
        // five lines; a longer notice scrolls. The close button sits on the right.
        const int textWidth = juce::jmax (100, area.getWidth() - 32 - 44);
        const int maxTextHeight = 128;   // about five lines
        noticeText.setScrollbarsShown (false);
        noticeText.setBounds (16, area.getY() + 7, textWidth, 1);   // lays the text out at this width
        const int needed = noticeText.getTextHeight() + 6;
        const int textHeight = juce::jlimit (26, maxTextHeight, needed);
        noticeText.setScrollbarsShown (needed > maxTextHeight);
        auto bar = area.removeFromTop (textHeight + 14);
        noticeClose.setBounds (bar.removeFromRight (48).reduced (7, juce::jmax (0, (bar.getHeight() - 34) / 2)));
        noticeText.setBounds (bar.reduced (16, 7));
    }

    noticeText.setVisible (noticeVisible);
    noticeClose.setVisible (noticeVisible);
    auto status = area.removeFromBottom (30);
    const bool narrowStatus = getWidth() < 700;   // portrait: a short tray hint, the rest of the line for the status
    statusRight.setText (narrowStatus ? ko ("최소화·X → 트레이") : ko ("최소화하면 트레이에서 계속 동작합니다"), juce::dontSendNotification);
    auto statusR = status.reduced (16, 0);
    statusR.removeFromRight (18);   // room for the grip
    statusRight.setBounds (statusR.removeFromRight (narrowStatus ? 124 : statusR.getWidth() / 2));
    statusLeft.setBounds (statusR);

    if (cornerGrip != nullptr)
    {
        auto* window = findParentComponentOfClass<juce::ResizableWindow>();
        cornerGrip->setVisible (window != nullptr && ! window->isFullScreen());   // a maximised window is not dragged
        cornerGrip->setBounds (getLocalBounds().removeFromBottom (18).removeFromRight (18));
    }

    // a side column only when the cards keep a usable width beside it (780+); narrower, the drawer takes the whole width
    const bool sideDrawer = getWidth() >= 1100;
    const int drawerW = sideDrawer ? juce::jmin (440, juce::jmax (320, getWidth() / 3)) : getWidth();
    const auto drawerArea = area.withLeft (getWidth() - drawerW);   // the right edge, over the cards and the master
    chainDrawer.setBounds (drawerArea);
    fxDrawerViewport.setBounds (drawerArea);
    chainDrawer.setVisible (drawer == Drawer::chain);
    fxDrawerViewport.setVisible (drawer == Drawer::fx);

    if (drawer == Drawer::fx)
        layoutFxDrawer();   // (a hidden drawer is laid out when it opens)

    if (drawer != Drawer::none)
        area.setRight (getWidth() - drawerW);

    // the master takes its full form only while the mics keep a card's worth of room; otherwise it folds to a strip
    const int cardWidth = area.getWidth() - 32;
    masterCard.setStrip (false);
    masterUnfoldedH = masterCard.getPreferredHeight (cardWidth);   // grows with its chip rows
    int masterH = masterUnfoldedH;

    if (area.getHeight() - (masterH + 16) - 24 < minCardsRoom)   // 24: the viewport's margins below
    {
        masterCard.setStrip (true);
        masterH = masterCard.getPreferredHeight (cardWidth);
    }

    masterCard.setBounds (area.removeFromBottom (masterH + 16).reduced (16, 8));   // the 8 px above and below are the layout's, not the card's
    viewport.setBounds (area.reduced (16, 12));
    layoutCards();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);

    if (noticeVisible)
    {
        auto bar = noticeText.getBounds().getUnion (noticeClose.getBounds()).expanded (16, 7).withX (0).withWidth (getWidth());
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
    g.fillRect (juce::Rectangle<int> (0, masterCard.getY() - 8, masterCard.getRight() + 16, 1));   // above the master, whatever its height
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

    // where the button is now: opening the drawer narrows the cards and may move it (or hide it, in the FX drawer)
    const auto anchorArea = anchor != nullptr ? anchor->getScreenBounds() : juce::Rectangle<int>();
    openChainFor (chain, title);
    chainDrawer.showAddMenu (anchorArea.isEmpty() ? chainDrawer.getScreenBounds() : anchorArea);
}

void MainComponent::showPluginManager()
{
    if (pluginManagerWindow == nullptr)
    {
        pluginManagerWindow = std::make_unique<PluginManagerWindow> (engine.getPluginHost(), settings, PluginPreset::defaultFolder());
        pluginManagerWindow->centreAroundComponent (this, pluginManagerWindow->getWidth(), pluginManagerWindow->getHeight());   // on this display, inside it
        pluginManagerWindow->onVst2Changed = [this] (bool on)
        {
            // the switch changes what the menus offer and what a session opened from now on loads; the chains of
            // the session already open stay as they are until it is opened again - so say so, and offer to
            int vst2Slots = 0;
            const auto& session = document.getSession();
            auto count = [&vst2Slots] (const std::vector<PluginSlotState>& chain)
            {
                for (const auto& slot : chain)
                    if (slot.format == "VST")
                        ++vst2Slots;
            };

            for (const auto& c : session.channels)
                count (c.chain);

            for (const auto& f : session.fx)
                count (f.chain);

            count (session.master.chain);

            if (vst2Slots == 0)
            {
                showStatus (on ? ko ("VST2 플러그인 사용: 켬 (이제 열거나 넣는 것부터)") : ko ("VST2 플러그인 사용: 끔 (이제 열거나 넣는 것부터)"));
                return;
            }

            const auto slots = juce::String (vst2Slots);

            if (! document.hasFile())
            {
                showStatus (on ? ko ("이 세션의 VST2 자리 ") + slots + ko ("개는 세션을 저장한 뒤 다시 열면 채워집니다")
                               : ko ("이 세션의 VST2 플러그인 ") + slots + ko ("개는 세션을 저장한 뒤 다시 열어야 빠집니다"), true);
                return;
            }

            showStatus (on ? ko ("VST2 플러그인 사용: 켬") : ko ("VST2 플러그인 사용: 끔"));
            juce::Component::SafePointer<MainComponent> safeThis (this);
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle (on ? ko ("VST2 자리 채우기") : ko ("VST2 플러그인 빼기"))
                                              .withMessage (on ? ko ("이 세션에 VST2 플러그인 자리가 ") + slots + ko ("개 있습니다 (VST2가 꺼져 있어 비워 둔 자리).") + juce::newLine
                                                                     + ko ("세션을 다시 열어 그 자리를 채울까요?")
                                                               : ko ("이 세션의 VST2 플러그인 ") + slots + ko ("개는 세션을 다시 열 때까지 그대로 동작합니다.") + juce::newLine
                                                                     + ko ("지금 세션을 다시 열어 그 자리를 비울까요? (세션 파일은 그대로, 다시 켜면 돌아옵니다)"))
                                              .withButton (ko ("다시 열기"))
                                              .withButton (ko ("나중에")),
                                          [safeThis] (int result)
            {
                if (safeThis != nullptr && result == 1 && safeThis->document.hasFile())
                    safeThis->openSession (safeThis->document.getFile());
            });
        };
    }

    pluginManagerWindow->open();
}

void MainComponent::showPluginGroups (const juce::Uuid& channelId)
{
    if (pluginGroupsWindow == nullptr)
    {
        pluginGroupsWindow = std::make_unique<PluginGroupsWindow> (document);
        pluginGroupsWindow->centreAroundComponent (this, pluginGroupsWindow->getWidth(), pluginGroupsWindow->getHeight());   // on this display, inside it
    }

    pluginGroupsWindow->open (channelId);
}

void MainComponent::showLoudnessWindow()
{
    if (loudnessWindow == nullptr)
    {
        loudnessWindow = std::make_unique<LoudnessWindow> (engine, settings);
        loudnessWindow->centreAroundComponent (this, loudnessWindow->getWidth(), loudnessWindow->getHeight());   // on this display, inside it
    }

    loudnessWindow->open();
}

//==============================================================================
static constexpr int maxDeviceChannelsShown = MixSession::maxDeviceChannels;

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
            inputNames.removeRange (maxDeviceChannelsShown, inputNames.size());     // the graph opens 64 at most: no picker beyond them
            outputNames.removeRange (maxDeviceChannelsShown, outputNames.size());
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

    if (startupNote.isNotEmpty() && ! startupNoteIsSafeMode && engine.isDeviceRunning())
        setStartupNote ({}, false, false);   // the startup "ASIO 장치를 열지 못했습니다" is over: a device runs

    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
        if (device->getTypeName().containsIgnoreCase ("ASIO"))
            document.setDeviceInfo (device->getName(), device->getCurrentBufferSizeSamples(), device->getCurrentSampleRate());
}

//==============================================================================
void MainComponent::timerCallback()
{
    engine.getLoudnessMeter().poll();   // the LUFS numbers keep up whether or not their window is open (an hour-long stream reads whole)
    const auto inView = viewport.getViewArea();   // a tall window scrolls: every meter is read and advances, only the cards on screen repaint

    for (auto& card : cards)
        card->pushMeter (engine.readChannelMeter (card->getChannelId()), card->getBounds().intersects (inView));

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
    {
        statusLeft.setText (statusText, juce::dontSendNotification);
    }
    else
    {
        statusLeft.setColour (juce::Label::textColourId, Palette::dimText);   // an error's red goes with its text
        statusLeft.setText ((running ? ko ("오디오 동작 중") : ko ("오디오 멈춤 - 설정에서 ASIO 장치를 확인하세요")) + "   " + ko ("끊김 ") + juce::String (engine.getXRunCount()) + ko ("회"),
                            juce::dontSendNotification);
    }

    document.pollPluginEdits();   // a knob turned in a plugin editor: the title shows the session as changed

    // a plugin that faulted (threw, or produced NaN / Inf): dry from then on, and the operator is told once
    juce::StringArray faulted, stalled;
    engine.forEachChain ([&] (PluginChain& chain) { faulted.addArray (chain.takeNewFaults()); stalled.addArray (chain.takeNewStalls()); });
    bool noteChanged = false;

    for (const auto& name : faulted)
        if (! faultedPlugins.contains (name))
        {
            faultedPlugins.add (name);
            noteChanged = true;
        }

    for (const auto& name : stalled)
        if (! stalledPlugins.contains (name))
        {
            stalledPlugins.add (name);
            noteChanged = true;
        }

    if (noteChanged)
    {
        // every plugin told of so far stays in the line (a later fault must not push an earlier one out)
        pluginNote.clear();

        if (! faultedPlugins.isEmpty())
            pluginNote << ko ("플러그인 오류로 꺼짐: ") << faultedPlugins.joinIntoString (", ") << ko (" - 그 자리는 소리를 그대로 통과시킵니다. 체인에서 빼거나 다시 넣으세요.");

        if (! stalledPlugins.isEmpty())
            pluginNote << (pluginNote.isEmpty() ? "" : "\n") << ko ("응답이 없어 건너뛰는 중: ") << stalledPlugins.joinIntoString (", ") << ko (" - 그 플러그인이 다시 답하면 소리가 돌아옵니다.");

        refreshNotice();
    }

    // a plugin with latency inside a mic chain: its pre-fader send and its direct output no longer line up with the
    // master (checked once a second; the line comes and goes with the chains)
    if (--ticksUntilLatencyCheck <= 0)
    {
        ticksUntilLatencyCheck = 30;
        int worst = 0;

        for (const auto& c : document.getSession().channels)
            if (auto* chain = engine.getChannelChain (c.id))
                worst = juce::jmax (worst, chain->getLatencySamples());

        const auto sr = juce::jmax (1.0, engine.getSampleRate());
        const juce::String text = worst > 0 ? ko ("마이크 체인에 지연이 있는 플러그인이 있습니다 (") + juce::String (1000.0 * worst / sr, 1)
                                                  + ko (" ms). 프리 센드나 직접 출력을 마스터와 같이 쓰면 위상이 어긋날 수 있습니다.")
                                            : juce::String();

        if (text != latencyNote)
        {
            latencyNote = text;
            refreshNotice();
        }
    }

    // a session named on the command line while a question was open: now that it is answered
    if (pendingCommandLineFile != juce::File() && juce::Component::getCurrentlyModalComponent() == nullptr)
    {
        const auto file = pendingCommandLineFile;
        pendingCommandLineFile = juce::File();
        openSession (file);
    }
}

bool MainComponent::saveIfDirty()
{
    if (! document.isDirty() || ! document.hasFile())
        return true;   // nothing to write

    const auto result = document.saveIfPossible();

    if (result.failed())
    {
        showStatus (ko ("저장 실패: ") + result.getErrorMessage(), true);
        setSaveError (ko ("저장 실패: ") + result.getErrorMessage());
        return false;
    }

    setSaveError ({});
    topBar.refresh();
    return true;
}

void MainComponent::setSessionNote (const juce::String& text, bool error)
{
    sessionNote = text;
    sessionNoteIsError = error;
    refreshNotice();
}

void MainComponent::setStartupNote (const juce::String& text, bool error, bool safeModeNote)
{
    startupNote = text;
    startupNoteIsError = error;
    startupNoteIsSafeMode = safeModeNote;
    refreshNotice();
}

void MainComponent::setSaveError (const juce::String& message)
{
    if (saveErrorNote == message)
        return;

    saveErrorNote = message;
    refreshNotice();
}

void MainComponent::refreshNotice()
{
    juce::StringArray lines;

    if (sessionNote.isNotEmpty())
        lines.add (sessionNote);

    if (startupNote.isNotEmpty())
        lines.add (startupNote);

    if (pluginNote.isNotEmpty())
        lines.add (pluginNote);

    if (latencyNote.isNotEmpty())
        lines.add (latencyNote);

    if (saveErrorNote.isNotEmpty())
        lines.add (saveErrorNote);

    noticeVisible = ! lines.isEmpty();
    noticeIsError = (sessionNote.isNotEmpty() && sessionNoteIsError)
                    || (startupNote.isNotEmpty() && startupNoteIsError)
                    || pluginNote.isNotEmpty()
                    || saveErrorNote.isNotEmpty();
    noticeText.setText (lines.joinIntoString ("\n"), false);
    resized();
    repaint();
}

static void commitPendingRename()
{
    // a name still being typed in a card (a Label's editor) goes into the document before the session is saved
    // or replaced - the editor commits on its own only when it loses the focus, and that arrives later
    if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
        if (auto* label = dynamic_cast<juce::Label*> (focused->getParentComponent()))
            if (label->isBeingEdited())
                label->hideEditor (false);
}

bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    if (key == juce::KeyPress (juce::KeyPress::escapeKey) && drawer != Drawer::none)
    {
        showDrawer (Drawer::none);   // Esc closes the chain / FX drawer (in a narrow window it covers everything)
        return true;
    }

    // the session shortcuts, wherever the focus is in the window (a text field lets them through)
    const auto mods = key.getModifiers();

    if (! mods.isCtrlDown() || mods.isAltDown())
        return false;

    const auto code = key.getKeyCode();

    if (code == 'S' || code == 's')
    {
        commitPendingRename();

        if (mods.isShiftDown())
            saveSessionAs();
        else
            saveSession();

        return true;
    }

    if ((code == 'N' || code == 'n') && ! mods.isShiftDown())
    {
        commitPendingRename();
        newSession();
        return true;
    }

    return false;
}

void MainComponent::parentHierarchyChanged()
{
    // the grip resizes the window itself; the window's constrainer keeps the minimum size (MainWindow sets its limits
    // before the content goes in)
    if (cornerGrip == nullptr)
    {
        if (auto* window = findParentComponentOfClass<juce::ResizableWindow>())
        {
            cornerGrip = std::make_unique<juce::ResizableCornerComponent> (window, window->getConstrainer());
            cornerGrip->setAlwaysOnTop (true);
            addAndMakeVisible (*cornerGrip);
            resized();
        }
    }
}

void MainComponent::hideNotice()
{
    // the close button: every line goes
    sessionNote.clear();
    startupNote.clear();
    pluginNote.clear();
    latencyNote.clear();
    saveErrorNote.clear();
    refreshNotice();
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
            setSaveError ({});
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
            {
                safeThis->document.discardUnsavedChanges();   // the shutdown save must not resurrect what was just discarded
                action();
            }
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
        faultedPlugins.clear();
        stalledPlugins.clear();
        pluginNote.clear();
        setSessionNote ({}, false);   // the old session's notes do not describe the new one
        setSaveError ({});
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
        setSessionNote (ko ("세션 열기 실패: ") + result.getErrorMessage(), true);
        return;
    }

    faultedPlugins.clear();
    stalledPlugins.clear();
    pluginNote.clear();
    setSessionNote ({}, false);   // the previous session's notes; the startup note stays until a device runs
    setSaveError ({});
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
        setSessionNote (ko ("세션을 열었지만 확인이 필요합니다: ") + warnings.joinIntoString ("\n"), true);
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
        setSaveError (ko ("저장 실패: ") + result.getErrorMessage());
        return false;
    }

    setSaveError ({});
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
        {
            file = file.withFileExtension (MixSession::fileExtension);

            // the chooser warned about the name the operator typed (e.g. "Show.txt"), not this one: never overwrite it unseen
            if (file.existsAsFile())
            {
                self.setSaveError (ko ("같은 이름의 세션 파일이 이미 있습니다 (확장자를 붙인 이름): ") + file.getFullPathName());

                if (then)
                    then (false);

                return;
            }
        }

        if (self.document.getSession().name.isEmpty() || self.document.getSession().name == ko ("새 세션"))
            self.document.setSessionName (file.getFileNameWithoutExtension());

        const auto result = self.document.save (file);

        if (result.failed())
        {
            self.setSaveError (ko ("저장 실패: ") + result.getErrorMessage());

            if (then)
                then (false);

            return;
        }

        self.setSaveError ({});
        self.settings.setLastSessionFile (file);
        self.settings.addRecentSession (file);
        self.showStatus (ko ("저장됨: ") + file.getFileName());

        if (then)
            then (true);
    });
}

bool MainComponent::openFromCommandLine (const juce::String& commandLine)
{
    const juce::ArgumentList args ("LiveMix", commandLine);

    for (const auto& arg : args.arguments)
    {
        const auto file = arg.resolveAsFile();

        if (! file.hasFileExtension (MixSession::fileExtension))
            continue;

        // a missing file is still the named session: its "파일이 없습니다" notice shows instead of a silent fallback
        if (juce::Component::getCurrentlyModalComponent() != nullptr)
            pendingCommandLineFile = file;   // a question (rename, save?) is open: answered first, then the file (timer)
        else
            openSession (file);

        return true;
    }

    return false;
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { ko ("세션"), ko ("온라인 백업"), ko ("설정"), ko ("도움말") };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;
    auto withShortcut = [] (int id, const juce::String& text, const juce::String& shortcut)
    {
        juce::PopupMenu::Item item (text);
        item.itemID = id;
        item.shortcutKeyDescription = shortcut;
        return item;
    };

    switch (topLevelMenuIndex)
    {
        case 0:
        {
            menu.addItem (withShortcut (1, ko ("새 세션"), "Ctrl+N"));
            menu.addItem (2, ko ("열기..."));
            menu.addItem (withShortcut (3, ko ("저장"), "Ctrl+S"));
            menu.addItem (withShortcut (4, ko ("다른 이름으로 저장..."), "Ctrl+Shift+S"));
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

            menu.addSeparator();
            menu.addItem (9, ko ("종료"));
            break;
        }

        case 1:
            menu.addItem (1, ko ("온라인 백업 창 열기..."));
            break;

        case 2:
            menu.addItem (1, ko ("설정..."));
            menu.addItem (2, ko ("플러그인 관리..."));
            menu.addItem (3, ko ("LUFS 미터..."));
            break;

        case 3:
            menu.addItem (1, ko ("커뮤니티 (카카오톡 오픈채팅)"));
            menu.addItem (2, ko ("업데이트 확인..."), Updater::isAvailable());
            menu.addSeparator();
            menu.addItem (3, ko ("LiveMix 정보"));
            break;

        default:
            break;
    }

    return menu;
}

void MainComponent::menuItemSelected (int id, int topLevelMenuIndex)
{
    switch (topLevelMenuIndex)
    {
        case 0:
            switch (id)
            {
                case 1: newSession(); break;
                case 2: openSessionDialog(); break;
                case 3: saveSession(); break;
                case 4: saveSessionAs(); break;
                case 5: renameSessionDialog(); break;
                case 9:
                    if (auto* app = juce::JUCEApplication::getInstance())
                        app->systemRequestedQuit();
                    break;
                default:
                    if (id >= 100)
                    {
                        const auto recent = settings.getRecentSessions();

                        if (id - 100 < recent.size())
                            openSession (juce::File (recent[id - 100]));
                    }
                    break;
            }
            break;

        case 1:
            if (id == 1)
                showBackupDialog();
            break;

        case 2:
            if (id == 1)
                showSettingsDialog();
            else if (id == 2)
                showPluginManager();
            else if (id == 3)
                showLoudnessWindow();
            break;

        case 3:
            if (id == 1)
                juce::URL (Links::feedbackChat).launchInDefaultBrowser();
            else if (id == 2)
                Updater::checkForUpdatesWithUI();
            else if (id == 3)
                showAbout();
            break;

        default:
            break;
    }
}

void MainComponent::renameSessionDialog()
{
    auto* alert = new juce::AlertWindow (ko ("세션 이름"), ko ("이 세션의 이름 (창 제목과 백업 파일에 씁니다)"), juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor ("name", document.getSession().name, ko ("이름"));
    alert->addButton (ko ("확인"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safeThis (this);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert] (int r)
    {
        if (safeThis != nullptr && r == 1)
            safeThis->document.setSessionName (alert->getTextEditorContents ("name"));
    }), true);
    focusAlertTextEditor (*alert, "name");
}

void MainComponent::showAbout()
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "LiveMix " + juce::JUCEApplication::getInstance()->getApplicationVersion(),
                                            ko ("방송용 라이브 마이크 VST3 · VST2 호스트\n곰튀김\n\n") + "VST is a trademark of Steinberg Media Technologies GmbH.", ko ("확인"));
}

void MainComponent::showBackupDialog()
{
    juce::Component::SafePointer<MainComponent> safeThis (this);
    BackupDialog::Callbacks callbacks;
    callbacks.presetRestored = [safeThis]
    {
        if (safeThis != nullptr && safeThis->pluginManagerWindow != nullptr)
            safeThis->pluginManagerWindow->refreshPresets();
    };
    callbacks.status = [safeThis] (const juce::String& message, bool error)
    {
        if (safeThis != nullptr)
            safeThis->showStatus (message, error);
    };
    callbacks.saveBeforeUpload = [safeThis]() -> bool
    {
        if (safeThis == nullptr)
            return false;

        auto& self = *safeThis;

        if (! self.document.hasFile())
        {
            self.showStatus (ko ("먼저 세션을 저장하세요 (세션 > 저장)"), true);
            return false;
        }

        self.document.pollPluginEdits();

        if (! self.saveIfDirty())
        {
            self.showStatus (ko ("세션을 저장하지 못해 백업을 시작하지 않았습니다"), true);   // an upload of the stale file would pass for a backup
            return false;
        }

        return true;
    };
    callbacks.restore = [safeThis] (const juce::File& file)
    {
        if (safeThis != nullptr)
            safeThis->openSession (file);
    };
    BackupDialog::show (document, settings, backup, this, std::move (callbacks));
}

void MainComponent::registerHotkeys()
{
    const auto apply = [this] (int id, const juce::String& description, const juce::String& what)
    {
        if (description.isEmpty())
        {
            hotkeys.clear (id);
            return;
        }

        juce::String error;

        if (! hotkeys.set (id, juce::KeyPress::createFromDescription (description), error))
            showStatus (what + ko (" 핫키(") + description + ko (") 등록 실패: ") + error, true);
    };

    apply (1, settings.getMicMuteHotkey(), ko ("마이크 뮤트그룹"));
    apply (2, settings.getFxMuteHotkey(), ko ("FX 뮤트그룹"));
    apply (3, settings.getWindowHotkey(), ko ("창 숨기기/불러오기"));
}

void MainComponent::layoutFxDrawer()
{
    // the drawer is as tall as its content (a long chain, many mics): the viewport scrolls it
    const int width = fxDrawerViewport.getWidth();
    const int bar = fxDrawerViewport.getScrollBarThickness();
    const bool scrolls = fxDrawer.getPreferredHeight (width - bar) > fxDrawerViewport.getHeight();
    const int contentWidth = juce::jmax (1, width - (scrolls ? bar : 0));
    fxDrawer.setSize (contentWidth, juce::jmax (fxDrawerViewport.getHeight(), fxDrawer.getPreferredHeight (contentWidth)));
}

void MainComponent::muteGroupsChanged()
{
    const bool mic = muteGroups.isMuted (MuteGroups::Group::mic);
    const bool fx = muteGroups.isMuted (MuteGroups::Group::fx);
    topBar.setMuteGroups (mic, fx);

    for (auto& card : cards)
        card->setGroupMuted (mic);

    fxDrawer.setGroupMuted (fx);
    showStatus (mic && fx ? ko ("마이크·FX 뮤트그룹 뮤트 중") : mic ? ko ("마이크 뮤트그룹 뮤트 중") : fx ? ko ("FX 뮤트그룹 뮤트 중") : ko ("뮤트그룹 해제"));
}

void MainComponent::showSettingsDialog()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    SettingsDialog::show (engine, settings, this, [safe] { if (safe != nullptr) safe->deviceChosen(); },
                          [safe] { if (safe != nullptr) safe->registerHotkeys(); },
                          [safe] (bool capturing)
                          {
                              if (safe == nullptr) return;
                              // the key being chosen must not fire its current action
                              if (capturing)
                              {
                                  safe->hotkeys.clear (1);
                                  safe->hotkeys.clear (2);
                                  safe->hotkeys.clear (3);
                              }
                              else
                                  safe->registerHotkeys();
                          },
                          [safe] { return safe != nullptr ? safe->getExternalControlStatus() : ControlServer::Status {}; },
                          [safe] (bool on)
                          {
                              if (safe != nullptr && safe->onExternalControlEnabled)
                                  safe->onExternalControlEnabled (on);
                          });
}

} // namespace gocue::livemix
