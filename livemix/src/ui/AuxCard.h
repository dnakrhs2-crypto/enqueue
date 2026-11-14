#pragma once

#include "ChannelCard.h"   // CardLayout
#include "Localisation.h"
#include "MixDocument.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace gocue::livemix
{

/** One aux channel: the sum of the mic channels that send to it (the chips here, or the AUX chips on their cards)
    through its own VST3 chain and ON/OFF switch, then pan, to the master and / or a device output pair. It has no
    device input or FX sends. The columns line up with the mic cards' (name, meter, chain, routing, output), so the two
    kinds read as one list. */
class AuxCard : public juce::Component
{
public:
    AuxCard (MixDocument& document, const juce::Uuid& auxId);
    ~AuxCard() override;

    const juce::Uuid& getAuxId() const noexcept { return auxId; }

    void refresh();
    /** Device output names for the direct output pair picker (empty = numbers only). */
    void setDeviceChannels (const juce::StringArray& outputNames);
    void setLayout (CardLayout layout);
    int getPreferredHeight (int width) const;
    void pushMeter (MixEngine::Meter meter, bool paint = true) { meter_.push (meter, paint); }

    std::function<void (const juce::Uuid&)> onOpenChain;
    std::function<void (const juce::Uuid&)> onAddPlugin;
    juce::Component& getAddPluginButton() noexcept { return addPluginButton; }
    std::function<void (const juce::Uuid&)> onRemove;
    std::function<void (const juce::Uuid&, int slotIndex)> onOpenPluginEditor;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct SenderChip;

    const MixAux* aux() const;
    int chainWidthForWidth (int width) const;
    int chainHeightForWidth (int width) const;
    int sendersWidthForWidth (int width) const;
    int sendersHeightForWidth (int width) const;
    void rebuildChain();
    void rebuildSenders();
    void commitOutput (bool pairSelected = false);
    void showMenu();

    MixDocument& document;
    juce::Uuid auxId;
    CardLayout layout = CardLayout::wide;
    juce::StringArray outputNames;

    juce::Label badge;
    NameLabel name;
    juce::TextButton menuButton { juce::String::fromUTF8 ("\xE2\x8B\xAF") };   // ···
    LampButton onButton;
    juce::Label meterCaption, chainCaption, sendersCaption, outputCaption, panCaption, panValue;
    MeterBar meter_;
    std::vector<std::unique_ptr<ChainChip>> chips;
    juce::TextButton openChainButton, addPluginButton;
    std::vector<std::unique_ptr<SenderChip>> senders;   // one per mic channel, in card order
    Chip masterChip, directChip;
    juce::ComboBox directCombo;
    PanSlider panSlider;
    static constexpr int chainFooter = 1 + 30;               // the chain actions under the chip rows
    static constexpr int meterHeight = 18 + 40;              // '미터' and the meter
    static constexpr int outputHeight = 26 + 34 + 4 + 34;    // caption, master / direct, pan - as on a mic card
    bool refreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuxCard)
};

} // namespace gocue::livemix
