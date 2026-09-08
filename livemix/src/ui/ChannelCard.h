#pragma once

#include "MixDocument.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <vector>

namespace gocue::livemix
{

/** How a card lays itself out for the window width (MainComponent decides). */
enum class CardLayout { wide, medium, narrow };   // 4 columns / 2 rows / 1 column

/** One mic channel: name (double-click edits), mic ON/OFF, input, VST3 chain summary, sends (one row per FX),
    outputs, pan, meter. Edits go through the document; the meter is pushed by the owner's timer. */
class ChannelCard : public juce::Component
{
public:
    ChannelCard (MixDocument& document, const juce::Uuid& channelId);
    ~ChannelCard() override;

    const juce::Uuid& getChannelId() const noexcept { return channelId; }

    /** Re-reads the model (names, switch, input, sends, outputs, pan, chain summary). */
    void refresh();
    /** Device channel names for the input / output pickers (empty = numbers only). */
    void setDeviceChannels (const juce::StringArray& inputNames, const juce::StringArray& outputNames);
    void setLayout (CardLayout layout);
    /** The height this card wants for the layout. */
    /** The height the card needs at 'width' in its current layout (the chip rows depend on the width). */
    int getPreferredHeight (int width) const;
    void pushMeter (MixEngine::Meter meter, bool paint = true) { meter_.push (meter, paint); }
    /** The mic mute group's state: a member shows itself muted while it is on. */
    void setGroupMuted (bool muted);

    std::function<void (const juce::Uuid&)> onOpenChain;      // "체인 열기"
    std::function<void (const juce::Uuid&)> onAddPlugin;      // "+ 추가"
    /** The '+ 추가' button: the plugin menu opens next to it. */
    juce::Component& getAddPluginButton() noexcept { return addPluginButton; }
    std::function<void (const juce::Uuid&)> onRemove;         // the ··· menu
    std::function<void (const juce::Uuid&, int slotIndex)> onOpenPluginEditor;   // a chain chip
    std::function<void (const juce::Uuid&)> onOpenPluginGroups;   // "플러그인 그룹": the groups window for this channel

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct SendRow;
    struct ChainChip;

    int chainRowsForWidth (int width) const;
    bool stackOutputControls (int width) const { return layout == CardLayout::narrow && width <= 420; }
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
    juce::Label inputCaption, chainCaption, fxCaption, outputCaption, panCaption, panValue, meterCaption;
    PanSlider panSlider;
    juce::ComboBox inputCombo;
    juce::ToggleButton stereoToggle;
    std::vector<std::unique_ptr<ChainChip>> chips;
    juce::Label chainArrows;
    juce::TextButton openChainButton, addPluginButton, pluginGroupsButton;
    juce::Label groupsCaption;
    std::array<std::unique_ptr<juce::TextButton>, (size_t) MixSession::maxPluginGroups> groupButtons;   // 1..5 under the chain: enabled once the group exists, lit (red) while it is off
    static constexpr int chainFooter = 6 + 26 + 6 + 30;   // under the chips: the groups row, then the buttons row
    std::vector<std::unique_ptr<SendRow>> sends;
    Chip masterChip, directChip, muteGroupChip { ko ("뮤트그룹") };
    juce::ComboBox directCombo;
    MeterBar meter_;
    bool refreshing = false;
    bool groupMuted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelCard)
};

} // namespace gocue::livemix
