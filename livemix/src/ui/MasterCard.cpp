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
    return obsToggleWidthFor (obsToggle.getButtonText());
}

int MasterCard::obsToggleWidthFor (const juce::String& text) const
{
    // Match LiveMixLookAndFeel::drawToggleButton: 18 px text at this row height, tick + left/right margins.
    const float size = juce::jmin (pt (15.0f), (float) obsRowHeight * 0.75f);
    return juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (size)), text)
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
        selectSavedChannel (outputCombo, outputNames, document.getSession().master.outputFirst, true);
    }
    else
    {
        outputCombo.clear (juce::dontSendNotification);
        outputCombo.addItem (device.output.isEmpty() ? ko ("없음 (OBS로만)") : juce::String ("1-2"), 1);
        outputCombo.setSelectedId (1, juce::dontSendNotification);
        outputCombo.setColour (juce::ComboBox::textColourId, Palette::text);
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
    // measured with the unfolded toggle text even while folded (the strip says only 'OBS'), so the unfolded height
    // the main window compares against does not change with the fold itself
    const int controls = obsToggleWidthFor (obsInstalling ? ko ("설치 중...") : ko ("OBS로 보내기")) + 8
                         + labelWidthForText (obsStatusLabel, obsStatusText (obsStatus));
    return (controls <= width ? obsRowHeight : 2 * obsRowHeight + obsRowGap) + hintGap + obsHintHeight (width);
}

bool MasterCard::mediumLatencyFitsBesideChain (int width)
{
    // One minimum-width chip and both actions, an 18 px gap, then latency + LUFS.
    return width - 418 >= ChipFlow::minWidth + 8 + 92 + 8 + 76 + 18 + 134 + 8 + 56;
}

MasterCard::ChainLayout MasterCard::measureChain (int width) const
{
    ChainLayout result;
    const bool narrow = width < narrowBelow;
    const bool medium = ! narrow && width < wideBelow;
    const int left = narrow ? 14 : 404;
    const int top = narrow ? 54 : 12;
    const int columnWidth = juce::jmax (1, narrow ? width - 28 : medium ? juce::jmin (361, width - 418) : 361);
    const int flowWidth = medium && mediumLatencyFitsBesideChain (width) ? juce::jmin (columnWidth, width - 418 - 216) : columnWidth;
    result.caption = { left, top + (narrow ? 1 : 0), narrow ? 70 : columnWidth, narrow ? 30 : 26 };
    int x = narrow ? 78 : 0;
    int y = top + (narrow ? 0 : 27);
    for (const auto& chip : chips)
    {
        const int w = ChipFlow::width (chip->getButtonText(), 30, flowWidth);
        if (x > 0 && x + w > flowWidth)
        {
            x = 0;
            y += ChipFlow::rowStep;
        }
        result.chips.emplace_back (left + x, y, w, ChipFlow::height);
        x += w + ChipFlow::gap;
    }
    if (! chips.empty()) x += 8 - ChipFlow::gap;
    if (x > 0 && x + 176 > flowWidth)
    {
        x = 0;
        y += ChipFlow::rowStep;
    }
    result.open = { left + x, y + 1, 92, 30 };
    result.add = { left + x + 100, y + 1, 76, 30 };
    result.bottom = y + ChipFlow::height;
    return result;
}

int MasterCard::getUnfoldedHeight (int width) const
{
    const auto chain = measureChain (width);
    const int inner = width - 28;
    if (width < narrowBelow)
    {
        return chain.bottom + 8 + 30 + 8 + 30 + 8 + 18 + 46 + 8 + obsControlsHeight (inner, 0) + 12;
    }

    if (width < wideBelow)
        return chain.bottom + 17 + (mediumLatencyFitsBesideChain (width) ? 0 : 38)
               + juce::jmax (102, 1 + obsControlsHeight (inner - 390, 4)) + 12;

    // The native two-line hint measures 37 px, versus the mockup's 32. It still fits the 192 px card
    // with 7 px below it; retain the measured text height and grow for longer statuses or chains.
    return juce::jmax (192, chain.bottom + 12, 118 + obsControlsHeight (250, 2) + 7);
}

void MasterCard::resized()
{
    auto area = getLocalBounds().reduced (14, 12);
    const bool stacked = ! strip && getWidth() < wideBelow;

    // Keep the existing visibility rules: compact forms use the single latency label; the strip keeps its controls.
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

    auto layoutObs = [this] (juce::Rectangle<int> r, int hintGap)
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
        obsSourceHint.setBounds (r.withHeight (obsHintHeight (r.getWidth())));
    };

    const auto chain = measureChain (getWidth());
    chainCaption.setBounds (chain.caption);
    openChainButton.setBounds (chain.open);
    addPluginButton.setBounds (chain.add);
    for (size_t i = 0; i < chips.size(); ++i)
        chips[i]->setBounds (chain.chips[i]);

    if (getWidth() < narrowBelow)
    {
        // Head, inline chain, latency + LUFS, then the complete output zone.
        auto headRow = area.removeFromTop (34);
        badge.setBounds (headRow.removeFromLeft (32).reduced (0, 2));
        headRow.removeFromLeft (10);
        title.setBounds (headRow.withWidth (juce::jmin (160, headRow.getWidth())));
        area.setTop (chain.bottom + 8);
        auto latency = area.removeFromTop (30);
        compactLatency.setBounds (latency.removeFromLeft (134));
        latency.removeFromLeft (8);
        lufsButton.setBounds (latency.removeFromLeft (56));
        area.removeFromTop (8);
        auto output = area.removeFromTop (30);
        outputCaption.setBounds (output.removeFromLeft (60));
        output.removeFromLeft (6);
        outputCombo.setBounds (output);
        area.removeFromTop (8);
        meterCaption.setBounds (area.removeFromTop (18));
        meter_.setBounds (area.removeFromTop (46));
        area.removeFromTop (8);
        layoutObs (area, 0);
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
    if (stacked)
    {
        const bool inlineLatency = mediumLatencyFitsBesideChain (getWidth());
        auto latency = content.withHeight (30);
        latency.setY (inlineLatency ? 40 : chain.bottom + 8);
        lufsButton.setBounds (latency.removeFromRight (56));
        latency.removeFromRight (8);
        compactLatency.setBounds (latency.removeFromRight (134));
        auto lower = content.withTop (chain.bottom + 17 + (inlineLatency ? 0 : 38));
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
        layoutObs (lower, 4);
        return;
    }

    latencyCaption.setBounds (783, 12, 168, 26);
    latencyValue.setBounds (783, 38, 104, 34);
    latencyNote.setBounds (783, 72, 168, 20);
    lufsButton.setBounds (895, 40, 56, 30);

    auto out = content.withLeft (getWidth() - 264);   // under the cards' 250 px output column
    auto outputRow = out.removeFromTop (26);
    outputCaption.setBounds (outputRow.removeFromLeft (60));
    outputRow.removeFromLeft (6);
    outputCombo.setBounds (outputRow.expanded (0, 2));
    out.removeFromTop (8);
    meterCaption.setBounds (out.removeFromTop (18));
    meter_.setBounds (out.removeFromTop (46));
    out.removeFromTop (8);
    layoutObs (out, 2);
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
