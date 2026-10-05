#include "ui/AutoLevelDialog.h"
#include "ui/ShortcutRouter.h"
#include "ui/UiUtils.h"

namespace gocue::AutoLevelDialog
{
namespace { juce::Component::SafePointer<juce::DialogWindow> dialog; }

Content::Content (ProjectDocument& doc, AudioEngine& e, std::function<double()> gainReader)
    : document (doc), engine (e), readGain (gainReader ? std::move (gainReader) : [&e] { return e.getAutoLevelGainDb(); })
{
    offButton.setButtonText (ko ("끄기"));
    onButton.setButtonText (ko ("켜기"));
    for (auto* button : { &offButton, &onButton })
    {
        // FooterBar's edit/show segments use this exact GoCueLookAndFeel shape and palette.
        button->getProperties().set ("slateSegment", true);
        button->setColour (juce::TextButton::buttonColourId, Palette::panel);
        button->setColour (juce::TextButton::buttonOnColourId, Palette::accent);
        button->setColour (juce::TextButton::textColourOffId, Palette::muted);
        button->setColour (juce::TextButton::textColourOnId, Palette::accentInk);
        button->setWantsKeyboardFocus (false);
        addAndMakeVisible (button);
    }
    offButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    onButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    offButton.onClick = [this] { enable (false); };
    onButton.onClick = [this] { enable (true); };
    targetLabel.setText (ko ("목표"), juce::dontSendNotification);
    correctionLabel.setText (ko ("보정"), juce::dontSendNotification);
    unitsLabel.setText ("LUFS", juce::dontSendNotification);
    for (auto* label : { &targetLabel, &correctionLabel, &unitsLabel, &gainLabel })
    {
        label->setFont (Palette::font (Palette::fieldLabelSize));
        label->setColour (juce::Label::textColourId, Palette::dimText);
        addAndMakeVisible (label);
    }
    gainLabel.setFont (Palette::monoFont (Palette::fieldValueSize));
    gainLabel.setJustificationType (juce::Justification::centredRight);
    gainLabel.setComponentID ("autoLevelGain");
    targetEditor.setComponentID ("autoLevelTarget");
    targetEditor.setFont (Palette::monoFont (Palette::fieldValueSize));
    targetEditor.setInputRestrictions (8, "-+.0123456789");
    targetEditor.setSelectAllWhenFocused (true);
    targetEditor.onReturnKey = [this] { commitTarget(); targetEditor.giveAwayKeyboardFocus(); };
    targetEditor.onFocusLost = [this] { commitTarget(); };
    addAndMakeVisible (targetEditor);
    document.addListener (this);
    documentStateChanged();
    setSize (360, 2 * (Palette::dialogInset + Palette::gap) + 2 * Palette::formRowHeight + Palette::fieldHeight);   // the same margin under the last field as above the first
    startTimerHz (30);
}

Content::~Content() { document.removeListener (this); }

void Content::enable (bool value)
{
    auto settings = document.settings;
    settings.autoLevelEnabled = value;
    document.setSettings (settings);
    engine.setAutoLevel (settings.autoLevelEnabled, settings.autoLevelTargetLufs);
}

void Content::commitTarget()
{
    const auto text = targetEditor.getText().trim();
    const auto number = text.trimCharactersAtStart ("-+");
    if (number.isNotEmpty() && number.containsAnyOf ("0123456789") && number.containsOnly ("0123456789.")
        && number.indexOfChar ('.') == number.lastIndexOfChar ('.') && text.length() - number.length() <= 1)
    {
        auto settings = document.settings;
        settings.autoLevelTargetLufs = text.getDoubleValue();
        settings.sanitise();
        if (settings.autoLevelTargetLufs != document.settings.autoLevelTargetLufs)
            document.setSettings (settings);
        engine.setAutoLevel (settings.autoLevelEnabled, settings.autoLevelTargetLufs);
    }
    targetEditor.setText (juce::String (document.settings.autoLevelTargetLufs, 1), false);
}

void Content::documentStateChanged()
{
    offButton.setToggleState (! document.settings.autoLevelEnabled, juce::dontSendNotification);
    onButton.setToggleState (document.settings.autoLevelEnabled, juce::dontSendNotification);
    if (! targetEditor.hasKeyboardFocus (true))
        targetEditor.setText (juce::String (document.settings.autoLevelTargetLufs, 1), false);
    refreshMeter();
}

void Content::refreshMeter()
{
    const bool on = document.settings.autoLevelEnabled;
    shownGain = on ? juce::jlimit (-20.0, 12.0, readGain()) : 0.0;
    gainLabel.setText (on ? (shownGain > 0.0 ? "+" : "") + juce::String (shownGain, 1) + " dB" : "0 dB", juce::dontSendNotification);
    gainLabel.setAlpha (on ? 1.0f : Palette::disabledAlpha);
    repaint (meterBounds);
}

void Content::resized()
{
    auto area = getLocalBounds().reduced (Palette::dialogInset + Palette::gap);
    auto row = area.removeFromTop (Palette::formRowHeight).withHeight (Palette::fieldHeight);
    offButton.setBounds (row.removeFromLeft (70));
    onButton.setBounds (row.removeFromLeft (70));
    row = area.removeFromTop (Palette::formRowHeight).withHeight (Palette::fieldHeight);
    targetLabel.setBounds (row.removeFromLeft (44));
    targetEditor.setBounds (row.removeFromLeft (84));
    unitsLabel.setBounds (row.removeFromLeft (48));
    row = area.removeFromTop (Palette::formRowHeight).withHeight (Palette::fieldHeight);
    correctionLabel.setBounds (row.removeFromLeft (44));
    gainLabel.setBounds (row.removeFromRight (82));
    meterBounds = row.reduced (4, 0);
}

void Content::paint (juce::Graphics& g)
{
    Palette::drawDialog (g, getLocalBounds());
    const float alpha = document.settings.autoLevelEnabled ? 1.0f : Palette::disabledAlpha;
    const auto track = meterBounds.toFloat().reduced (3.0f, 0.0f).withSizeKeepingCentre ((float) meterBounds.getWidth() - 6.0f, 4.0f);
    g.setColour (Palette::outline.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (track, Palette::colourBarRadius);
    const float zero = track.getX() + track.getWidth() * (20.0f / 32.0f);
    const float x = track.getX() + track.getWidth() * (float) ((shownGain + 20.0) / 32.0);
    // the correction as a bar from 0 dB: which way and how far at a glance; the dot marks the reading
    g.setColour (Palette::accent.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (zero, x), track.getY(), juce::jmax (zero, x), track.getBottom()),
                            Palette::colourBarRadius);
    g.setColour (Palette::dimText.withMultipliedAlpha (alpha));
    g.drawVerticalLine ((int) zero, track.getCentreY() - 5.0f, track.getCentreY() + 5.0f);
    g.setColour (Palette::accent.withMultipliedAlpha (alpha));
    g.fillEllipse (x - 3.0f, track.getCentreY() - 3.0f, 6.0f, 6.0f);
}

void Content::paintOverChildren (juce::Graphics& g)
{
    // the segment pair shares one outline drawn by its owner, like the menu bar's edit / show segment
    g.setColour (Palette::outline);
    g.drawRoundedRectangle (offButton.getBounds().getUnion (onButton.getBounds()).toFloat().reduced (0.5f),
                            Palette::cornerRadius, Palette::borderWidth);
}

void show (ProjectDocument& document, AudioEngine& engine, juce::Component* centreAround)
{
    if (dialog != nullptr) { dialog->toFront (true); return; }
    auto content = std::make_unique<Content> (document, engine);
    const auto size = content->getBounds();
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = ko ("자동 레벨 맞추기");
    options.content.setOwned (content.release());
    options.componentToCentreAround = centreAround;
    options.dialogBackgroundColour = Palette::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    dialog = options.launchAsync();
    if (dialog != nullptr)
    {
        // JUCE sizes the window for its own title bar and only then switches to the native one, leaving the content
        // 28 px too tall (an empty strip under the last row): give it back its own size, centred again
        dialog->setContentComponentSize (size.getWidth(), size.getHeight());
        dialog->centreAroundComponent (centreAround, dialog->getWidth(), dialog->getHeight());
    }
    ShortcutRouter::watchWindow (dialog.getComponent());
}

void closeIfOpen() { if (dialog != nullptr) delete dialog.getComponent(); }

} // namespace gocue::AutoLevelDialog
