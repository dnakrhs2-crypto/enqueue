#include "FxDrawer.h"

namespace gocue::livemix
{

FxDrawer::~FxDrawer() = default;

FxDrawer::FxDrawer (MixDocument& doc) : document (doc)
{
    masterChip.setButtonText (ko ("마스터"));
    directChip.setButtonText (ko ("직접 출력"));
    monoChip.setButtonText (ko ("모노"));
    title.setText (ko ("FX 채널"), juce::dontSendNotification);
    title.setFont (titleFont (17.0f));
    addAndMakeVisible (title);
    closeButton.setWantsKeyboardFocus (false);
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);

    addFxButton.setButtonText (ko ("+ FX 채널"));
    addFxButton.setWantsKeyboardFocus (false);
    addFxButton.onClick = [this]
    {
        const auto id = document.addFx();

        if (! id.isNull())
            selectFx (id);
    };
    addAndMakeVisible (addFxButton);
    removeFxButton.setButtonText (ko ("이 FX 삭제"));
    removeFxButton.setWantsKeyboardFocus (false);
    removeFxButton.onClick = [this] { removeSelected(); };
    addAndMakeVisible (removeFxButton);

    name.onRenamed = [this] (const juce::String& text) { if (! selected.isNull()) document.renameFx (selected, text); };
    addAndMakeVisible (name);

    styleCaption (chainCaption, ko ("VST3 체인"));
    addAndMakeVisible (chainCaption);
    openChainButton.setButtonText (ko ("체인 열기"));
    openChainButton.setWantsKeyboardFocus (false);
    openChainButton.onClick = [this] { if (onOpenChain && ! selected.isNull()) onOpenChain (selected); };
    addAndMakeVisible (openChainButton);
    addPluginButton.setButtonText (ko ("+ 추가"));
    addPluginButton.setWantsKeyboardFocus (false);
    addPluginButton.onClick = [this] { if (onAddPlugin && ! selected.isNull()) onAddPlugin (selected); };
    addAndMakeVisible (addPluginButton);

    styleCaption (returnCaption, ko ("돌아오는 양") + "   " + ko ("마스터로"));
    addAndMakeVisible (returnCaption);
    returnSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    returnSlider.setRange (0.0, 100.0, 1.0);
    returnSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    returnSlider.setWantsKeyboardFocus (false);
    returnSlider.onValueChange = [this]
    {
        returnValue.setText (juce::String ((int) std::lround (returnSlider.getValue())) + "%", juce::dontSendNotification);

        if (! refreshing && ! selected.isNull())
            document.setFxReturn (selected, returnSlider.getValue() / 100.0);
    };
    addAndMakeVisible (returnSlider);
    returnValue.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    returnValue.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (returnValue);

    styleCaption (outputCaption, ko ("출력"));
    addAndMakeVisible (outputCaption);
    masterChip.onClick = [this] { commitOutput(); };
    addAndMakeVisible (masterChip);
    directChip.onClick = [this] { commitOutput(); };
    addAndMakeVisible (directChip);
    directCombo.setWantsKeyboardFocus (false);
    directCombo.onChange = [this] { commitOutput(); };
    addAndMakeVisible (directCombo);
    monoChip.setTooltip (ko ("켜면 이 FX의 소리를 좌우 합쳐 양쪽에 똑같이 내보냅니다"));
    monoChip.onClick = [this]
    {
        if (! refreshing && ! selected.isNull())
            document.setFxMono (selected, monoChip.getToggleState());
    };
    addAndMakeVisible (monoChip);

    styleCaption (meterCaption, ko ("미터"));
    addAndMakeVisible (meterCaption);
    addAndMakeVisible (meter_);

    styleCaption (sendersCaption, ko ("이 FX로 보내는 마이크"));
    addAndMakeVisible (sendersCaption);
    styleCaption (note, ko ("양과 프리/포스트는 각 마이크 카드의 \"이펙터\" 칸에서 조절합니다"));
    note.setFont (bodyFont (12.5f));
    addAndMakeVisible (note);

    refresh();
}

const MixFx* FxDrawer::fx() const
{
    return document.getSession().findFx (selected);
}

void FxDrawer::setDeviceChannels (const juce::StringArray& outs)
{
    outputNames = outs;
    refresh();
}

void FxDrawer::selectFx (const juce::Uuid& id)
{
    selected = id;
    refresh();
}

