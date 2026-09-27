#include "MasterCard.h"

namespace gocue::livemix
{

MasterCard::MasterCard (MixDocument& doc) : document (doc)
{
    badge.setText ("M", juce::dontSendNotification);
    badge.setFont (juce::Font (juce::FontOptions (pt (14.0f), juce::Font::bold)));
    badge.setJustificationType (juce::Justification::centred);
    badge.setColour (juce::Label::backgroundColourId, Palette::text);
    badge.setColour (juce::Label::textColourId, Palette::background);
    addAndMakeVisible (badge);
    title.setText (ko ("마스터"), juce::dontSendNotification);
    title.setFont (titleFont());
    addAndMakeVisible (title);
    styleCaption (note, ko ("마이크 + FX 전부 여기로"));
    note.setFont (bodyFont (12.5f));
    addAndMakeVisible (note);

    styleCaption (chainCaption, ko ("VST3 체인"));
    addAndMakeVisible (chainCaption);
    openChainButton.setButtonText (ko ("체인 열기"));
    openChainButton.setWantsKeyboardFocus (false);
    openChainButton.onClick = [this] { if (onOpenChain) onOpenChain(); };
    addAndMakeVisible (openChainButton);
    addPluginButton.setButtonText (ko ("+ 추가"));
    addPluginButton.setWantsKeyboardFocus (false);
    addPluginButton.onClick = [this] { if (onAddPlugin) onAddPlugin(); };
    addAndMakeVisible (addPluginButton);
    lufsButton.setButtonText ("LUFS");
    lufsButton.setTooltip (ko ("마스터 출력의 라우드니스 미터 (LUFS, 트루 피크)"));
    lufsButton.setWantsKeyboardFocus (false);
    lufsButton.onClick = [this] { if (onOpenLoudness) onOpenLoudness(); };
    addAndMakeVisible (lufsButton);

    styleCaption (latencyCaption, ko ("지연"));
    addAndMakeVisible (latencyCaption);
    latencyValue.setFont (juce::Font (juce::FontOptions (pt (26.0f), juce::Font::bold)));
    latencyValue.setText ("-", juce::dontSendNotification);
    addAndMakeVisible (latencyValue);
    styleCaption (latencyNote, "");
    latencyNote.setFont (bodyFont (12.5f));
    addAndMakeVisible (latencyNote);
    styleCaption (compactLatency, "");   // the stack: the latency in one line at the right of the meter's caption
    compactLatency.setFont (bodyFont (12.5f));
    compactLatency.setJustificationType (juce::Justification::centredRight);
    addChildComponent (compactLatency);

    styleCaption (outputCaption, ko ("메인 출력"));
    addAndMakeVisible (outputCaption);
    outputCombo.setWantsKeyboardFocus (false);
    outputCombo.onChange = [this]
    {
        if (! refreshing)
            document.setMasterOutput (juce::jmax (0, outputCombo.getSelectedId() - 1));
    };
    addAndMakeVisible (outputCombo);

    obsToggle.setButtonText (ko ("OBS로 보내기"));
    obsToggle.setWantsKeyboardFocus (false);
    obsToggle.onClick = [this]
    {
        const bool enabled = obsToggle.getToggleState();
        document.setSendToObs (enabled);
        if (enabled && onObsEnabled) onObsEnabled();
    };
    addAndMakeVisible (obsToggle);
    styleCaption (obsStatusLabel, "");
    obsStatusLabel.setFont (bodyFont (12.5f));
    obsStatusLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (obsStatusLabel);
    refreshObsStatus();

    styleCaption (meterCaption, ko ("출력 미터 L / R"));
    addAndMakeVisible (meterCaption);
    addAndMakeVisible (meter_);
    refresh();
}

void MasterCard::setDeviceChannels (const juce::StringArray& outs)
{
    outputNames = outs;
    refresh();
}

void MasterCard::setLatency (double ms, int bufferSize, double sampleRate)
{
    latencyValue.setText (ms > 0.0 ? juce::String (ms, 1) + " ms" : "-", juce::dontSendNotification);
    latencyNote.setText (bufferSize > 0 ? juce::String (bufferSize) + ko (" 샘플") + " · " + juce::String (sampleRate / 1000.0, 1) + " kHz" : ko ("장치 없음"),
                         juce::dontSendNotification);
    compactLatency.setText (ms > 0.0 ? ko ("지연 ") + juce::String (ms, 1) + " ms · " + juce::String (bufferSize) + ko (" 샘플") : ko ("장치 없음"),
                            juce::dontSendNotification);
}

void MasterCard::setStrip (bool folded)
{
    if (strip == folded)
        return;

    strip = folded;
    refreshObsToggle();
    refreshObsStatus();
    resized();
}

int MasterCard::obsToggleWidth() const
{
    // Match LiveMixLookAndFeel::drawToggleButton: 18 px text at this row height, tick + left/right margins.
    const float size = juce::jmin (pt (15.0f), (float) obsRowHeight * 0.75f);
    return juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (size)), obsToggle.getButtonText())
           + juce::roundToInt (size * 1.1f) + 12;
}

