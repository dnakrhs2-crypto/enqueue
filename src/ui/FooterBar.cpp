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
    editButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    showButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    editButton.getProperties().set ("slateSegment", true);
    showButton.getProperties().set ("slateSegment", true);
    warningsButton.getProperties().set ("slatePill", true);
    warningsButton.getProperties().set ("slateSmall", true);
    warningsButton.getProperties().set ("slateColourOutline", true);
    warningsButton.setColour (juce::TextButton::buttonColourId, Palette::panel);
    warningsButton.setColour (juce::TextButton::textColourOffId, Palette::warn);
    warningsButton.setVisible (false);

    countLabel.setColour (juce::Label::textColourId, Palette::text);
    countLabel.setFont (Palette::font (Palette::headerSize, true));
    countLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (countLabel);

    modeHint.setColour (juce::Label::textColourId, Palette::dimText);
    modeHint.setFont (Palette::font (Palette::fileSize));
    modeHint.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (modeHint);

    audioStatus.setColour (juce::Label::textColourId, Palette::muted);
    audioStatus.setFont (Palette::monoFont (Palette::headerSize));
    audioStatus.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (audioStatus);
    for (auto* label : { &countLabel, &modeHint, &audioStatus })
    {
        label->setBorderSize (juce::BorderSize<int> (0));
        label->setMinimumHorizontalScale (1.0f);
    }

    setShowMode (false);
    setCueCount (0);
}

void FooterBar::setShowMode (bool mode)
{
    showMode = mode;
    editButton.setToggleState (! showMode, juce::dontSendNotification);
    showButton.setToggleState (showMode, juce::dontSendNotification);
    for (auto* button : { &editButton, &showButton })
    {
        button->setColour (juce::TextButton::buttonColourId, Palette::panel);
        button->setColour (juce::TextButton::buttonOnColourId, Palette::accent);
        button->setColour (juce::TextButton::textColourOffId, Palette::muted);
        button->setColour (juce::TextButton::textColourOnId, Palette::accentInk);
    }
    modeHint.setText (showMode ? ko ("쇼 모드: 편집 잠김 (Ctrl+Shift+M)") : ko ("편집 모드 · 쇼 모드 = Ctrl+Shift+M"), juce::dontSendNotification);
    modeHint.setTooltip (modeHint.getText());
    repaint();
}

void FooterBar::setCueCount (int count)
{
    const auto text = ko ("큐 ") + juce::String (count) + ko ("개");
    if (countLabel.getText() == text)
        return;
    countLabel.setText (text, juce::dontSendNotification);
    resized();
    repaint();
}

void FooterBar::setWarningCount (int count)
{
    const auto text = ko ("경고 ") + juce::String (count);
    if (warningsButton.getButtonText() == text && warningsButton.isVisible() == (count > 0))
        return;
    warningsButton.setVisible (count > 0);
    warningsButton.setButtonText (text);
    resized();
    repaint();
}

void FooterBar::setAudioStatus (juce::String text)
{
    if (audioStatus.getText() == text)
        return;
    audioStatus.setText (text, juce::dontSendNotification);
    audioStatus.setTooltip (text);
    resized();
}

void FooterBar::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;
    auto area = getLocalBounds().reduced (14, 3);
    modeBounds = area.removeFromLeft (148);
    auto modes = modeBounds;
    editButton.setBounds (modes.removeFromLeft (80));
    showButton.setBounds (modes);
    area.removeFromLeft (Palette::gap);
    const int countWidth = juce::GlyphArrangement::getStringWidthInt (countLabel.getFont(), countLabel.getText()) + 18;
    countLabel.setBounds (area.removeFromLeft (countWidth));
    area.removeFromLeft (Palette::gap);
    if (warningsButton.isVisible())
    {
        const int warningWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::headerSize, true), warningsButton.getButtonText()) + 18;
        warningsButton.setBounds (area.removeFromLeft (warningWidth));
        area.removeFromLeft (Palette::gap);
    }
    const int audioWidth = juce::GlyphArrangement::getStringWidthInt (audioStatus.getFont(), audioStatus.getText());
    audioStatus.setBounds (area.removeFromRight (juce::jmin (audioWidth, juce::jmax (0, area.getWidth()))));
    area.removeFromRight (juce::jmin (Palette::gap, area.getWidth()));
    modeHint.setBounds (area);
}

void FooterBar::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, 0.5f, (float) getWidth(), 0.5f);
    g.drawRoundedRectangle (countLabel.getBounds().toFloat().reduced (0.5f), Palette::pillRadius, Palette::borderWidth);
}

void FooterBar::paintOverChildren (juce::Graphics& g)
{
    g.setColour (Palette::outline);
    g.drawRoundedRectangle (modeBounds.toFloat().reduced (0.5f), Palette::cornerRadius, Palette::borderWidth);
}

} // namespace gocue
