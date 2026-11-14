#include "Localisation.h"
#include "AuxCard.h"

#include <algorithm>

namespace gocue::livemix
{

/** A mic channel that can send to this aux: lit (accent edge) while it does. The same switch as that mic card's
    AUX chip. */
struct AuxCard::SenderChip : public Chip
{
    explicit SenderChip (const juce::Uuid& id) : channelId (id) {}
    juce::Uuid channelId;
};

//==============================================================================
AuxCard::~AuxCard() = default;

AuxCard::AuxCard (MixDocument& doc, const juce::Uuid& id) : document (doc), auxId (id)
{
    masterChip.setButtonText (tr ("마스터"));
    directChip.setButtonText (tr ("직접 출력"));

    badge.setFont (juce::Font (juce::FontOptions (pt (12.5f), juce::Font::bold)));
    badge.setJustificationType (juce::Justification::centred);
    badge.setColour (juce::Label::textColourId, Palette::accent);
    badge.setColour (juce::Label::backgroundColourId, Palette::card2);
    badge.setColour (juce::Label::outlineColourId, Palette::accent);
    badge.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (badge);

    name.onRenamed = [this] (const juce::String& text) { document.renameAux (auxId, text); };
    name.setTwoLineFont (juce::Font (juce::FontOptions (pt (13.0f), juce::Font::bold)));
    addAndMakeVisible (name);

    menuButton.setWantsKeyboardFocus (false);
    menuButton.setTooltip (tr ("채널 메뉴"));
    menuButton.onClick = [this] { showMenu(); };
    addAndMakeVisible (menuButton);

    onButton.setTexts (tr ("억스 ON"), tr ("억스 OFF"), {});
    onButton.onClick = [this] { document.setAuxOn (auxId, ! onButton.isOn()); };
    addAndMakeVisible (onButton);

    styleCaption (meterCaption, tr ("미터"));   // a bus like the FX drawer's: it has no device input
    addAndMakeVisible (meterCaption);
    meter_.setStereo (true);   // the sum of its mics is a stereo bus
    addAndMakeVisible (meter_);

    styleCaption (chainCaption, tr ("VST3 체인   순서대로 통과"));
    addAndMakeVisible (chainCaption);
    openChainButton.setButtonText (tr ("체인 열기"));
    openChainButton.setWantsKeyboardFocus (false);
    openChainButton.onClick = [this] { if (onOpenChain) onOpenChain (auxId); };
    addAndMakeVisible (openChainButton);
    addPluginButton.setButtonText (tr ("+ 추가"));
    addPluginButton.setWantsKeyboardFocus (false);
    addPluginButton.onClick = [this] { if (onAddPlugin) onAddPlugin (auxId); };
    addAndMakeVisible (addPluginButton);

    styleCaption (sendersCaption, tr ("보내는 마이크 채널"));
    addAndMakeVisible (sendersCaption);

    styleCaption (outputCaption, tr ("출력"));
    addAndMakeVisible (outputCaption);
    masterChip.setTooltip (tr ("마스터를 거쳐 메인 출력으로"));
    masterChip.onClick = [this] { commitOutput(); };
    addAndMakeVisible (masterChip);
    directChip.setTooltip (tr ("장치 출력 쌍으로 직접 (OBS 등에서 따로 받을 때)"));
    directChip.onClick = [this] { commitOutput(); };
    addAndMakeVisible (directChip);
    directCombo.setWantsKeyboardFocus (false);
    directCombo.onChange = [this] { commitOutput (true); };
    addAndMakeVisible (directCombo);

    styleCaption (panCaption, tr ("팬"));
    addAndMakeVisible (panCaption);
    panSlider.setTooltip (trf ("팬: 왼쪽/오른쪽 배치. 더블클릭하면 가운데{0}", tr (". 스테레오 채널은 좌우 균형")));
    panSlider.onValueChange = [this]
    {
        panValue.setText (panSlider.getTextFromValue (panSlider.getValue()), juce::dontSendNotification);

        if (! refreshing)
            document.setAuxPan (auxId, panSlider.getValue());
    };
    addAndMakeVisible (panSlider);
    panValue.setFont (juce::Font (juce::FontOptions (pt (20.0f), juce::Font::bold)));
    panValue.setJustificationType (juce::Justification::centredRight);
    panValue.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (panValue);

    refresh();
}

const MixAux* AuxCard::aux() const
{
    return document.getSession().findAux (auxId);
}

void AuxCard::setDeviceChannels (const juce::StringArray& outs)
{
    outputNames = outs;
    refresh();
}

void AuxCard::refresh()
{
    const auto* a = aux();

    if (a == nullptr)
        return;

    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto& session = document.getSession();
    int index = 0;

    for (size_t i = 0; i < session.auxes.size(); ++i)
        if (session.auxes[i].id == auxId)
            index = (int) i;

    badge.setText ("AUX" + juce::String (index + 1), juce::dontSendNotification);

    if (! name.isBeingEdited())
        name.setText (a->name, juce::dontSendNotification);

    name.setTooltip (trf ("{0}\n더블클릭해서 이름 바꾸기", a->name));
    onButton.setOn (a->on);

    masterChip.setToggleState (a->output.master, juce::dontSendNotification);
    directChip.setToggleState (a->output.direct, juce::dontSendNotification);
    const auto runningDevice = document.getEngine().getOpenDevice();
    const auto device = runningDevice.input.isNotEmpty() ? runningDevice : session.device;
    fillDirectOutputCombo (directCombo, directChip, outputNames, device.isAsio(), device.output.isNotEmpty(),
                           a->output.directFirst, a->output.direct);
    panSlider.setValue (a->pan, juce::dontSendNotification);
    panValue.setText (panSlider.getTextFromValue (a->pan), juce::dontSendNotification);

    rebuildChain();
    rebuildSenders();

    for (auto* child : getChildren())
        child->setAlpha (a->on || child == &onButton ? 1.0f : 0.62f);

    for (auto& chip : senders)   // a mic that is OFF sends nothing for now: dimmed like that mic's card
        if (const auto* c = session.findChannel (chip->channelId); c != nullptr && ! c->on)
            chip->setAlpha (0.62f);

    resized();
    repaint();
}

void AuxCard::rebuildChain()
{
    chips.clear();
    auto* chain = document.getEngine().getAuxChain (auxId);

    if (chain == nullptr)
        return;

    for (int i = 0; i < chain->getNumSlots(); ++i)
    {
        const auto& slot = chain->getSlot (i);
        const auto text = slot.plugin != nullptr ? slot.plugin->getName() : trf ("{0} (없음)", slot.state.name);
        auto chip = std::make_unique<ChainChip> (i, text, slot.bypassed.load() || slot.plugin == nullptr);
        chip->setTooltip (text);
        chip->onClick = [this, i] { if (onOpenPluginEditor) onOpenPluginEditor (auxId, i); };
        addAndMakeVisible (*chip);
        chips.push_back (std::move (chip));
    }
}

void AuxCard::rebuildSenders()
{
    const auto& session = document.getSession();
    std::vector<std::unique_ptr<SenderChip>> next;   // reused by mic id: the chip clicked may be the one refreshed

    for (size_t i = 0; i < session.channels.size(); ++i)
    {
        const auto& c = session.channels[i];
        std::unique_ptr<SenderChip> chip;

        for (auto& existing : senders)
            if (existing != nullptr && existing->channelId == c.id)
                chip = std::move (existing);

        if (chip == nullptr)
        {
            chip = std::make_unique<SenderChip> (c.id);
            chip->onClick = [this, raw = chip.get()]
            {
                if (! refreshing)   // the chip has already flipped: lit = that mic sends here
                    document.setChannelAuxSend (raw->channelId, auxId, raw->getToggleState());
            };
            addAndMakeVisible (*chip);
        }

        const auto text = juce::String ((int) i + 1) + "  " + c.name;   // the mic card's number first, as on its card
        chip->setButtonText (text);
        chip->setTooltip (text);   // a long name is cut short on the chip
        chip->setToggleState (std::find (c.auxSends.begin(), c.auxSends.end(), auxId) != c.auxSends.end(),
                              juce::dontSendNotification);
        next.push_back (std::move (chip));
    }

    senders = std::move (next);
}

void AuxCard::commitOutput (bool pairSelected)
{
    if (refreshing)
        return;

    const auto* a = aux();
    if (a == nullptr)
        return;

    const int sel = directCombo.getSelectedId();
    auto output = a->output;
    output.master = masterChip.getToggleState();
    output.direct = directChip.getToggleState();
    // as a mic card: toggle clicks keep the saved ASIO pair; a pair picked on a running ASIO device is stored
    auto& engine = document.getEngine();
    if (pairSelected && sel > 0 && directCombo.isEnabled() && engine.isDeviceRunning() && engine.getOpenDevice().isAsio()
        && sel + 1 <= engine.getNumDeviceOutputs())
        output.directFirst = sel - 1;
    document.setAuxOutput (auxId, output);
}

void AuxCard::showMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, tr ("이름 바꾸기"));
    menu.addItem (2, tr ("체인 열기"));
    menu.addSeparator();
    menu.addItem (3, tr ("이 억스 채널 삭제"));

    juce::Component::SafePointer<AuxCard> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton), [safeThis] (int result)
    {
        if (safeThis == nullptr)
            return;

        if (result == 1)
            safeThis->name.showEditor();
        else if (result == 2 && safeThis->onOpenChain)
            safeThis->onOpenChain (safeThis->auxId);
        else if (result == 3 && safeThis->onRemove)
            safeThis->onRemove (safeThis->auxId);
    });
}

