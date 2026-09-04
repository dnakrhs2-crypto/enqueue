#pragma once

#include "MixDocument.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace gocue::livemix
{

/** How a card lays itself out for the window width (MainComponent decides). */
enum class CardLayout { wide, medium, narrow };   // 4 columns / 2 rows / 1 column

/** One mic channel: name (double-click edits), mic ON/OFF, input, VST3 chain summary, sends (one row per FX),
    outputs, meter. Edits go through the document; the meter is pushed by the owner's timer. */
class ChannelCard : public juce::Component
{
public:
    ChannelCard (MixDocument& document, const juce::Uuid& channelId);
    ~ChannelCard() override;

    const juce::Uuid& getChannelId() const noexcept { return channelId; }

    /** Re-reads the model (names, switch, input, sends, outputs, chain summary). */
    void refresh();
    /** Device channel names for the input / output pickers (empty = numbers only). */
    void setDeviceChannels (const juce::StringArray& inputNames, const juce::StringArray& outputNames);
    void setLayout (CardLayout layout);
    /** The height this card wants for the layout. */
    /** The height the card needs at 'width' in its current layout (the chip rows depend on the width). */
    int getPreferredHeight (int width) const;
    void pushMeter (MixEngine::Meter meter) { meter_.push (meter); }

    std::function<void (const juce::Uuid&)> onOpenChain;      // "체인 열기"
    std::function<void (const juce::Uuid&)> onAddPlugin;      // "+ 추가"
    /** The '+ 추가' button: the plugin menu opens next to it. */
    juce::Component& getAddPluginButton() noexcept { return addPluginButton; }
    std::function<void (const juce::Uuid&)> onRemove;         // the ··· menu
    std::function<void (const juce::Uuid&, int slotIndex)> onOpenPluginEditor;   // a chain chip

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct SendRow;
    struct ChainChip;

    int chainRowsForWidth (int width) const;
    void rebuildSends();
    void rebuildChain();
    void commitInput();
    void commitOutput();
    void showMenu();
    const MixChannel* channel() const;

    MixDocument& document;
    juce::Uuid channelId;
    CardLayout layout = CardLayout::wide;
    juce::StringArray inputNames, outputNames;

    juce::Label number;
    NameLabel name;
    juce::TextButton menuButton { juce::String::fromUTF8 ("\xE2\x8B\xAF") };   // ···
    LampButton micButton;
    juce::Label inputCaption, chainCaption, fxCaption, outputCaption, meterCaption;
    juce::ComboBox inputCombo;
    juce::ToggleButton stereoToggle;
    std::vector<std::unique_ptr<ChainChip>> chips;
    juce::Label chainArrows;
    juce::TextButton openChainButton, addPluginButton;
    std::vector<std::unique_ptr<SendRow>> sends;
    Chip masterChip, directChip;
    juce::ComboBox directCombo;
    MeterBar meter_;
    bool refreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelCard)
};

} // namespace gocue::livemix
