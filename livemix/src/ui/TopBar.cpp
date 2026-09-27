#include "TopBar.h"
#include "AudioBackends.h"

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
    sessionName.setJustificationType (juce::Justification::centredLeft);
    sessionName.setMinimumHorizontalScale (1.0f);
    sessionName.setColour (juce::Label::backgroundColourId, Palette::card2);
    sessionName.setColour (juce::Label::outlineColourId, Palette::line);
    sessionName.setTooltip (ko ("열린 세션. 세션 버튼에서 저장·열기"));
    addAndMakeVisible (sessionName);
    styleCaption (sessionState, "");
    addAndMakeVisible (sessionState);

    styleCaption (deviceLabel, "");
    deviceLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (deviceLabel);
    deviceCombo.setWantsKeyboardFocus (false);
    deviceCombo.setTextWhenNothingSelected (ko ("오디오 장치 없음"));
    deviceCombo.onChange = [this]
    {
        if (! refreshing && onDeviceChosen && deviceCombo.getSelectedId() > 0)
            onDeviceChosen (deviceCombo.getText());
    };
    addAndMakeVisible (deviceCombo);

    statusLabel.setFont (bodyFont (14.0f));
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
    sessionState.setText (document.isDirty() ? ko ("저장 안 됨") : document.hasFile() ? ko ("저장됨") : ko ("아직 파일 없음"), juce::dontSendNotification);
    sessionState.setColour (juce::Label::textColourId, document.isDirty() ? Palette::meterYellow : Palette::dimText);
}

void TopBar::setDevices (const juce::StringArray& names, const juce::String& current, const juce::String& typeName)
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto label = AudioBackends::label (typeName);
    deviceLabel.setText (label.upToFirstOccurrenceOf (" ", false, false), juce::dontSendNotification);
    deviceLabel.setTooltip (label);
    deviceCombo.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        deviceCombo.addItem (names[i], i + 1);

    const int index = names.indexOf (current);
    deviceCombo.setSelectedId (index >= 0 ? index + 1 : 0, juce::dontSendNotification);
    resized();
}

void TopBar::setStatus (double sampleRate, int bufferSize, double latencyMs, double dspLoad, bool running)
{
    if (! running)
        statusLabel.setText (ko ("오디오 멈춤"), juce::dontSendNotification);
    else
        statusLabel.setText (juce::String (sampleRate / 1000.0, 1) + " kHz · " + juce::String (bufferSize) + ko (" 샘플") + "  " + juce::String (latencyMs, 1) + " ms",
                             juce::dontSendNotification);

    statusLabel.setColour (juce::Label::textColourId, running ? Palette::text : Palette::danger);
    dspMeter.load = dspLoad;
    dspLabel.setText ("CPU " + juce::String ((int) std::lround (dspLoad * 100.0)) + "%", juce::dontSendNotification);
    dspMeter.repaint();
}

void TopBar::setFxCount (int count)
{
    fxButton.setButtonText (ko ("FX 채널") + (count > 0 ? "  " + juce::String (count) : juce::String()));
}

void TopBar::setMuteGroups (bool micMuted, bool fxMuted)
{
    const int before = preferredHeight (getWidth());
    micMuteBadge.setVisible (micMuted);
    fxMuteBadge.setVisible (fxMuted);

    if (preferredHeight (getWidth()) != before && onHeightChanged)
        onHeightChanged();   // the owner gives the bar its new height (and lays everything out)
    else
        resized();
}

TopBar::Mode TopBar::modeFor (int width) const noexcept
{
    // one row: the logo (130), the two buttons (242), status (up to 230), device (160+), type (56), a session name of
    // at least 160 and room kept for both mute badges (200) - about 1220. The badges never change the mode: a mute
    // hotkey pressed mid-stream must not move the whole layout.
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

    logoMark.setBounds (row1.removeFromLeft (28).reduced (0, 3));
    row1.removeFromLeft (8);
    logoText.setBounds (row1.removeFromLeft (84));
    row1.removeFromLeft (10);

    if (mode == Mode::narrow)
    {
        // the two buttons share their own row
        auto r = row2;
        const int w = juce::jmax (40, (r.getWidth() - 6) / 2);
        fxButton.setBounds (r.removeFromLeft (w));
        r.removeFromLeft (6);
        pluginsButton.setBounds (r.removeFromLeft (w));
    }
    else
    {
        // the right end of the first row
        pluginsButton.setBounds (row1.removeFromRight (120));
        row1.removeFromRight (8);
        fxButton.setBounds (row1.removeFromRight (100));
        row1.removeFromRight (14);
    }

    // the device / status part: the same row in the wide bar, its own row below
    auto& statusRow = mode == Mode::wide ? row1 : mode == Mode::compact ? row2 : row3;
    const bool showCpu = mode != Mode::narrow;   // the narrow bar's device row has no room for it
    dspMeter.setVisible (showCpu);
    dspLabel.setVisible (showCpu);

    if (showCpu)
    {
        dspMeter.setBounds (statusRow.removeFromRight (70));
        statusRow.removeFromRight (6);
        dspLabel.setBounds (statusRow.removeFromRight (62));
        statusRow.removeFromRight (10);
    }

    // the mute badges: in the one-row bar next to the status (its width keeps their room); in the two- and three-row
    // bars at the right end of the first row, where only the session name gives - the device box keeps its width
    auto& badgeRow = mode == Mode::wide ? statusRow : row1;

    for (auto* badge : { &fxMuteBadge, &micMuteBadge })
        if (badge->isVisible())
        {
            badge->setBounds (badgeRow.removeFromRight (mode == Mode::narrow ? 76 : 92).reduced (0, 5));
            badgeRow.removeFromRight (8);
        }

    if (mode == Mode::wide)
    {
        deviceLabel.setJustificationType (juce::Justification::centredRight);
        statusLabel.setBounds (statusRow.removeFromRight (juce::jlimit (120, 230, statusRow.getWidth() / 4)));   // both give a little with two badges up: the session name keeps its 160
        statusRow.removeFromRight (8);
        deviceCombo.setBounds (statusRow.removeFromRight (juce::jlimit (120, 260, statusRow.getWidth() / 3)));
        statusRow.removeFromRight (6);
        deviceLabel.setBounds (statusRow.removeFromRight (labelWidthForText (deviceLabel, deviceLabel.getText())));
        statusRow.removeFromRight (10);
    }
    else
    {
        deviceLabel.setJustificationType (juce::Justification::centredLeft);
        deviceLabel.setBounds (statusRow.removeFromLeft (labelWidthForText (deviceLabel, deviceLabel.getText())));
        statusRow.removeFromLeft (6);
        statusLabel.setBounds (statusRow.removeFromRight (juce::jlimit (120, 230, statusRow.getWidth() * 2 / 5)));
        statusRow.removeFromRight (8);
        deviceCombo.setBounds (statusRow);
    }

    // the session name and state take what is left of the first row
    auto session = mode == Mode::wide ? row1.removeFromLeft (juce::jlimit (160, 320, row1.getWidth())) : row1;
    sessionName.setBounds (session.removeFromLeft (juce::jmax (100, session.getWidth() - 90)));
    session.removeFromLeft (8);
    sessionState.setBounds (session);
}

void TopBar::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bar);
    g.setColour (Palette::line);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

} // namespace gocue::livemix
