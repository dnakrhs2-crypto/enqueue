#include "ChannelCard.h"

namespace gocue::livemix
{

/** One "이펙터" row: FX badge + name, the send fader, the value, the 프리/포스트 chip. */
struct ChannelCard::SendRow : public juce::Component
{
    SendRow (ChannelCard& o, const juce::Uuid& fx) : owner (o), fxId (fx)
    {
        badge.setFont (juce::Font (juce::FontOptions (pt (11.0f), juce::Font::bold)));
        badge.setColour (juce::Label::textColourId, juce::Colours::white);
        badge.setColour (juce::Label::backgroundColourId, Palette::accent);
        badge.setJustificationType (juce::Justification::centred);
        badge.setTooltip (ko ("더블클릭: 이 FX 채널의 이름 바꾸기"));
        badge.onDoubleClick = [this] { fxName.showEditor(); };
        addAndMakeVisible (badge);
        fxName.setNameFont (juce::Font (juce::FontOptions (pt (13.0f), juce::Font::bold)));
        fxName.setMinimumHorizontalScale (1.0f);
        fxName.onRenamed = [this] (const juce::String& text) { owner.document.renameFx (fxId, text); };   // every card, tab and the session follow
        fxName.onEditorShown = [this] { editingName = true; resized(); };    // the name gets room to type in
        fxName.onEditorHidden = [this] { editingName = false; resized(); };
        addAndMakeVisible (fxName);

        fader.setSliderStyle (juce::Slider::LinearHorizontal);
        fader.setRange (0.0, 100.0, 1.0);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        fader.setWantsKeyboardFocus (false);
        fader.setScrollWheelEnabled (false);   // the wheel over it scrolls the cards; a live send must not turn by accident
        fader.onValueChange = [this]
        {
            if (! owner.refreshing)
                owner.document.setSend (owner.channelId, fxId, fader.getValue() / 100.0, preToggle.getToggleState());

            value.setText (juce::String ((int) std::lround (fader.getValue())) + "%", juce::dontSendNotification);
        };
        addAndMakeVisible (fader);

        value.setFont (juce::Font (juce::FontOptions (pt (20.0f), juce::Font::bold)));
        value.setJustificationType (juce::Justification::centredRight);
        value.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (value);

        preToggle.setClickingTogglesState (true);
        preToggle.setWantsKeyboardFocus (false);
        preToggle.setTooltip (ko ("프리 = VST3 체인 전 원음을 보냄, 포스트 = 체인을 거친 소리를 보냄"));
        preToggle.onClick = [this]
        {
            preToggle.setButtonText (preToggle.getToggleState() ? ko ("프리") : ko ("포스트"));

            if (! owner.refreshing)
                owner.document.setSend (owner.channelId, fxId, fader.getValue() / 100.0, preToggle.getToggleState());
        };
        addAndMakeVisible (preToggle);
    }

    void refresh (const MixFx& fx, int index, const MixSend& send)
    {
        badge.setText ("FX" + juce::String (index + 1), juce::dontSendNotification);

        if (! fxName.isBeingEdited())
            fxName.setText (fx.name.trim() == "FX " + juce::String (index + 1) ? juce::String() : fx.name, juce::dontSendNotification);   // the default name repeats the badge

        fxName.setTooltip (fx.name + "   " + ko ("(더블클릭: 이름 바꾸기)"));
        fader.setValue (send.amount * 100.0, juce::dontSendNotification);
        value.setText (juce::String ((int) std::lround (send.amount * 100.0)) + "%", juce::dontSendNotification);
        preToggle.setToggleState (send.pre, juce::dontSendNotification);
        preToggle.setButtonText (send.pre ? ko ("프리") : ko ("포스트"));
        resized();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        badge.setBounds (r.removeFromLeft (34).reduced (0, 6));
        r.removeFromLeft (6);
        const int nameWidth = editingName ? juce::jmax (120, r.getWidth() / 2)
                                          : juce::jlimit (56, 140, labelWidthForText (fxName, fxName.getText()));
        preToggle.setBounds (r.removeFromRight (58).reduced (0, 5));
        r.removeFromRight (8);
        value.setBounds (r.removeFromRight (labelWidthForText (value, "100%")));
        r.removeFromRight (6);
        // Even a 300 px FX column keeps a 100 px fader; the name gives up room first - except while its editor is
        // open, when typing needs the room and the fader may shrink for that moment (review finding, 0.5.4).
        const int faderReserve = editingName ? 0 : 100;
        fxName.setBounds (r.removeFromLeft (juce::jmin (nameWidth, juce::jmax (0, r.getWidth() - 6 - faderReserve))));
        r.removeFromLeft (6);
        fader.setBounds (r);
    }

