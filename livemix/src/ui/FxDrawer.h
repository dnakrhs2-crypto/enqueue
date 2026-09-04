#pragma once

#include "MixDocument.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace gocue::livemix
{

/** The FX channels in a drawer: one tab per FX (double-click renames, ✕ removes, + adds), the selected FX's chain
    summary, return amount, outputs, meter and the mics that send to it. */
class FxDrawer : public juce::Component
{
public:
    explicit FxDrawer (MixDocument& document);
    ~FxDrawer() override;

    void refresh();
    void setDeviceChannels (const juce::StringArray& outputNames);
    void pushMeter (const juce::Uuid& fxId, MixEngine::Meter meter);
    juce::Uuid getSelectedFx() const noexcept { return selected; }
    /** The FX mute group's state: a member shows its return muted while it is on. */
    void setGroupMuted (bool muted);
    void selectFx (const juce::Uuid& id);

    std::function<void()> onClose;
    std::function<void (const juce::Uuid&)> onOpenChain;
    std::function<void (const juce::Uuid&)> onAddPlugin;
    /** The '+ 추가' button: the plugin menu opens next to it. */
    juce::Component& getAddPluginButton() noexcept { return addPluginButton; }
    std::function<void (const juce::Uuid&, int slotIndex)> onOpenPluginEditor;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void rebuildTabs();
    void rebuildChain();
    void rebuildSenders();
    void commitOutput();
    void removeSelected();
    const MixFx* fx() const;

    MixDocument& document;
    juce::Uuid selected = juce::Uuid::null();
    juce::StringArray outputNames;

    juce::Label title, chainCaption, returnCaption, returnValue, outputCaption, meterCaption, sendersCaption, note;
    juce::TextButton closeButton { juce::String::fromUTF8 ("\xE2\x9C\x95") };
    std::vector<std::unique_ptr<juce::TextButton>> tabs;
    juce::TextButton addFxButton, removeFxButton;
    NameLabel name;
    std::vector<std::unique_ptr<juce::TextButton>> chips;
    juce::TextButton openChainButton, addPluginButton;
    juce::Slider returnSlider;
    Chip masterChip, directChip, monoChip, muteGroupChip { ko ("뮤트그룹") };
    bool groupMuted = false;
    juce::ComboBox directCombo;
    MeterBar meter_ { true };
    std::vector<std::unique_ptr<juce::Label>> senders;
    bool refreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxDrawer)
};

} // namespace gocue::livemix
