#include "TopBar.h"

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

    styleCaption (asioLabel, "ASIO");
    asioLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (asioLabel);
    deviceCombo.setWantsKeyboardFocus (false);
    deviceCombo.setTextWhenNothingSelected (ko ("ASIO 장치 없음"));
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
    addAndMakeVisible (dspLabel);
    addAndMakeVisible (dspMeter);

    auto button = [this] (juce::TextButton& b, const juce::String& text, std::function<void()> fn)
    {
        b.setButtonText (text);
        b.setWantsKeyboardFocus (false);
        b.onClick = std::move (fn);
        addAndMakeVisible (b);
    };

    button (sessionButton, ko ("세션"), [this] { if (onSessionMenu) onSessionMenu (&sessionButton); });
    button (fxButton, ko ("FX 채널"), [this] { if (onFxPanel) onFxPanel(); });
    button (backupButton, ko ("온라인 백업"), [this] { if (onBackup) onBackup(); });
    backupButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
    backupButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    button (settingsButton, ko ("설정"), [this] { if (onSettings) onSettings(); });
    button (helpButton, ko ("정보"), [this] { if (onHelpMenu) onHelpMenu (&helpButton); });
    helpButton.setTooltip (ko ("커뮤니티 · 업데이트 확인 · 정보"));

    refresh();
}

void TopBar::refresh()
{
    sessionName.setText (document.getDisplayName(), juce::dontSendNotification);
    sessionState.setText (document.isDirty() ? ko ("저장 안 됨") : document.hasFile() ? ko ("저장됨") : ko ("아직 파일 없음"), juce::dontSendNotification);
    sessionState.setColour (juce::Label::textColourId, document.isDirty() ? Palette::meterYellow : Palette::dimText);
}

void TopBar::setDevices (const juce::StringArray& names, const juce::String& current)
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    deviceCombo.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        deviceCombo.addItem (names[i], i + 1);

    const int index = names.indexOf (current);
    deviceCombo.setSelectedId (index >= 0 ? index + 1 : 0, juce::dontSendNotification);
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

void TopBar::resized()
{
    auto area = getLocalBounds().reduced (16, 0);
    const int h = 34;
    auto row = area.withSizeKeepingCentre (area.getWidth(), h);

    logoMark.setBounds (row.removeFromLeft (28).reduced (0, 3));
    row.removeFromLeft (8);
    logoText.setBounds (row.removeFromLeft (84));
    row.removeFromLeft (10);

    settingsButton.setBounds (row.removeFromRight (64));
    row.removeFromRight (8);
    helpButton.setBounds (row.removeFromRight (64));
    row.removeFromRight (8);
    backupButton.setBounds (row.removeFromRight (100));
    row.removeFromRight (8);
    fxButton.setBounds (row.removeFromRight (100));
    row.removeFromRight (8);
    sessionButton.setBounds (row.removeFromRight (64));
    row.removeFromRight (14);

    const bool wide = row.getWidth() > 900;
    dspMeter.setVisible (wide);
    dspLabel.setVisible (wide);

    if (wide)
    {
        dspMeter.setBounds (row.removeFromRight (70));
        row.removeFromRight (6);
        dspLabel.setBounds (row.removeFromRight (62));
        row.removeFromRight (10);
    }

    statusLabel.setBounds (row.removeFromRight (juce::jmin (230, row.getWidth() / 3)));
    row.removeFromRight (8);
    deviceCombo.setBounds (row.removeFromRight (juce::jlimit (160, 260, row.getWidth() / 3)));
    row.removeFromRight (6);
    asioLabel.setBounds (row.removeFromRight (40));
    row.removeFromRight (10);

    auto session = row.removeFromLeft (juce::jlimit (160, 320, row.getWidth()));
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