    ChannelCard& owner;
    juce::Uuid fxId;
    DoubleClickLabel badge;
    NameLabel fxName;
    juce::Label value;
    juce::Slider fader;
    Chip preToggle { ko ("포스트") };
    bool editingName = false;
};

/** A numbered chip in the chain summary: click opens the plugin's window. */
struct ChannelCard::ChainChip : public juce::TextButton
{
    ChainChip (int idx, const juce::String& text, bool bypassed) : juce::TextButton (text), index (idx), off (bypassed)
    {
        setWantsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (isMouseOverButton ? Palette::slotBg.brighter (0.08f) : Palette::slotBg);
        g.fillRoundedRectangle (bounds, Palette::controlRadius);
        g.setColour (Palette::slotLine);
        g.drawRoundedRectangle (bounds, Palette::controlRadius, 1.0f);

        auto idx = getLocalBounds().reduced (6, 0).removeFromLeft (20).toFloat().withSizeKeepingCentre (20.0f, 20.0f);
        g.setColour (off ? Palette::lampOff : Palette::accent);
        g.fillRoundedRectangle (idx, 6.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (pt (12.0f), juce::Font::bold)));
        g.drawText (juce::String (index + 1), idx.toNearestInt(), juce::Justification::centred, false);

        g.setColour (off ? Palette::dimText : Palette::text);
        g.setFont (juce::Font (juce::FontOptions (pt (13.5f), juce::Font::bold)));
        g.drawText (getButtonText(), getLocalBounds().withTrimmedLeft (32).withTrimmedRight (8), juce::Justification::centredLeft, true);
    }

    int index;
    bool off;
};

//==============================================================================
ChannelCard::~ChannelCard() = default;