void FxDrawer::pushMeter (const juce::Uuid& fxId, MixEngine::Meter meter)
{
    if (fxId == selected)
        meter_.push (meter);
}

void FxDrawer::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto& session = document.getSession();

    if (fx() == nullptr)
        selected = session.fx.empty() ? juce::Uuid::null() : session.fx.front().id;

    rebuildTabs();
    const auto* f = fx();
    const bool have = f != nullptr;

    for (auto* c : std::initializer_list<juce::Component*> { &name, &openChainButton, &addPluginButton, &returnSlider, &masterChip, &directChip, &monoChip, &directCombo, &removeFxButton })
        c->setEnabled (have);

    addFxButton.setEnabled ((int) session.fx.size() < MixSession::maxFx);

    if (have)
    {
        if (! name.isBeingEdited())
            name.setText (f->name, juce::dontSendNotification);

        returnSlider.setValue (f->returnAmount * 100.0, juce::dontSendNotification);
        returnValue.setText (juce::String ((int) std::lround (f->returnAmount * 100.0)) + "%", juce::dontSendNotification);
        masterChip.setToggleState (f->output.master, juce::dontSendNotification);
        directChip.setToggleState (f->output.direct, juce::dontSendNotification);
        fillChannelCombo (directCombo, outputNames, true, MixSession::maxDeviceChannels);
        directCombo.setSelectedId (f->output.directFirst + 1, juce::dontSendNotification);
        directCombo.setEnabled (f->output.direct);
        monoChip.setToggleState (f->mono, juce::dontSendNotification);
    }
    else
    {
        name.setText (ko ("FX 채널이 없습니다"), juce::dontSendNotification);
    }

    rebuildChain();
    rebuildSenders();
    resized();
}

void FxDrawer::rebuildTabs()
{
    tabs.clear();
    const auto& session = document.getSession();

    for (size_t i = 0; i < session.fx.size(); ++i)
    {
        const auto& f = session.fx[i];
        auto tab = std::make_unique<juce::TextButton> ("FX" + juce::String ((int) i + 1) + "  " + f.name);
        tab->setWantsKeyboardFocus (false);
        tab->setToggleState (f.id == selected, juce::dontSendNotification);
        tab->setColour (juce::TextButton::buttonOnColourId, Palette::accent);
        const auto id = f.id;
        tab->onClick = [this, id] { selectFx (id); };
        addAndMakeVisible (*tab);
        tabs.push_back (std::move (tab));
    }
}

void FxDrawer::rebuildChain()
{
    chips.clear();
    auto* chain = selected.isNull() ? nullptr : document.getEngine().getFxChain (selected);

    if (chain == nullptr)
        return;

    for (int i = 0; i < chain->getNumSlots(); ++i)
    {
        const auto& slot = chain->getSlot (i);
        auto chip = std::make_unique<juce::TextButton> (juce::String (i + 1) + "  " + (slot.plugin != nullptr ? slot.plugin->getName() : slot.state.name + ko (" (없음)")));
        chip->setWantsKeyboardFocus (false);
        chip->setColour (juce::TextButton::buttonColourId, Palette::slotBg);
        chip->setColour (juce::TextButton::textColourOffId, slot.bypassed.load() ? Palette::dimText : Palette::text);
        const auto id = selected;
        chip->onClick = [this, id, i] { if (onOpenPluginEditor) onOpenPluginEditor (id, i); };
        addAndMakeVisible (*chip);
        chips.push_back (std::move (chip));
    }
}

void FxDrawer::rebuildSenders()
{
    senders.clear();
    const auto& session = document.getSession();

    for (size_t i = 0; i < session.channels.size(); ++i)
    {
        const auto& c = session.channels[i];
        MixSend send;

        for (const auto& s : c.sends)
            if (s.fx == selected)
                send = s;

        auto label = std::make_unique<juce::Label>();
        const auto amount = juce::String ((int) std::lround (send.amount * 100.0)) + "%";
        label->setText (juce::String ((int) i + 1) + "  " + c.name + "     " + amount + (send.amount > 0.0 ? (send.pre ? ko (" · 프리") : ko (" · 포스트")) : juce::String()), juce::dontSendNotification);
        label->setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        label->setColour (juce::Label::textColourId, send.amount > 0.0 && c.on ? Palette::text : Palette::dimText);
        label->setColour (juce::Label::backgroundColourId, Palette::card2);
        label->setColour (juce::Label::outlineColourId, Palette::line);
        label->setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (*label);
        senders.push_back (std::move (label));
    }
}