void MasterCard::setObsStatus (ObsStatus status)
{
    if (obsStatus == status) return;
    obsStatus = status;
    refreshObsStatus();
    resized();
}

void MasterCard::setObsInstalling (bool installing)
{
    if (obsInstalling == installing) return;
    obsInstalling = installing;
    refreshObsToggle();
    resized();
}

void MasterCard::refreshObsToggle()
{
    obsToggle.setButtonText (obsInstalling ? ko ("설치 중...") : strip ? juce::String ("OBS") : ko ("OBS로 보내기"));
}

void MasterCard::refreshObsStatus()
{
    const auto text = obsStatus == ObsStatus::audioStopped ? ko ("오디오 멈춤")
                    : obsStatus == ObsStatus::installNeeded ? ko ("OBS 플러그인 설치 필요")
                    : obsStatus == ObsStatus::restartObs ? ko ("OBS를 다시 시작하세요")
                    : obsStatus == ObsStatus::connected ? ko ("OBS 연결됨") : ko ("OBS 대기 중");
    obsStatusLabel.setText (strip ? juce::String::fromUTF8 ("●") : text, juce::dontSendNotification);
    obsStatusLabel.setTooltip (text);
    obsStatusLabel.setJustificationType (strip ? juce::Justification::centred : juce::Justification::centredLeft);
    obsStatusLabel.setColour (juce::Label::textColourId, obsStatus == ObsStatus::audioStopped ? Palette::danger
                              : obsStatus == ObsStatus::installNeeded || obsStatus == ObsStatus::restartObs ? juce::Colour (0xffffb454)
                              : obsStatus == ObsStatus::connected ? Palette::lampOn : Palette::dimText);
}

void MasterCard::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto running = document.getEngine().getOpenDevice();
    const auto device = running.input.isNotEmpty() ? running : document.getSession().device;
    if (device.isAsio())
    {
        fillChannelCombo (outputCombo, outputNames, true, MixSession::maxDeviceChannels);
        outputCombo.setSelectedId (document.getSession().master.outputFirst + 1, juce::dontSendNotification);
    }
    else
    {
        outputCombo.clear (juce::dontSendNotification);
        outputCombo.addItem (device.output.isEmpty() ? ko ("없음 (OBS로만)") : juce::String ("1-2"), 1);
        outputCombo.setSelectedId (1, juce::dontSendNotification);
    }
    obsToggle.setToggleState (document.getSession().master.sendToObs, juce::dontSendNotification);
    rebuildChain();
    resized();
}

void MasterCard::rebuildChain()
{
    chips.clear();
    auto& chain = document.getEngine().getMasterChain();

    for (int i = 0; i < chain.getNumSlots(); ++i)
    {
        const auto& slot = chain.getSlot (i);
        auto chip = std::make_unique<juce::TextButton> (juce::String (i + 1) + "  " + (slot.plugin != nullptr ? slot.plugin->getName() : slot.state.name + ko (" (없음)")));
        chip->setWantsKeyboardFocus (false);
        chip->setColour (juce::TextButton::buttonColourId, Palette::slotBg);
        chip->setColour (juce::TextButton::textColourOffId, slot.bypassed.load() ? Palette::dimText : Palette::text);
        chip->onClick = [this, i] { if (onOpenPluginEditor) onOpenPluginEditor (i); };
        addAndMakeVisible (*chip);
        chips.push_back (std::move (chip));
    }
}