ChannelCard::ChannelCard (MixDocument& doc, const juce::Uuid& id) : document (doc), channelId (id)
{
    masterChip.setButtonText (ko ("마스터"));
    directChip.setButtonText (ko ("직접 출력"));
    number.setFont (juce::Font (juce::FontOptions (pt (14.0f), juce::Font::bold)));
    number.setJustificationType (juce::Justification::centred);
    number.setColour (juce::Label::backgroundColourId, Palette::card2);
    number.setColour (juce::Label::outlineColourId, Palette::line);
    addAndMakeVisible (number);

    name.onRenamed = [this] (const juce::String& text) { document.renameChannel (channelId, text); };
    addAndMakeVisible (name);

    menuButton.setWantsKeyboardFocus (false);
    menuButton.setTooltip (ko ("채널 메뉴"));
    menuButton.onClick = [this] { showMenu(); };
    addAndMakeVisible (menuButton);

    micButton.onClick = [this] { document.setChannelOn (channelId, ! micButton.isOn()); };
    addAndMakeVisible (micButton);

    styleCaption (inputCaption, ko ("입력"));
    addAndMakeVisible (inputCaption);
    inputCombo.setWantsKeyboardFocus (false);
    inputCombo.onChange = [this] { commitInput(); };
    addAndMakeVisible (inputCombo);
    stereoToggle.setButtonText (ko ("스테레오"));
    stereoToggle.setWantsKeyboardFocus (false);
    stereoToggle.setTooltip (ko ("켜면 입력 두 개(왼쪽·오른쪽)를 한 채널로 씁니다"));
    stereoToggle.onClick = [this] { commitInput(); };
    addAndMakeVisible (stereoToggle);

    styleCaption (chainCaption, ko ("VST3 체인") + "   " + ko ("순서대로 통과"));
    addAndMakeVisible (chainCaption);
    chainArrows.setVisible (false);
    openChainButton.setButtonText (ko ("체인 열기"));
    openChainButton.setWantsKeyboardFocus (false);
    openChainButton.onClick = [this] { if (onOpenChain) onOpenChain (channelId); };
    addAndMakeVisible (openChainButton);
    addPluginButton.setButtonText (ko ("+ 추가"));
    addPluginButton.setWantsKeyboardFocus (false);
    addPluginButton.onClick = [this] { if (onAddPlugin) onAddPlugin (channelId); };
    addAndMakeVisible (addPluginButton);
    pluginGroupsButton.setButtonText (ko ("플러그인 그룹"));
    pluginGroupsButton.setTooltip (ko ("이 채널의 플러그인 그룹: 체인의 플러그인 중 골라 그룹으로 묶어 두면, 아래 번호 한 번으로 그것들만 끄고 켭니다 (최대 5개)"));
    pluginGroupsButton.setWantsKeyboardFocus (false);
    pluginGroupsButton.onClick = [this] { if (onOpenPluginGroups) onOpenPluginGroups (channelId); };
    addAndMakeVisible (pluginGroupsButton);

    styleCaption (groupsCaption, ko ("그룹 OFF"));
    addAndMakeVisible (groupsCaption);

    for (int i = 0; i < MixSession::maxPluginGroups; ++i)
    {
        auto b = std::make_unique<juce::TextButton> (juce::String (i + 1));
        b->setWantsKeyboardFocus (false);
        b->setColour (juce::TextButton::buttonOnColourId, Palette::danger);
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        b->onClick = [this, i]
        {
            if (const auto* c = channel())
                if (i < (int) c->pluginGroups.size())
                    document.setPluginGroupOff (channelId, i, ! c->pluginGroups[(size_t) i].off);
        };
        addAndMakeVisible (*b);
        groupButtons[(size_t) i] = std::move (b);
    }

    styleCaption (fxCaption, ko ("이펙터") + "   " + ko ("FX 채널로 보내는 양"));
    addAndMakeVisible (fxCaption);

    styleCaption (outputCaption, ko ("출력"));
    addAndMakeVisible (outputCaption);
    masterChip.setTooltip (ko ("마스터를 거쳐 메인 출력으로"));
    masterChip.onClick = [this] { commitOutput(); };
    addAndMakeVisible (masterChip);
    directChip.setTooltip (ko ("장치 출력 쌍으로 직접 (OBS 등에서 따로 받을 때)"));
    directChip.onClick = [this] { commitOutput(); };
    addAndMakeVisible (directChip);
    directCombo.setWantsKeyboardFocus (false);
    directCombo.onChange = [this] { commitOutput (true); };
    addAndMakeVisible (directCombo);
    muteGroupChip.setTooltip (ko ("켜 두면 마이크 뮤트그룹 핫키가 이 마이크를 함께 뮤트합니다 (핫키는 설정에서)"));
    muteGroupChip.onClick = [this]
    {
        if (! refreshing)
            document.setChannelMuteGroup (channelId, muteGroupChip.getToggleState());
    };
    addAndMakeVisible (muteGroupChip);

    styleCaption (panCaption, ko ("팬"));
    addAndMakeVisible (panCaption);
    panSlider.onValueChange = [this]
    {
        panValue.setText (panSlider.getTextFromValue (panSlider.getValue()), juce::dontSendNotification);

        if (! refreshing)
            document.setChannelPan (channelId, panSlider.getValue());
    };
    addAndMakeVisible (panSlider);
    panValue.setFont (juce::Font (juce::FontOptions (pt (20.0f), juce::Font::bold)));
    panValue.setJustificationType (juce::Justification::centredRight);
    panValue.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (panValue);

    styleCaption (meterCaption, ko ("입력 미터"));
    addAndMakeVisible (meterCaption);
    addAndMakeVisible (meter_);

    refresh();
}

