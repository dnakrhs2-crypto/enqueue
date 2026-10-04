#include "MasterCard.h"

#include <cmath>

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
    obsToggle.setTooltip (ko ("OBS에서 반드시 소스 추가(+)로 'LiveMix'를 추가해 주세요"));
    obsToggle.setWantsKeyboardFocus (false);
    obsToggle.onClick = [this]
    {
        const bool enabled = obsToggle.getToggleState();
        document.setSendToObs (enabled);
        if (enabled && onObsEnabled) onObsEnabled();
    };
    addAndMakeVisible (obsToggle);
    styleCaption (obsStatusLabel, "");
    obsStatusLabel.setComponentID ("obs-status");
    obsStatusLabel.setFont (bodyFont (12.5f));
    obsStatusLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (obsStatusLabel);
    obsStatusLabel.addMouseListener (this, false);
    styleCaption (obsSourceHint, obsToggle.getTooltip());
    obsSourceHint.setComponentID ("obs-source-hint");
    obsSourceHint.setFont (bodyFont (12.5f));
    obsSourceHint.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (obsSourceHint);
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

void MasterCard::mouseUp (const juce::MouseEvent& event)
{
    if (event.eventComponent == &obsStatusLabel && ! event.mods.isPopupMenu()
        && obsStatus == ObsStatus::installNeeded && ! obsInstalling && onObsInstallRequested)
        onObsInstallRequested();
}

