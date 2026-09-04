#pragma once

#include "MixDocument.h"
#include "Widgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::livemix
{

/** The master: chain summary, latency, main output pair, L/R meter. Docked under the channel list. */
class MasterCard : public juce::Component
{
public:
    explicit MasterCard (MixDocument& document);

    void refresh();
    void setDeviceChannels (const juce::StringArray& outputNames);
    void setLatency (double ms, int bufferSize, double sampleRate);
    void pushMeter (MixEngine::Meter meter) { meter_.push (meter); }

    std::function<void()> onOpenChain;
    std::function<void()> onAddPlugin;
    /** The '+ 추가' button: the plugin menu opens next to it. */
    juce::Component& getAddPluginButton() noexcept { return addPluginButton; }
    std::function<void (int slotIndex)> onOpenPluginEditor;

    void resized() override;
    /** The height the card needs at 'width': its usual height, more when the chips take more than two rows; the
        stacked (portrait) layout's height below narrowBelow. */
    int getPreferredHeight (int width) const;
    static constexpr int narrowBelow = 760;   // narrower than this: head, chain, latency and output stack up
    void paint (juce::Graphics& g) override;

private:
    struct Chip;
    void rebuildChain();

    MixDocument& document;
    juce::StringArray outputNames;
    juce::Label badge, title, note, chainCaption, latencyCaption, latencyValue, latencyNote, outputCaption, meterCaption;
    std::vector<std::unique_ptr<juce::TextButton>> chips;
    juce::TextButton openChainButton, addPluginButton;
    juce::ComboBox outputCombo;
    MeterBar meter_ { true };
    bool refreshing = false;
};

} // namespace gocue::livemix