const MixChannel* ChannelCard::channel() const
{
    return document.getSession().findChannel (channelId);
}

void ChannelCard::setDeviceChannels (const juce::StringArray& ins, const juce::StringArray& outs)
{
    inputNames = ins;
    outputNames = outs;
    refresh();
}

void ChannelCard::refresh()
{
    const auto* c = channel();

    if (c == nullptr)
        return;

    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto& session = document.getSession();
    int index = 0;

    for (size_t i = 0; i < session.channels.size(); ++i)
        if (session.channels[i].id == channelId)
            index = (int) i;

    number.setText (juce::String (index + 1), juce::dontSendNotification);

    if (! name.isBeingEdited())
        name.setText (c->name, juce::dontSendNotification);

    name.setTooltip (c->name + "\n" + ko ("더블클릭해서 이름 바꾸기"));
    micButton.setOn (c->on);

    fillChannelCombo (inputCombo, inputNames, c->stereo, MixSession::maxDeviceChannels);
    selectSavedChannel (inputCombo, inputNames, c->inputFirst, c->stereo);
    stereoToggle.setToggleState (c->stereo, juce::dontSendNotification);

    masterChip.setToggleState (c->output.master, juce::dontSendNotification);
    directChip.setToggleState (c->output.direct, juce::dontSendNotification);
    const auto runningDevice = document.getEngine().getOpenDevice();
    const auto device = runningDevice.input.isNotEmpty() ? runningDevice : session.device;
    fillDirectOutputCombo (directCombo, directChip, outputNames, device.isAsio(), device.output.isNotEmpty(),
                           c->output.directFirst, c->output.direct);
    muteGroupChip.setToggleState (c->muteGroup, juce::dontSendNotification);
    micButton.setMuted (groupMuted && c->muteGroup);
    meter_.setStereo (c->stereo);
    panSlider.setValue (c->pan, juce::dontSendNotification);
    panValue.setText (panSlider.getTextFromValue (c->pan), juce::dontSendNotification);
    panSlider.setTooltip (ko ("팬: 왼쪽/오른쪽 배치. 더블클릭하면 가운데")
                          + (c->stereo ? ko (". 스테레오 채널은 좌우 균형") : juce::String()));

    for (int i = 0; i < MixSession::maxPluginGroups; ++i)
    {
        auto& b = *groupButtons[(size_t) i];
        const bool exists = i < (int) c->pluginGroups.size();
        const bool off = exists && c->pluginGroups[(size_t) i].off;
        b.setEnabled (exists);
        b.setToggleState (off, juce::dontSendNotification);
        b.setTooltip (exists ? ko ("플러그인 그룹 ") + juce::String (i + 1) + " (" + juce::String ((int) c->pluginGroups[(size_t) i].slots.size()) + ko ("개)")
                                   + (off ? ko (": 꺼져 있음 - 누르면 켭니다") : ko (": 누르면 이 그룹의 플러그인을 끕니다"))
                             : ko ("플러그인 그룹 ") + juce::String (i + 1) + ko ("은 아직 없습니다 ('플러그인 그룹'에서 만듭니다)"));
    }

    rebuildChain();
    rebuildSends();
    for (auto* child : getChildren())
        child->setAlpha (c->on || child == &micButton ? 1.0f : 0.62f);
    resized();
    repaint();
}

void ChannelCard::setGroupMuted (bool muted)
{
    groupMuted = muted;

    if (const auto* c = channel())
        micButton.setMuted (groupMuted && c->muteGroup);
}