void MasterCard::setObsStatus (ObsStatus status, const juce::String& reason)
{
    if (obsStatus == status && obsStatusReason == reason) return;
    obsStatus = status;
    obsStatusReason = reason;
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

juce::String MasterCard::obsStatusText (ObsStatus status)
{
    switch (status)
    {
        case ObsStatus::audioStopped: return ko ("오디오 멈춤");
        case ObsStatus::sendFailed: return ko ("OBS 보내기 실패");
        case ObsStatus::installNeeded: return ko ("OBS 플러그인 설치 필요");
        case ObsStatus::restartObs: return ko ("OBS를 다시 시작하세요");
        case ObsStatus::connected: return ko ("OBS 연결됨");
        case ObsStatus::addSource: return ko ("OBS에 소스 추가 필요");
        case ObsStatus::updateObs: return ko ("OBS 31.1 이상 필요");
        case ObsStatus::portableObs: return ko ("휴대용 OBS: 플러그인 복사 필요");
        case ObsStatus::waiting: return ko ("OBS 대기 중");
    }
    return {};
}

MasterCard::ObsAdvice MasterCard::obsStatusFor (const juce::String& sendError, bool pluginCurrent, bool restartNeeded,
                                              bool deviceRunning, ObsSender::ReaderState reader,
                                              const std::vector<ObsPluginInstaller::RunningObs>& runningObs)
{
    const auto advice = [] (ObsStatus status, const juce::String& reason = {}) { return ObsAdvice { status, obsStatusText (status), reason }; };
    if (sendError.isNotEmpty()) return advice (ObsStatus::sendFailed, sendError);
    if (! pluginCurrent) return advice (ObsStatus::installNeeded);
    if (reader == ObsSender::ReaderState::connected) return advice (ObsStatus::connected);
    if (restartNeeded) return advice (ObsStatus::restartObs);
    if (! deviceRunning) return advice (ObsStatus::audioStopped);
    if (reader == ObsSender::ReaderState::idle)
        return advice (ObsStatus::addSource, ko ("OBS에 LiveMix 플러그인은 올라와 있지만 소리를 받는 곳이 없습니다. OBS 소스(+)에서 'LiveMix'를 추가하세요."));
    for (const auto& obs : runningObs)
        if (obs.needsUpdate())
            return advice (ObsStatus::updateObs, ko ("실행 중인 OBS ") + obs.versionString() + ko ("에서는 LiveMix 플러그인을 읽을 수 없습니다. OBS를 31.1 이상으로 업데이트하세요."));
    for (const auto& obs : runningObs)
        if (obs.portable)
            return advice (ObsStatus::portableObs, ko ("휴대용(포터블) OBS는 자동 설치 대상이 아닙니다. LiveMix 설치 폴더의 obs-plugin\\livemix-obs\\livemix-obs.dll을 포터블 OBS 폴더의 obs-plugins\\64bit\\에, obs-plugin\\livemix-obs\\data\\locale\\을 data\\obs-plugins\\livemix-obs\\locale\\에 복사한 뒤 OBS를 다시 시작하세요."));
    if (! runningObs.empty())
        return advice (ObsStatus::restartObs, ko ("켜져 있는 OBS가 LiveMix 플러그인을 읽지 않았습니다. OBS를 완전히 끄고 다시 켜세요."));
    return advice (ObsStatus::waiting, ko ("OBS를 켜고 소스(+)에서 'LiveMix'를 추가하세요."));
}

void MasterCard::refreshObsStatus()
{
    const auto text = obsStatusText (obsStatus);
    obsStatusLabel.setText (strip ? juce::String::fromUTF8 ("●") : text, juce::dontSendNotification);
    obsStatusLabel.setTooltip (text + (obsStatusReason.isEmpty() ? juce::String() : "\n" + obsStatusReason));
    obsStatusLabel.setMouseCursor (obsStatus == ObsStatus::installNeeded ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    obsStatusLabel.setJustificationType (strip ? juce::Justification::centred : juce::Justification::centredLeft);
    obsStatusLabel.setColour (juce::Label::textColourId, obsStatus == ObsStatus::audioStopped || obsStatus == ObsStatus::sendFailed ? Palette::danger
                              : obsStatus == ObsStatus::installNeeded || obsStatus == ObsStatus::restartObs || obsStatus == ObsStatus::addSource
                                || obsStatus == ObsStatus::updateObs || obsStatus == ObsStatus::portableObs ? juce::Colour (0xffffb454)
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

int MasterCard::obsHintHeight (int width) const
{
    juce::AttributedString text;
    text.append (obsSourceHint.getText(), obsSourceHint.getFont());
    juce::TextLayout layout;
    layout.createLayout (text, (float) juce::jmax (1, width - obsSourceHint.getBorderSize().getLeftAndRight()));
    return juce::jmax (obsRowHeight, (int) std::ceil (layout.getHeight()) + obsSourceHint.getBorderSize().getTopAndBottom() + 4);
}

int MasterCard::obsControlsHeight (int width, int hintGap) const
{
    const int controls = obsToggleWidth() + 8 + labelWidthForText (obsStatusLabel, obsStatusText (obsStatus));
    return (controls <= width ? obsRowHeight : 2 * obsRowHeight + obsRowGap) + hintGap + obsHintHeight (width);
}

int MasterCard::chainRowsForWidth (int width) const
{
    if (width < narrowBelow && chips.empty()) return 0;
    const int inner = width - 28;
    const int chainW = width < narrowBelow ? inner : width < wideBelow ? juce::jmin (361, inner - 194 - 160 - 36) : 361;
    return ChipFlow::layout (chips, juce::Rectangle<int> (0, 0, juce::jmax (1, chainW), 1), 30, false);
}

int MasterCard::getUnfoldedHeight (int width) const
{
    const int rows = chainRowsForWidth (width);
    const int inner = width - 28;
    if (width < narrowBelow)
    {
        return 24 + 34 + 8 + 30 + (rows > 0 ? 6 + rows * ChipFlow::rowStep : 0)
               + 2 + 30 + 8 + 18 + 46 + 8 + obsControlsHeight (inner, 0);
    }

    if (width < wideBelow)
        return 24 + 96 + (rows - 1) * ChipFlow::rowStep + 16
               + juce::jmax (102, 1 + obsControlsHeight (inner - 390, 4));

    // Two chip rows fit above the third-row buttons. Longer chains grow in the same 38 px steps.
    return 24 + juce::jmax (130 + juce::jmax (0, rows - 2) * ChipFlow::rowStep, obsControlsHeight (309, 2) - 1);
}

void MasterCard::resized()
{
    auto area = getLocalBounds().reduced (14, 12);
    const bool stacked = ! strip && getWidth() < wideBelow;

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
    obsSourceHint.setVisible (! strip);

    for (auto& chip : chips)
        chip->setVisible (! strip);

    if (strip)
    {
        // Reserve the measured OBS controls and a usable meter before optional title, LUFS and output controls.
        auto row = area.withSizeKeepingCentre (area.getWidth(), 34);
        badge.setBounds (row.removeFromLeft (32).reduced (0, 2));
        row.removeFromLeft (8);
        obsStatusLabel.setBounds (row.removeFromRight (labelWidthForText (obsStatusLabel, obsStatusLabel.getText())));
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

    auto layoutObs = [this] (juce::Rectangle<int> r, int hintGap, bool fullWidthHint)
    {
        auto row = r.removeFromTop (obsRowHeight);
        obsToggle.setBounds (row.removeFromLeft (obsToggleWidth()));
        row.removeFromLeft (8);
        const int statusWidth = labelWidthForText (obsStatusLabel, obsStatusLabel.getText());
        if (row.getWidth() < statusWidth)
        {
            r.removeFromTop (obsRowGap);
            row = r.removeFromTop (obsRowHeight);
        }
        obsStatusLabel.setBounds (row.removeFromLeft (statusWidth));
        r.removeFromTop (hintGap);
        const int hintWidth = fullWidthHint ? r.getWidth() : juce::jmin (r.getWidth(), labelWidthForText (obsSourceHint, obsSourceHint.getText()));
        obsSourceHint.setBounds (r.withSize (hintWidth, obsHintHeight (hintWidth)));
    };

    if (getWidth() < narrowBelow)
    {
        // Head, chain, latency + output, meter, OBS: the same signal order in a single stack.
        auto headRow = area.removeFromTop (34);
        badge.setBounds (headRow.removeFromLeft (32).reduced (0, 2));
        headRow.removeFromLeft (10);
        title.setBounds (headRow.withWidth (juce::jmin (160, headRow.getWidth())));
        area.removeFromTop (8);

        auto chainRow = area.removeFromTop (30);
        chainCaption.setBounds (chainRow.removeFromLeft (juce::jmin (110, juce::jmax (60, chainRow.getWidth() - 92 - 76 - 16))));
        chainRow.removeFromLeft (8);
        openChainButton.setBounds (chainRow.removeFromLeft (92));
        chainRow.removeFromLeft (8);
        addPluginButton.setBounds (chainRow.removeFromLeft (76));

        if (! chips.empty())
        {
            area.removeFromTop (6);
            const int rows = ChipFlow::layout (chips, area, 30, true);
            area.removeFromTop (rows * ChipFlow::rowStep);
        }

        area.removeFromTop (2);
        auto routing = area.removeFromTop (30);
        const int outputWidth = juce::jlimit (90, 150, (routing.getWidth() - 44) / 3);
        outputCombo.setBounds (routing.removeFromRight (outputWidth));
        routing.removeFromRight (6);
        outputCaption.setBounds (routing.removeFromRight (60));
        routing.removeFromRight (18);
        lufsButton.setBounds (routing.removeFromLeft (56));
        routing.removeFromLeft (8);
        compactLatency.setBounds (routing);
        area.removeFromTop (8);
        meterCaption.setBounds (area.removeFromTop (18));
        meter_.setBounds (area.removeFromTop (46));
        area.removeFromTop (8);
        layoutObs (area, 0, true);
        return;
    }

    const auto content = area;
    auto head = area.removeFromLeft (194);
    auto row = head.removeFromTop (34);
    badge.setBounds (row.removeFromLeft (32).reduced (0, 2));
    row.removeFromLeft (10);
    title.setBounds (row);
    head.removeFromTop (6);
    note.setBounds (head.removeFromTop (20));
    area.removeFromLeft (18);

    auto chain = area.removeFromLeft (getWidth() < wideBelow ? juce::jmin (361, content.getWidth() - 194 - 160 - 36) : 361);
    area.removeFromLeft (18);
    auto lat = area.removeFromLeft (160);
    const int rows = chainRowsForWidth (getWidth());
    const int extra = juce::jmax (0, rows - (stacked ? 1 : 2)) * ChipFlow::rowStep;
    chainCaption.setBounds (chain.removeFromTop (26));
    chain.removeFromTop (1);
    ChipFlow::layout (chips, chain, 30, true);
    auto buttons = chain.withY ((stacked ? 78 : 112) + extra).withHeight (30);
    openChainButton.setBounds (buttons.removeFromLeft (92));
    buttons.removeFromLeft (8);
    addPluginButton.setBounds (buttons.removeFromLeft (76));
    if (stacked)
    {
        compactLatency.setBounds (lat.removeFromTop (26).withWidth (140));
        lat.removeFromTop (2);
        lufsButton.setBounds (lat.removeFromTop (30).withWidth (56));
        auto lower = content.withY (124 + extra).withHeight (getHeight() - 12 - 124 - extra);
        auto out = lower.removeFromLeft (372);
        lower.removeFromLeft (18);
        auto outputRow = out.removeFromTop (30);
        outputCaption.setBounds (outputRow.removeFromLeft (60));
        outputRow.removeFromLeft (6);
        outputCombo.setBounds (outputRow);
        out.removeFromTop (8);
        meterCaption.setBounds (out.removeFromTop (18));
        meter_.setBounds (out.removeFromTop (46));
        lower.removeFromTop (1);
        layoutObs (lower, 4, false);
        return;
    }

    latencyCaption.setBounds (lat.removeFromTop (26));
    latencyValue.setBounds (lat.removeFromTop (34));
    latencyNote.setBounds (lat.removeFromTop (20));
    lufsButton.setBounds (lat.withY (112 + extra).withSize (56, 30));

    area.removeFromLeft (18);
    auto obs = area.removeFromRight (309);
    area.removeFromRight (18);
    auto out = area;
    outputCaption.setBounds (out.removeFromTop (26));
    out.removeFromTop (2);
    outputCombo.setBounds (out.removeFromTop (30));
    out.removeFromTop (8);
    meterCaption.setBounds (out.removeFromTop (18));
    meter_.setBounds (out.removeFromTop (46));
    layoutObs (obs.withY (11), 2, false);
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
