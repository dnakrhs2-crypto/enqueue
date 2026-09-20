#include "ui/FooterBar.h"
#include "app/Commands.h"
#include "app/ShortcutDisplay.h"

#include "ui/UiUtils.h"

namespace gocue
{

ModeToggle::ModeToggle()
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

    editButton.onClick = [this] { if (onShowModeChanged) onShowModeChanged (false); };
    showButton.onClick = [this] { if (onShowModeChanged) onShowModeChanged (true); };
    editButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    showButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    editButton.getProperties().set ("slateSegment", true);
    showButton.getProperties().set ("slateSegment", true);
    setShowMode (false);
}

void ModeToggle::setShowMode (bool showMode)
{
    editButton.setToggleState (! showMode, juce::dontSendNotification);
    showButton.setToggleState (showMode, juce::dontSendNotification);
    for (auto* button : { &editButton, &showButton })
    {
        button->setColour (juce::TextButton::buttonColourId, Palette::panel);
        button->setColour (juce::TextButton::buttonOnColourId, Palette::accent);
        button->setColour (juce::TextButton::textColourOffId, Palette::muted);
        button->setColour (juce::TextButton::textColourOnId, Palette::accentInk);
    }
}

void ModeToggle::resized()
{
    modeBounds = getLocalBounds().reduced (Palette::gap, 3);
    auto modes = modeBounds;
    editButton.setBounds (modes.removeFromLeft (modes.getWidth() / 2));
    showButton.setBounds (modes);
}

void ModeToggle::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void ModeToggle::paintOverChildren (juce::Graphics& g)
{
    g.setColour (Palette::outline);
    g.drawRoundedRectangle (modeBounds.toFloat().reduced (0.5f), Palette::cornerRadius, Palette::borderWidth);
}

FooterBar::FooterBar()
{
    warningsButton.setButtonText (ko ("경고"));
    warningsButton.setWantsKeyboardFocus (false);
    warningsButton.onClick = [this] { if (onWarningsClicked) onWarningsClicked(); };
    addAndMakeVisible (warningsButton);
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
    midiStatus.setFont (Palette::font (Palette::fileSize));
    addAndMakeVisible (midiStatus);
    for (auto* label : { &countLabel, &modeHint, &audioStatus, &midiStatus })
    {
        label->setBorderSize (juce::BorderSize<int> (0));
        label->setMinimumHorizontalScale (1.0f);
    }

    setShowMode (false);
    setCueCount (0);
}

void FooterBar::setShowMode (bool mode, const ShortcutService* shortcuts)
{
    showMode = mode;
    const auto keys = ShortcutDisplay::currentKeys (shortcuts, CommandIDs::toggleShowMode);
    modeHint.setText (showMode ? ko ("쇼 모드: 편집 잠김 (") + keys + ")" : ko ("편집 모드 · 쇼 모드 = ") + keys, juce::dontSendNotification);
    modeHint.setTooltip (modeHint.getText());
    resized();
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

void FooterBar::setAudioStatus (juce::String text, bool warning, juce::String tooltip)
{
    if (tooltip.isEmpty())
        tooltip = text;

    if (audioStatus.getText() == text && audioWarning == warning && audioStatus.getTooltip() == tooltip)
        return;
    audioWarning = warning;
    audioStatus.setColour (juce::Label::textColourId, warning ? Palette::stopButton : Palette::muted);
    audioStatus.setText (text, juce::dontSendNotification);
    audioStatus.setTooltip (tooltip);
    resized();
}

void FooterBar::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;
    auto area = getLocalBounds().reduced (14, 3);
    const int countWidth = juce::GlyphArrangement::getStringWidthInt (countLabel.getFont(), countLabel.getText()) + 18;
    countLabel.setBounds (area.removeFromLeft (countWidth));
    area.removeFromLeft (Palette::gap);
    if (warningsButton.isVisible())
    {
        const int warningWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::headerSize, true), warningsButton.getButtonText()) + 18;
        warningsButton.setBounds (area.removeFromLeft (warningWidth));
        area.removeFromLeft (Palette::gap);
    }
    const int hintWidth = juce::GlyphArrangement::getStringWidthInt (modeHint.getFont(), modeHint.getText());
    modeHint.setBounds (area.removeFromLeft (juce::jmin (hintWidth, area.getWidth() / 2)));
    area.removeFromLeft (juce::jmin (Palette::gap, area.getWidth()));
    const int midiWidth = juce::GlyphArrangement::getStringWidthInt (midiStatus.getFont(), midiStatus.getText()) + 8;
    midiStatus.setBounds (area.removeFromLeft (juce::jmin (midiWidth, area.getWidth() / 2)));
    audioStatus.setBounds (area);
}

void FooterBar::setMidiStatus (const juce::String& text, const juce::String& tooltip, bool warning)
{
    if (midiStatus.getText() == text && midiStatus.getTooltip() == tooltip) return;
    midiStatus.setText (text, juce::dontSendNotification);
    midiStatus.setTooltip (tooltip);
    midiStatus.setColour (juce::Label::textColourId, warning ? Palette::warn : Palette::muted);
    resized();
}

void FooterBar::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, 0.5f, (float) getWidth(), 0.5f);
    const auto countBounds = countLabel.getBounds().toFloat().reduced (0.5f);
    g.drawRoundedRectangle (countBounds, Palette::pillRadius (countBounds), Palette::borderWidth);
}

} // namespace gocue