void ChannelCard::rebuildChain()
{
    chips.clear();
    auto* chain = document.getEngine().getChannelChain (channelId);

    if (chain == nullptr)
        return;

    for (int i = 0; i < chain->getNumSlots(); ++i)
    {
        const auto& slot = chain->getSlot (i);
        const auto text = slot.plugin != nullptr ? slot.plugin->getName() : slot.state.name + ko (" (없음)");
        auto chip = std::make_unique<ChainChip> (i, text, slot.bypassed.load() || slot.plugin == nullptr);
        chip->setTooltip (text);   // a narrow card cuts the name short
        chip->onClick = [this, i] { if (onOpenPluginEditor) onOpenPluginEditor (channelId, i); };
        addAndMakeVisible (*chip);
        chips.push_back (std::move (chip));
    }
}

void ChannelCard::rebuildSends()
{
    const auto* c = channel();
    const auto& session = document.getSession();

    if (c == nullptr)
        return;

    // one row per FX channel, in FX order (rows are reused by FX id)
    std::vector<std::unique_ptr<SendRow>> next;

    for (size_t f = 0; f < session.fx.size(); ++f)
    {
        const auto& fx = session.fx[f];
        std::unique_ptr<SendRow> row;

        for (auto& existing : sends)
            if (existing != nullptr && existing->fxId == fx.id)
                row = std::move (existing);

        if (row == nullptr)
        {
            row = std::make_unique<SendRow> (*this, fx.id);
            addAndMakeVisible (*row);
        }

        MixSend send;

        for (const auto& s : c->sends)
            if (s.fx == fx.id)
                send = s;

        row->refresh (fx, (int) f, send);
        next.push_back (std::move (row));
    }

    sends = std::move (next);
}

void ChannelCard::commitInput()
{
    if (refreshing)
        return;

    const auto* c = channel();
    const int sel = inputCombo.getSelectedId();
    // no matching item (a mono input mid-toggle, a route the device no longer has): keep the channel's input rather than fall to input 1
    const int first = sel > 0 ? sel - 1 : (c != nullptr ? c->inputFirst : 0);
    document.setChannelInput (channelId, first, stereoToggle.getToggleState());
}

void ChannelCard::commitOutput (bool pairSelected)
{
    if (refreshing)
        return;

    const auto* c = channel();
    if (c == nullptr)
        return;

    const int sel = directCombo.getSelectedId();
    auto output = c->output;
    output.master = masterChip.getToggleState();
    output.direct = directChip.getToggleState();
    // The effective Windows pair is display-only. Toggle clicks always retain the saved ASIO pair. A pair the operator
    // picks on a running ASIO device is stored - even the device's only pair (a 2-output interface whose session asked
    // for 3-4 shows 없음 until 1-2 is chosen).
    auto& engine = document.getEngine();
    if (pairSelected && sel > 0 && directCombo.isEnabled() && engine.isDeviceRunning() && engine.getOpenDevice().isAsio()
        && sel + 1 <= engine.getNumDeviceOutputs())   // the pair must exist (an output-less ASIO device lists placeholders)
        output.directFirst = sel - 1;
    document.setChannelOutput (channelId, output);
}

void ChannelCard::showMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, ko ("이름 바꾸기"));
    menu.addItem (2, ko ("체인 열기"));
    menu.addSeparator();
    menu.addItem (3, ko ("이 채널 삭제"), document.getSession().channels.size() > 1);

    juce::Component::SafePointer<ChannelCard> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton), [safeThis] (int result)
    {
        if (safeThis == nullptr)
            return;

        if (result == 1)
            safeThis->name.showEditor();
        else if (result == 2 && safeThis->onOpenChain)
            safeThis->onOpenChain (safeThis->channelId);
        else if (result == 3 && safeThis->onRemove)
            safeThis->onRemove (safeThis->channelId);
    });
}

void ChannelCard::setLayout (CardLayout newLayout)
{
    if (layout != newLayout)
    {
        layout = newLayout;
        resized();
    }
}

