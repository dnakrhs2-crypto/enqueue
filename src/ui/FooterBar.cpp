#include "ui/FooterBar.h"

#include "ui/UiUtils.h"

namespace gocue
{

FooterBar::FooterBar()
{
    auto setup = [this] (juce::TextButton& button, const char* text)
    {
        button.setButtonText (ko (text));
        button.setWantsKeyboardFocus (false);
        button.setClickingTogglesState (false);
        addAndMakeVisible (button);
    };

    setup (editButton, "편집 모드");
    setup (showButton, "쇼 모드");
    setup (warningsButton, "경고");

    editButton.onClick = [this] { if (onShowModeChanged) onShowModeChanged (false); };
    showButton.onClick = [this] { if (onShowModeChanged) onShowModeChanged (true); };
    warningsButton.onClick = [this] { if (onWarningsClicked) onWarningsClicked(); };
    warningsButton.setColour (juce::TextButton::buttonColourId, Palette::fadingOut);
    warningsButton.setColour (juce::TextButton::textColourOffId, Palette::onBright);
    warningsButton.setVisible (false);

    countLabel.setColour (juce::Label::textColourId, Palette::dimText);
    countLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (countLabel);

    modeHint.setColour (juce::Label::textColourId, Palette::dimText);
    modeHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    modeHint.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (modeHint);

    setShowMode (false);
    setCueCount (0);
}

void FooterBar::setShowMode (bool mode)
{
    showMode = mode;
    editButton.setColour (juce::TextButton::buttonColourId, showMode ? Palette::rowEven : Palette::standby.darker (0.3f));
    showButton.setColour (juce::TextButton::buttonColourId, showMode ? Palette::stopButton : Palette::rowEven);
    modeHint.setText (showMode ? ko ("쇼 모드: 편집 잠김 (Ctrl+Shift+M)") : juce::String(), juce::dontSendNotification);
    repaint();
}

void FooterBar::setCueCount (int count)
{
    countLabel.setText (ko ("큐 ") + juce::String (count) + ko ("개"), juce::dontSendNotification);
}

void FooterBar::setWarningCount (int count)
{
    warningsButton.setVisible (count > 0);
    warningsButton.setButtonText (ko ("경고 ") + juce::String (count));
}

void FooterBar::resized()
{
    auto area = getLocalBounds().reduced (8, 3);
    editButton.setBounds (area.removeFromLeft (90));
    area.removeFromLeft (4);
    showButton.setBounds (area.removeFromLeft (80));
    area.removeFromLeft (12);
    countLabel.setBounds (area.removeFromLeft (110));
    warningsButton.setBounds (area.removeFromRight (90));
    area.removeFromRight (8);
    modeHint.setBounds (area);
}

void FooterBar::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, 0.5f, (float) getWidth(), 0.5f);
}

} // namespace gocue