void FxDrawer::commitOutput()
{
    if (refreshing || selected.isNull())
        return;

    MixOutput output;
    output.master = masterChip.getToggleState();
    output.direct = directChip.getToggleState();
    output.directFirst = juce::jmax (0, directCombo.getSelectedId() - 1);
    document.setFxOutput (selected, output);
}

void FxDrawer::removeSelected()
{
    if (selected.isNull())
        return;

    const auto id = selected;
    juce::Component::SafePointer<FxDrawer> safeThis (this);
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle (ko ("FX 채널 삭제"))
                                      .withMessage (ko ("이 FX 채널과 그 플러그인, 마이크들의 샌드 설정이 지워집니다. 삭제할까요?"))
                                      .withButton (ko ("삭제"))
                                      .withButton (ko ("취소")),
                                  [safeThis, id] (int result)
    {
        if (safeThis != nullptr && result == 1)
            safeThis->document.removeFx (id);
    });
}

void FxDrawer::resized()
{
    auto area = getLocalBounds().reduced (18, 16);
    auto head = area.removeFromTop (34);
    closeButton.setBounds (head.removeFromRight (34).reduced (0, 3));
    head.removeFromRight (8);
    title.setBounds (head);
    area.removeFromTop (10);

    auto tabRow = area.removeFromTop (32);
    addFxButton.setBounds (tabRow.removeFromRight (90));
    tabRow.removeFromRight (6);

    for (auto& tab : tabs)
    {
        const int w = juce::jmin (150, juce::jmax (80, tabRow.getWidth() / juce::jmax (1, (int) tabs.size())));
        tab->setBounds (tabRow.removeFromLeft (w));
        tabRow.removeFromLeft (6);
    }

    area.removeFromTop (12);
    auto nameRow = area.removeFromTop (30);
    removeFxButton.setBounds (nameRow.removeFromRight (90));
    nameRow.removeFromRight (8);
    name.setBounds (nameRow);
    area.removeFromTop (12);

    chainCaption.setBounds (area.removeFromTop (20));
    int x = area.getX(), y = area.getY();
    const int chipRows = juce::jmax (1, ((int) chips.size() + 1) / 2);

    for (auto& chip : chips)
    {
        const int w = juce::jlimit (110, area.getWidth(), 30 + juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)), chip->getButtonText())));

        if (x + w > area.getRight() && x > area.getX())
        {
            x = area.getX();
            y += 38;
        }

        chip->setBounds (x, y, w, 32);
        x += w + 6;
    }

    area.removeFromTop (chips.empty() ? 0 : chipRows * 38);
    auto buttons = area.removeFromTop (30);
    openChainButton.setBounds (buttons.removeFromLeft (92));
    buttons.removeFromLeft (8);
    addPluginButton.setBounds (buttons.removeFromLeft (76));
    area.removeFromTop (14);

    returnCaption.setBounds (area.removeFromTop (20));
    auto ret = area.removeFromTop (34);
    returnValue.setBounds (ret.removeFromRight (64));
    ret.removeFromRight (8);
    returnSlider.setBounds (ret);
    area.removeFromTop (12);

    outputCaption.setBounds (area.removeFromTop (20));
    auto out = area.removeFromTop (34);
    masterChip.setBounds (out.removeFromLeft (72));
    out.removeFromLeft (8);
    directChip.setBounds (out.removeFromLeft (84));
    out.removeFromLeft (6);
    monoChip.setBounds (out.removeFromLeft (58));
    out.removeFromLeft (6);
    directCombo.setBounds (out.withHeight (30).withY (out.getY() + 2));
    area.removeFromTop (12);

    meterCaption.setBounds (area.removeFromTop (18));
    meter_.setBounds (area.removeFromTop (44));
    area.removeFromTop (12);

    sendersCaption.setBounds (area.removeFromTop (20));

    for (auto& s : senders)
    {
        s->setBounds (area.removeFromTop (30).reduced (0, 1));
        area.removeFromTop (4);
    }

    area.removeFromTop (4);
    note.setBounds (area.removeFromTop (36));
}

void FxDrawer::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bar);
    g.setColour (Palette::line);
    g.fillRect (getLocalBounds().removeFromLeft (1));
}

} // namespace gocue::livemix