int ChannelCard::chainWidthForWidth (int width) const
{
    // The same column width drives wrapping and placement.
    const int inner = width - 28;
    int chainW = inner;

    if (layout == CardLayout::wide)
        chainW = 361;
    else if (layout == CardLayout::medium)
        chainW = inner - 194 - 18 - 160 - 18;

    return juce::jmax (1, chainW);
}

int ChannelCard::chainHeightForWidth (int width) const
{
    const int chainW = chainWidthForWidth (width);
    const int rows = ChipFlow::layout (chips, juce::Rectangle<int> (0, 0, chainW, 1), 44, false);
    return 27 + rows * ChipFlow::rowStep + chainFooter + (chainW < groupRowWidth ? 36 : 0);
}

int ChannelCard::getPreferredHeight (int width) const
{
    const int sendRows = juce::jmax (1, (int) sends.size());
    const int fxH = 28 + sendRows * 38 - 8;
    const int chainH = chainHeightForWidth (width);
    const int outH = 26 + 34 + 4 + 34;

    switch (layout)
    {
        case CardLayout::wide:   return juce::jmax (132, juce::jmax (chainH, fxH)) + 24;
        case CardLayout::medium: return juce::jmax (132, chainH) + 16 + juce::jmax (fxH, outH) + 26;
        case CardLayout::narrow: return 114 + 96 + chainH + fxH + outH + 4 * 14 + 26;
    }

    return 200;
}