int MasterCard::getPreferredHeight (int width) const
{
    return strip ? stripHeight : getUnfoldedHeight (width);
}

int MasterCard::getUnfoldedHeight (int width) const
{
    if (width < narrowBelow)
    {
        // the compact stack: the head row (with the output pair), the chain row and its chips (none: no row), the meter's caption row, the meter
        const int rows = chips.empty() ? 0 : ChipFlow::layout (chips, juce::Rectangle<int> (0, 0, juce::jmax (1, width - 28), 1), 30, false);
        return 24 + 34 + 8 + 30 + (rows > 0 ? 6 + rows * ChipFlow::rowStep : 0) + 10 + 18 + 46 + obsRowGap + obsRowHeight;
    }

    // the chain column's width in resized() at this card width, then the rows the chips take in it
    const int afterHeadAndOut = width - 28 - 230 - 18 - 250 - 18;
    const int chainW = afterHeadAndOut - juce::jlimit (160, 300, afterHeadAndOut / 3) - 18;
    const int rows = ChipFlow::layout (chips, juce::Rectangle<int> (0, 0, juce::jmax (1, chainW), 1), 30, false);
    return juce::jmax (176, 24 + 22 + rows * ChipFlow::rowStep + 6 + 30) + obsRowGap + obsRowHeight;
}