void AuxCard::setLayout (CardLayout newLayout)
{
    if (layout != newLayout)
    {
        layout = newLayout;
        resized();
    }
}

int AuxCard::chainWidthForWidth (int width) const
{
    // the mic cards' columns: name 194, input 160, chain 361 in the wide layout
    const int inner = width - 28;
    int chainW = inner;

    if (layout == CardLayout::wide)
        chainW = 361;
    else if (layout == CardLayout::medium)
        chainW = inner - 194 - 18 - 160 - 18;

    return juce::jmax (1, chainW);
}

int AuxCard::sendersWidthForWidth (int width) const
{
    // the mic cards' FX column: between the chain and the 250 px output (wide), or left of the output under the
    // name and meter (medium)
    const int inner = width - 28;

    if (layout == CardLayout::wide)
        return juce::jmax (1, inner - 194 - 18 - 160 - 18 - 361 - 18 - 250 - 18);

    if (layout == CardLayout::medium)
        return 194 + 18 + 160;

    return juce::jmax (1, inner);
}

int AuxCard::chainHeightForWidth (int width) const
{
    const int rows = ChipFlow::layout (chips, juce::Rectangle<int> (0, 0, chainWidthForWidth (width), 1), 44, false);
    return 27 + rows * ChipFlow::rowStep + chainFooter;
}

