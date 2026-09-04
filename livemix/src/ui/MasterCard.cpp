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

    styleCaption (latencyCaption, ko ("지연"));
    addAndMakeVisible (latencyCaption);
    latencyValue.setFont (juce::Font (juce::FontOptions (pt (26.0f), juce::Font::bold)));
    latencyValue.setText ("-", juce::dontSendNotification);
    addAndMakeVisible (latencyValue);
    styleCaption (latencyNote, "");
    latencyNote.setFont (bodyFont (12.5f));
    addAndMakeVisible (latencyNote);

    styleCaption (outputCaption, ko ("메인 출력"));
    addAndMakeVisible (outputCaption);
    outputCombo.setWantsKeyboardFocus (false);
    outputCombo.onChange = [this]
    {
        if (! refreshing)
            document.setMasterOutput (juce::jmax (0, outputCombo.getSelectedId() - 1));
    };
    addAndMakeVisible (outputCombo);

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
}

void MasterCard::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    fillChannelCombo (outputCombo, outputNames, true, MixSession::maxDeviceChannels);
    outputCombo.setSelectedId (document.getSession().master.outputFirst + 1, juce::dontSendNotification);
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
    if (width < narrowBelow)
    {
        // stacked: head, chain, latency, output
        const int rows = ChipFlow::layout (chips, juce::Rectangle<int> (0, 0, juce::jmax (1, width - 28), 1), 30, false);
        return 24 + 34 + 6 + 20 + 12 + (22 + rows * ChipFlow::rowStep + 6 + 30) + 12 + (22 + 34) + 12 + (22 + 30 + 10 + 18 + 46);
    }

    // the chain column's width in resized() at this card width, then the rows the chips take in it
    const int afterHeadAndOut = width - 28 - 230 - 18 - 250 - 18;
    const int chainW = afterHeadAndOut - juce::jlimit (160, 300, afterHeadAndOut / 3) - 18;
    const int rows = ChipFlow::layout (chips, juce::Rectangle<int> (0, 0, juce::jmax (1, chainW), 1), 30, false);
    return juce::jmax (176, 24 + 22 + rows * ChipFlow::rowStep + 6 + 30);   // margins, caption, chips, gap, buttons
}

void MasterCard::resized()
{
    auto area = getLocalBounds().reduced (14, 12);

    if (getWidth() < narrowBelow)
    {
        // a tall, narrow window: the four blocks stack
        auto headRow = area.removeFromTop (34);
        badge.setBounds (headRow.removeFromLeft (32).reduced (0, 2));
        headRow.removeFromLeft (10);
        title.setBounds (headRow);
        area.removeFromTop (6);
        note.setBounds (area.removeFromTop (20));
        area.removeFromTop (12);

        chainCaption.setBounds (area.removeFromTop (22));
        const int rows = ChipFlow::layout (chips, area, 30, true);
        area.removeFromTop (rows * ChipFlow::rowStep + 6);
        auto buttons = area.removeFromTop (30);
        openChainButton.setBounds (buttons.removeFromLeft (92));
        buttons.removeFromLeft (8);
        addPluginButton.setBounds (buttons.removeFromLeft (76));
        area.removeFromTop (12);

        latencyCaption.setBounds (area.removeFromTop (22));
        auto lat = area.removeFromTop (34);
        latencyValue.setBounds (lat.removeFromLeft (juce::jmin (140, lat.getWidth() / 2)));
        lat.removeFromLeft (10);
        latencyNote.setBounds (lat);
        area.removeFromTop (12);

        outputCaption.setBounds (area.removeFromTop (22));
        outputCombo.setBounds (area.removeFromTop (30).withWidth (juce::jmin (220, area.getWidth())));
        area.removeFromTop (10);
        meterCaption.setBounds (area.removeFromTop (18));
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