void MasterCard::resized()
{
    auto area = getLocalBounds().reduced (14, 12);
    const bool stacked = ! strip && getWidth() < narrowBelow;

    // what each form shows: the columns everything; the stack no note and no latency block (the latency goes on the
    // meter's caption row); the strip only the badge, title, meter, chain button and output pair
    note.setVisible (! strip && ! stacked);
    latencyCaption.setVisible (! strip && ! stacked);
    latencyValue.setVisible (! strip && ! stacked);
    latencyNote.setVisible (! strip && ! stacked);
    compactLatency.setVisible (stacked);
    chainCaption.setVisible (! strip);
    addPluginButton.setVisible (! strip);
    meterCaption.setVisible (! strip);
    outputCaption.setVisible (! strip);
    outputCombo.setVisible (true);   // the strip may hide it below
    title.setVisible (true);
    lufsButton.setVisible (true);

    for (auto& chip : chips)
        chip->setVisible (! strip);

    if (strip)
    {
        // Reserve the measured OBS controls and a usable meter before optional title, LUFS and output controls.
        auto row = area.withSizeKeepingCentre (area.getWidth(), 34);
        badge.setBounds (row.removeFromLeft (32).reduced (0, 2));
        row.removeFromLeft (8);
        obsStatusLabel.setBounds (row.removeFromRight (20));
        row.removeFromRight (4);
        obsToggle.setBounds (row.removeFromRight (obsToggleWidth()).withSizeKeepingCentre (obsToggleWidth(), obsRowHeight));
        row.removeFromRight (8);
        openChainButton.setBounds (row.removeFromRight (88));
        row.removeFromRight (8);
        constexpr int meterMinimum = 64;
        const bool withLufs = row.getWidth() >= meterMinimum + 56 + 8;
        lufsButton.setVisible (withLufs);
        if (withLufs)
        {
            lufsButton.setBounds (row.removeFromRight (56));
            row.removeFromRight (8);
        }
        const bool withTitle = row.getWidth() >= meterMinimum + 64 + 8;
        title.setVisible (withTitle);
        if (withTitle)
        {
            title.setBounds (row.removeFromLeft (64));
            row.removeFromLeft (8);
        }
        const int outputWidth = juce::jmax (110, juce::GlyphArrangement::getStringWidthInt (bodyFont(), outputCombo.getText()) + 38);
        const bool withOutput = row.getWidth() >= meterMinimum + outputWidth + 8;
        outputCombo.setVisible (withOutput);
        if (withOutput)
        {
            outputCombo.setBounds (row.removeFromRight (outputWidth).reduced (0, 2));
            row.removeFromRight (8);
        }
        meter_.setBounds (row.reduced (0, 3));
        return;
    }

    auto obsRow = area.removeFromBottom (obsRowHeight);
    area.removeFromBottom (obsRowGap);
    obsToggle.setBounds (obsRow.removeFromLeft (obsToggleWidth()));
    obsRow.removeFromLeft (8);
    obsStatusLabel.setBounds (obsRow.removeFromLeft (labelWidthForText (obsStatusLabel, obsStatusLabel.getText())));

    if (stacked)
    {
        // a narrow window: the head row carries the output pair, the chain row its buttons, then the chips and the meter
        auto headRow = area.removeFromTop (34);
        badge.setBounds (headRow.removeFromLeft (32).reduced (0, 2));
        headRow.removeFromLeft (10);
        outputCombo.setBounds (headRow.removeFromRight (juce::jmin (150, juce::jmax (90, headRow.getWidth() / 3))).reduced (0, 2));
        headRow.removeFromRight (6);
        outputCaption.setBounds (headRow.removeFromRight (60));
        headRow.removeFromRight (6);
        title.setBounds (headRow);
        area.removeFromTop (8);

        auto chainRow = area.removeFromTop (30);
        chainCaption.setBounds (chainRow.removeFromLeft (juce::jmin (110, juce::jmax (60, chainRow.getWidth() - 92 - 76 - 56 - 24))));
        chainRow.removeFromLeft (8);
        openChainButton.setBounds (chainRow.removeFromLeft (92));
        chainRow.removeFromLeft (8);
        addPluginButton.setBounds (chainRow.removeFromLeft (76));
        chainRow.removeFromLeft (8);
        lufsButton.setBounds (chainRow.removeFromLeft (56));

        if (! chips.empty())
        {
            area.removeFromTop (6);
            const int rows = ChipFlow::layout (chips, area, 30, true);
            area.removeFromTop (rows * ChipFlow::rowStep);
        }

        area.removeFromTop (10);
        auto captionRow = area.removeFromTop (18);
        compactLatency.setBounds (captionRow.removeFromRight (juce::jmin (220, captionRow.getWidth() / 2)));
        meterCaption.setBounds (captionRow);
        meter_.setBounds (area.removeFromTop (juce::jmin (46, juce::jmax (0, area.getHeight()))));
        return;
    }

    auto head = area.removeFromLeft (230);
    auto row = head.removeFromTop (34);
    badge.setBounds (row.removeFromLeft (32).reduced (0, 2));
    row.removeFromLeft (10);
    title.setBounds (row);
    head.removeFromTop (6);
    note.setBounds (head.removeFromTop (20));
    area.removeFromLeft (18);

    auto out = area.removeFromRight (250);
    area.removeFromRight (18);
    auto lat = area.removeFromRight (juce::jlimit (160, 300, area.getWidth() / 3));
    area.removeFromRight (18);

    chainCaption.setBounds (area.removeFromTop (22));
    auto buttons = area.removeFromBottom (30);
    openChainButton.setBounds (buttons.removeFromLeft (92));
    buttons.removeFromLeft (8);
    addPluginButton.setBounds (buttons.removeFromLeft (76));
    buttons.removeFromLeft (8);
    lufsButton.setBounds (buttons.removeFromLeft (56));
    area.removeFromBottom (6);
    ChipFlow::layout (chips, area, 30, true);

    latencyCaption.setBounds (lat.removeFromTop (22));
    latencyValue.setBounds (lat.removeFromTop (34));
    latencyNote.setBounds (lat.removeFromTop (20));

    outputCaption.setBounds (out.removeFromTop (22));
    outputCombo.setBounds (out.removeFromTop (30).withWidth (juce::jmin (180, out.getWidth())));
    out.removeFromTop (10);
    meterCaption.setBounds (out.removeFromTop (18));
    meter_.setBounds (out.removeFromTop (juce::jmin (46, out.getHeight())));
}

void MasterCard::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Palette::masterCard);
    g.fillRoundedRectangle (bounds, Palette::cardRadius);
    g.setColour (Palette::line);
    g.drawRoundedRectangle (bounds, Palette::cardRadius, 1.0f);
}

} // namespace gocue::livemix