void ChannelCard::resized()
{
    auto area = getLocalBounds().reduced (14, 12);

    auto layoutHead = [this] (juce::Rectangle<int> r)
    {
        auto row = r.removeFromTop (34);
        number.setBounds (row.removeFromLeft (32).reduced (0, 2));
        row.removeFromLeft (10);
        menuButton.setBounds (row.removeFromRight (34).reduced (0, 3));
        row.removeFromRight (6);
        name.setBounds (row);
        r.removeFromTop (8);
        micButton.setBounds (r.removeFromTop (40));
        r.removeFromTop (8);
        muteGroupChip.setBounds (r.removeFromTop (24).withWidth (100));
    };

    auto layoutInput = [this] (juce::Rectangle<int> r, bool stacked)
    {
        if (stacked)
        {
            auto row = r.removeFromTop (30);
            inputCaption.setBounds (row.removeFromLeft (34));
            stereoToggle.setBounds (row.removeFromRight (88));
            row.removeFromRight (4);
            inputCombo.setBounds (row);
            r.removeFromTop (8);
        }
        else
        {
            auto row = r.removeFromTop (26);
            inputCaption.setBounds (row.removeFromLeft (34));
            stereoToggle.setBounds (row.removeFromRight (88).expanded (0, 2));
            r.removeFromTop (2);
            inputCombo.setBounds (r.removeFromTop (30));
            r.removeFromTop (16);
        }
        meterCaption.setBounds (r.removeFromTop (18));
        meter_.setBounds (r.removeFromTop (40));
    };

    auto layoutChain = [this] (juce::Rectangle<int> r)
    {
        chainCaption.setBounds (r.removeFromTop (26));
        r.removeFromTop (1);
        const int rows = ChipFlow::layout (chips, r, 44, true);
        r.removeFromTop (rows * ChipFlow::rowStep + 1);
        auto buttons = r.removeFromTop (30);
        openChainButton.setBounds (buttons.removeFromLeft (92));
        buttons.removeFromLeft (8);
        addPluginButton.setBounds (buttons.removeFromLeft (76));
        r.removeFromTop (6);
        auto groupsRow = r.removeFromTop (30);
        pluginGroupsButton.setBounds (groupsRow.removeFromLeft (112));
        groupsRow.removeFromLeft (8);
        if (r.getWidth() < groupRowWidth)
        {
            r.removeFromTop (6);
            groupsRow = r.removeFromTop (30);
        }
        groupsCaption.setBounds (groupsRow.removeFromLeft (66).reduced (0, 2));

        for (auto& b : groupButtons)
        {
            b->setBounds (groupsRow.removeFromLeft (30).reduced (0, 3));
            groupsRow.removeFromLeft (4);
        }
    };

    auto layoutFx = [this] (juce::Rectangle<int> r)
    {
        fxCaption.setBounds (r.removeFromTop (26));
        r.removeFromTop (2);

        for (auto& row : sends)
        {
            row->setBounds (r.removeFromTop (30));
            r.removeFromTop (8);
        }
    };

    auto layoutOut = [this] (juce::Rectangle<int> r, bool tight)
    {
        outputCaption.setBounds (r.removeFromTop (26));
        auto chipsRow = r.removeFromTop (34);
        // the wide column is 250 px: slightly narrower chips leave the pair box 88 px, so a named ASIO pair
        // ('31-32  <name>') still shows its whole number before the ellipsis
        masterChip.setBounds (chipsRow.removeFromLeft (tight ? 70 : 72));
        chipsRow.removeFromLeft (tight ? 6 : 8);
        directChip.setBounds (chipsRow.removeFromLeft (tight ? 80 : 84));
        chipsRow.removeFromLeft (6);
        directCombo.setBounds (chipsRow.withHeight (30).withY (chipsRow.getY() + 2));
        r.removeFromTop (4);
        auto panRow = r.removeFromTop (34);
        panCaption.setBounds (panRow.removeFromLeft (34));
        panValue.setBounds (panRow.removeFromRight (labelWidthForText (panValue, "R100")));
        panRow.removeFromRight (6);
        panSlider.setBounds (panRow);
    };

    const int chainH = chainHeightForWidth (getWidth());
    if (layout == CardLayout::wide)
    {
        auto head = area.removeFromLeft (194);
        area.removeFromLeft (18);
        auto input = area.removeFromLeft (160);
        area.removeFromLeft (18);
        auto chain = area.removeFromLeft (361);
        area.removeFromLeft (18);
        auto out = area.removeFromRight (250);   // the pair box keeps 80 px: '31-32' reads in full
        area.removeFromRight (18);
        layoutHead (head);
        layoutInput (input, false);
        layoutChain (chain.withHeight (chainH));
        layoutFx (area);
        layoutOut (out, true);
    }
    else if (layout == CardLayout::medium)
    {
        const int topH = juce::jmax (132, chainH);
        auto top = area.removeFromTop (topH);
        area.removeFromTop (16);
        auto head = top.removeFromLeft (194);
        top.removeFromLeft (18);
        auto input = top.removeFromLeft (160);
        top.removeFromLeft (18);
        layoutHead (head);
        layoutInput (input, false);
        layoutChain (top.withHeight (chainH));
        auto fx = area.removeFromLeft (194 + 18 + 160);
        area.removeFromLeft (18);
        layoutFx (fx);
        layoutOut (area, false);
    }
    else
    {
        layoutHead (area.removeFromTop (114));
        area.removeFromTop (14);
        layoutInput (area.removeFromTop (96), true);
        area.removeFromTop (14);
        layoutChain (area.removeFromTop (chainH));
        area.removeFromTop (14);
        layoutFx (area.removeFromTop (28 + juce::jmax (1, (int) sends.size()) * 38 - 8));
        area.removeFromTop (14);
        layoutOut (area, false);
    }
}

void ChannelCard::paint (juce::Graphics& g)
{
    const auto* c = channel();
    const bool off = c != nullptr && ! c->on;
    if (off) g.beginTransparencyLayer (0.62f);
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Palette::card);
    g.fillRoundedRectangle (bounds, Palette::cardRadius);
    g.setColour (Palette::line);
    g.drawRoundedRectangle (bounds, Palette::cardRadius, 1.0f);
    if (off) g.endTransparencyLayer();
}

} // namespace gocue::livemix
