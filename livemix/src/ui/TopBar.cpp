#include "TopBar.h"
#include "AudioBackends.h"
#include "DeviceFormatText.h"

namespace gocue::livemix
{

void TopBar::DspMeter::paint (juce::Graphics& g)
{
    auto bar = getLocalBounds().toFloat().reduced (0.0f, (float) getHeight() * 0.5f - 4.0f);
    g.setColour (Palette::meterBg);
    g.fillRoundedRectangle (bar, 4.0f);
    const float w = (float) juce::jlimit (0.0, 1.0, load) * bar.getWidth();
    g.setColour (load > 0.85 ? Palette::danger : load > 0.6 ? Palette::meterYellow : Palette::accent);
    g.fillRoundedRectangle (bar.withWidth (juce::jmax (w, load > 0.0 ? 3.0f : 0.0f)), 4.0f);
}

TopBar::TopBar (MixDocument& doc) : document (doc)
{
    logoMark.setText ("ON", juce::dontSendNotification);
    logoMark.setFont (juce::Font (juce::FontOptions (pt (12.0f), juce::Font::bold)));
    logoMark.setJustificationType (juce::Justification::centred);
    logoMark.setColour (juce::Label::backgroundColourId, Palette::brand);
    logoMark.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (logoMark);
    logoText.setText ("LiveMix", juce::dontSendNotification);
    logoText.setFont (juce::Font (juce::FontOptions (pt (20.0f), juce::Font::bold)));
    addAndMakeVisible (logoText);

    sessionName.setFont (juce::Font (juce::FontOptions (pt (15.0f), juce::Font::bold)));
    sessionName.setComponentID ("session-name");
    sessionName.setJustificationType (juce::Justification::centredLeft);
    sessionName.setMinimumHorizontalScale (1.0f);
    sessionName.setColour (juce::Label::backgroundColourId, Palette::card2);
    sessionName.setColour (juce::Label::outlineColourId, Palette::line);
    sessionName.setTooltip (ko ("열린 세션. 세션 메뉴에서 저장·열기"));
    addAndMakeVisible (sessionName);
    styleCaption (sessionState, "");
    sessionState.setComponentID ("session-state");
    addAndMakeVisible (sessionState);

    styleCaption (deviceLabel, "");
    deviceLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (deviceLabel);
    deviceCombo.setWantsKeyboardFocus (false);
    deviceCombo.setTextWhenNothingSelected (ko ("오디오 장치 없음"));
    deviceCombo.onChange = [this]
    {
        deviceCombo.settle();
        if (! refreshing && onDeviceChosen && deviceCombo.getSelectedId() > 0)
            onDeviceChosen (deviceCombo.getText());
    };
    deviceCombo.onRepick = [this]
    {
        if (! refreshing && onDeviceRepicked && deviceCombo.getSelectedId() > 0)
            onDeviceRepicked (deviceCombo.getText());
    };
    deviceCombo.beforeListOpens = [this]   // the devices that changed in the middle of a pick, before the list shows
    {
        if (waitingDevices && onDevicesWanted)
            onDevicesWanted();
    };
    addAndMakeVisible (deviceCombo);

    statusLabel.setFont (bodyFont (14.0f));
    statusLabel.setComponentID ("device-status");
    statusLabel.setMinimumHorizontalScale (1.0f);
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setColour (juce::Label::backgroundColourId, Palette::card2);
    statusLabel.setColour (juce::Label::outlineColourId, Palette::line);
    addAndMakeVisible (statusLabel);
    styleCaption (dspLabel, "CPU");

    for (auto* badge : { &micMuteBadge, &fxMuteBadge })
    {
        badge->setFont (juce::Font (juce::FontOptions (pt (12.5f), juce::Font::bold)));
        badge->setJustificationType (juce::Justification::centred);
        badge->setColour (juce::Label::textColourId, juce::Colours::white);
        badge->setColour (juce::Label::backgroundColourId, Palette::danger);
        addChildComponent (*badge);   // shown while its group is muted
    }

    micMuteBadge.setText (ko ("마이크 뮤트"), juce::dontSendNotification);
    fxMuteBadge.setText (ko ("FX 뮤트"), juce::dontSendNotification);
    addAndMakeVisible (dspLabel);
    addAndMakeVisible (dspMeter);

    auto button = [this] (juce::TextButton& b, const juce::String& text, std::function<void()> fn)
    {
        b.setButtonText (text);
        b.setWantsKeyboardFocus (false);
        b.onClick = std::move (fn);
        addAndMakeVisible (b);
    };

    button (fxButton, ko ("FX 채널"), [this] { if (onFxPanel) onFxPanel(); });
    button (pluginsButton, ko ("플러그인 관리"), [this] { if (onPluginManager) onPluginManager(); });
    pluginsButton.setTooltip (ko ("스캔한 플러그인의 사용 여부, VST2 스위치, 플러그인 프리셋"));

    refresh();
}

void TopBar::refresh()
{
    sessionName.setText (document.getDisplayName(), juce::dontSendNotification);
    sessionName.setTooltip (ko ("열린 세션. 세션 메뉴에서 저장·열기") + "\n" + document.getDisplayName());
    sessionState.setText (document.isDirty() ? ko ("저장 안 됨") : document.hasFile() ? ko ("저장됨") : ko ("아직 파일 없음"), juce::dontSendNotification);
    sessionState.setColour (juce::Label::textColourId, document.isDirty() ? Palette::meterYellow : Palette::dimText);
    resized();
}

void TopBar::setDevices (const juce::StringArray& names, const juce::String& current, const juce::String& typeName)
{
    // Not in the middle of a pick (the list open, or a pick whose change is on its way): a refill (a device unplugged
    // meanwhile) would give it to another device or lose it. The app asks again once the pick is through.
    waitingDevices = deviceCombo.busy();
    if (waitingDevices)
        return;

    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto label = AudioBackends::label (typeName);
    deviceLabel.setText (label.upToFirstOccurrenceOf (" ", false, false), juce::dontSendNotification);
    deviceLabel.setTooltip (label);
    deviceCombo.setTooltip (label + (current.isNotEmpty() ? "\n" + current : juce::String()));
    deviceCombo.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        deviceCombo.addRepickableItem (names[i], i + 1);

    const int index = names.indexOf (current);
    deviceCombo.setSelectedId (index >= 0 ? index + 1 : 0, juce::dontSendNotification);
    deviceCombo.settle();
    resized();
}

juce::String TopBar::buildStatusText (double sampleRate, int bufferSize, double latencyMs, bool running,
                                     const MixEngine::DeviceFormat& format, bool showSampleWord)
{
    if (! running) return ko ("오디오 멈춤");
    return juce::String (sampleRate / 1000.0, 1) + ko (" kHz · ")
        + (format.inputBits > 0 ? DeviceFormatText::bitDepth (format.inputBits, false) + ko (" · ") : juce::String())
        + juce::String (bufferSize) + (showSampleWord ? ko (" 샘플") : juce::String()) + "  " + juce::String (latencyMs, 1) + " ms";
}

void TopBar::setStatus (double sampleRate, int bufferSize, double latencyMs, double dspLoad, bool running, const MixEngine::DeviceFormat& format)
{
    const auto text = buildStatusText (sampleRate, bufferSize, latencyMs, running, format);
    const auto cpu = "CPU " + juce::String ((int) std::lround (dspLoad * 100.0)) + "%";
    const bool changed = fullStatusText != text || dspLabel.getText() != cpu;
    if (fullStatusText != text)
    {
        fullStatusText = text;
        shortStatusText = buildStatusText (sampleRate, bufferSize, latencyMs, running, format, false);
        minimalStatusText = buildStatusText (sampleRate, bufferSize, latencyMs, running, {}, false);
    }
    auto tooltip = fullStatusText;
    if (running && format.kind != MixEngine::DeviceFormat::Kind::none)
    {
        const bool shared = format.kind == MixEngine::DeviceFormat::Kind::windowsShared;
        tooltip += "\n" + DeviceFormatText::directions (format, true, document.getEngine().isMonitorRunning(), shared)
            + (shared ? ko (" (윈도우 설정)") : format.kind == MixEngine::DeviceFormat::Kind::asio ? ko (" (ASIO 드라이버)") : ko (" (독점)"));
    }
    statusLabel.setTooltip (tooltip);

    statusLabel.setColour (juce::Label::textColourId, running ? Palette::text : Palette::danger);
    dspMeter.load = dspLoad;
    dspLabel.setText (cpu, juce::dontSendNotification);
    if (changed) resized();
    dspMeter.repaint();
}

void TopBar::setFxCount (int count)
{
    fxButton.setButtonText (ko ("FX 채널") + (count > 0 ? "  " + juce::String (count) : juce::String()));
    resized();
}

void TopBar::setMuteGroups (bool micMuted, bool fxMuted)
{
    micMuteBadge.setVisible (micMuted);
    fxMuteBadge.setVisible (fxMuted);
    resized();
}

TopBar::Mode TopBar::modeFor (int width) const noexcept
{
    // Keep room for measured status text, a 120 px device, a 160 px session and both mute badges.
    // The badges never change the mode: a mute hotkey must not move the whole layout.
    return width >= 1220 ? Mode::wide : width >= 700 ? Mode::compact : Mode::narrow;
}

int TopBar::preferredHeight (int width) const noexcept
{
    switch (modeFor (width))
    {
        case Mode::wide:    return 64;
        case Mode::compact: return 64 + 42;
        case Mode::narrow:  return 64 + 42 * 2;
    }

    return 64;
}

void TopBar::resized()
{
    if (getWidth() <= 0) return;
    const auto mode = modeFor (getWidth());
    const int h = 34, gap = 8;
    const int rows = mode == Mode::wide ? 1 : mode == Mode::compact ? 2 : 3;
    auto area = getLocalBounds().reduced (16, 0);
    auto column = area.withSizeKeepingCentre (area.getWidth(), rows * h + (rows - 1) * gap);
    auto row1 = column.removeFromTop (h);
    column.removeFromTop (gap);
    auto row2 = rows >= 2 ? column.removeFromTop (h) : juce::Rectangle<int>();
    column.removeFromTop (gap);
    auto row3 = rows >= 3 ? column.removeFromTop (h) : juce::Rectangle<int>();

    const int stateWidth = labelWidthForText (sessionState, sessionState.getText());
    const int sessionMinimum = 160 + gap + stateWidth;
    const int sessionTarget = juce::jlimit (160, 320, labelWidthForText (sessionName, sessionName.getText()));
    const int typeWidth = labelWidthForText (deviceLabel, deviceLabel.getText());
    const int cpuWidth = labelWidthForText (dspLabel, dspLabel.getText());
    const int minimumStatus = labelWidthForText (statusLabel, minimalStatusText);
    const int badgeReservation = labelWidthForText (micMuteBadge, micMuteBadge.getText())
                               + labelWidthForText (fxMuteBadge, fxMuteBadge.getText()) + 2 * gap;
    const auto buttonWidth = [&] (juce::TextButton& button, int minimum)
    {
        const auto font = getLookAndFeel().getTextButtonFont (button, h);
        return juce::jmax (minimum, juce::GlyphArrangement::getStringWidthInt (font, button.getButtonText()) + 24);
    };
    const int fxWidth = buttonWidth (fxButton, 100), pluginsWidth = buttonWidth (pluginsButton, 120);
    auto& buttonRow = mode == Mode::narrow ? row2 : row1;
    pluginsButton.setBounds (buttonRow.removeFromRight (pluginsWidth));
    buttonRow.removeFromRight (gap);
    fxButton.setBounds (buttonRow.removeFromRight (fxWidth));
    buttonRow.removeFromRight (gap);

    auto& statusRow = mode == Mode::wide ? row1 : mode == Mode::compact ? row2 : row3;
    const int deviceMinimum = mode == Mode::narrow ? 0 : 120 + gap;
    int required = minimumStatus + badgeReservation + typeWidth + gap + deviceMinimum;
    if (mode == Mode::wide) required += 36 + sessionMinimum + gap;
    bool showCpu = mode != Mode::narrow;
    bool showMeter = showCpu;
    bool showLogo = true;
    if (showCpu) required += cpuWidth + gap;
    if (showMeter) required += 70 + gap;
    if (mode == Mode::wide) required += 84 + gap;
    // Keep the existing measured-text fallbacks at tight widths. Where all the reserved controls fit, separate
    // session | device/status/CPU | actions by 24 px (the 1440 px mockup), keeping 8 px inside each group.
    const int groupGap = mode == Mode::wide && required + 2 * (24 - gap) <= statusRow.getWidth() ? 24 : gap;
    if (mode == Mode::wide)
    {
        buttonRow.removeFromRight (groupGap - gap);
        required += groupGap - gap;
    }
    // Decorative widths yield only when even the shortest status and minimum device cannot fit.
    if (required > statusRow.getWidth() && showMeter) { showMeter = false; required -= 70 + gap; }
    if (mode == Mode::wide && required > statusRow.getWidth()) { showLogo = false; required -= 84 + gap; }
    if (required > statusRow.getWidth() && showCpu) { showCpu = false; required -= cpuWidth + gap; }
    const bool showType = required <= statusRow.getWidth();
    if (mode != Mode::wide) showLogo = row1.getWidth() >= 36 + 84 + gap + sessionMinimum;

    logoMark.setBounds (row1.removeFromLeft (28).reduced (0, 3));
    row1.removeFromLeft (gap);
    logoText.setVisible (showLogo);
    if (showLogo)
    {
        logoText.setBounds (row1.removeFromLeft (84));
        row1.removeFromLeft (gap);
    }
    int sessionWidth = sessionTarget;
    if (mode == Mode::wide)
    {
        // Only spare width goes to the name, after the device text, full status and both mute badges (shown or
        // not: a mute hotkey must not resize the name and push the rest along).
        // Keep the existing 160 px allowance before shortening status text; include the selector's text padding.
        const int deviceWidth = juce::jmax (160, juce::GlyphArrangement::getStringWidthInt (
            getLookAndFeel().getComboBoxFont (deviceCombo), deviceCombo.getText()) + 30 + 10 + 4);
        int reserved = stateWidth + gap + groupGap + deviceWidth + gap
                     + labelWidthForText (statusLabel, fullStatusText)
                     + (showType ? typeWidth + gap : 0)
                     + (showCpu ? cpuWidth + gap : 0) + (showMeter ? 70 + gap : 0) + badgeReservation;
        sessionWidth = juce::jlimit (160, sessionTarget, row1.getWidth() - reserved);
    }
    // off the wide layout the name keeps the rest of its row, as before 0.13.4
    auto session = mode == Mode::wide ? row1.removeFromLeft (juce::jmin (row1.getWidth(), sessionWidth + gap + stateWidth)) : row1;
    sessionState.setBounds (session.removeFromRight (stateWidth));
    session.removeFromRight (gap);
    sessionName.setBounds (session);
    if (mode == Mode::wide) row1.removeFromLeft (groupGap);

    dspMeter.setVisible (showMeter);
    dspLabel.setVisible (showCpu);
    if (showMeter)
    {
        dspMeter.setBounds (statusRow.removeFromRight (70));
        statusRow.removeFromRight (gap);
    }
    if (showCpu)
    {
        dspLabel.setBounds (statusRow.removeFromRight (cpuWidth));
        statusRow.removeFromRight (gap);
    }
    for (auto* badge : { &fxMuteBadge, &micMuteBadge })
        if (badge->isVisible())
        {
            badge->setBounds (statusRow.removeFromRight (labelWidthForText (*badge, badge->getText())).reduced (0, 5));
            statusRow.removeFromRight (gap);
        }

    deviceLabel.setVisible (showType);
    deviceLabel.setJustificationType (juce::Justification::centredLeft);
    if (showType)
    {
        deviceLabel.setBounds (statusRow.removeFromLeft (typeWidth));
        statusRow.removeFromLeft (gap);
    }
    // The device selector gives way to 160 px before the status drops anything (the bit depth and the word 샘플 are
    // worth more than the rest of a long endpoint name); it then takes whatever the status leaves.
    const int available = statusRow.getWidth() - (mode == Mode::narrow ? 0 : 160 + gap);
    const auto text = labelWidthForText (statusLabel, fullStatusText) <= available ? fullStatusText
                    : labelWidthForText (statusLabel, shortStatusText) <= available ? shortStatusText : minimalStatusText;
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setBounds (statusRow.removeFromRight (labelWidthForText (statusLabel, text)));
    if (mode == Mode::narrow)
        deviceCombo.setBounds (row2);
    else
    {
        statusRow.removeFromRight (gap);
        deviceCombo.setBounds (statusRow);
    }
}

void TopBar::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bar);
    g.setColour (Palette::line);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

} // namespace gocue::livemix