int AuxCard::sendersHeightForWidth (int width) const
{
    const int rows = ChipFlow::layout (senders, juce::Rectangle<int> (0, 0, sendersWidthForWidth (width), 1), 28, false);
    return 28 + rows * ChipFlow::rowStep - ChipFlow::gap;
}

int AuxCard::getPreferredHeight (int width) const
{
    const int chainH = chainHeightForWidth (width);
    const int sendersH = sendersHeightForWidth (width);

    switch (layout)
    {
        case CardLayout::wide:   return juce::jmax (132, juce::jmax (chainH, juce::jmax (sendersH, outputHeight))) + 24;
        case CardLayout::medium: return juce::jmax (132, chainH) + 16 + juce::jmax (sendersH, outputHeight) + 24;
        case CardLayout::narrow: return 114 + 14 + meterHeight + 14 + chainH + 14 + sendersH + 14 + outputHeight + 24;
    }

    return 200;
}

void AuxCard::resized()
{
    auto area = getLocalBounds().reduced (14, 12);

    auto layoutHead = [this] (juce::Rectangle<int> r)
    {
        auto row = r.removeFromTop (34);
        badge.setBounds (row.removeFromLeft (juce::jmax (52, labelWidthForText (badge, "AUX4"))).reduced (0, 2));
        row.removeFromLeft (10);
        name.setBounds (row);
        r.removeFromTop (8);
        onButton.setBounds (r.removeFromTop (40));
        r.removeFromTop (8);
        menuButton.setBounds (r.removeFromTop (24).removeFromRight (34));   // where a mic's menu is
    };

    auto layoutMeter = [this] (juce::Rectangle<int> r)
    {
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
    };

    auto layoutSenders = [this] (juce::Rectangle<int> r)
    {
        sendersCaption.setBounds (r.removeFromTop (26));
        r.removeFromTop (2);
        ChipFlow::layout (senders, r, 28, true);
    };

    auto layoutOut = [this] (juce::Rectangle<int> r, bool tight)
    {
        // the mic card's output column, without its AUX chips: an aux feeds no other aux
        outputCaption.setBounds (r.removeFromTop (26));
        auto chipsRow = r.removeFromTop (34);
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
        auto meterColumn = area.removeFromLeft (160);
        area.removeFromLeft (18);
        auto chain = area.removeFromLeft (361);
        area.removeFromLeft (18);
        auto out = area.removeFromRight (250);
        area.removeFromRight (18);
        layoutHead (head);
        layoutMeter (meterColumn);
        layoutChain (chain.withHeight (chainH));
        layoutSenders (area);
        layoutOut (out, true);
    }
    else if (layout == CardLayout::medium)
    {
        auto top = area.removeFromTop (juce::jmax (132, chainH));
        area.removeFromTop (16);
        auto head = top.removeFromLeft (194);
        top.removeFromLeft (18);
        auto meterColumn = top.removeFromLeft (160);
        top.removeFromLeft (18);
        layoutHead (head);
        layoutMeter (meterColumn);
        layoutChain (top.withHeight (chainH));
        layoutSenders (area.removeFromLeft (194 + 18 + 160));
        area.removeFromLeft (18);
        layoutOut (area, false);
    }
    else
    {
        layoutHead (area.removeFromTop (114));
        area.removeFromTop (14);
        layoutMeter (area.removeFromTop (meterHeight));
        area.removeFromTop (14);
        layoutChain (area.removeFromTop (chainH));
        area.removeFromTop (14);
        layoutSenders (area.removeFromTop (sendersHeightForWidth (getWidth())));
        area.removeFromTop (14);
        layoutOut (area, false);
    }
}

void AuxCard::paint (juce::Graphics& g)
{
    const auto* a = aux();
    const bool off = a != nullptr && ! a->on;
    if (off) g.beginTransparencyLayer (0.62f);
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Palette::card);
    g.fillRoundedRectangle (bounds, Palette::cardRadius);
    g.setColour (Palette::line);
    g.drawRoundedRectangle (bounds, Palette::cardRadius, 1.0f);
    if (off) g.endTransparencyLayer();
}

} // namespace gocue::livemix
