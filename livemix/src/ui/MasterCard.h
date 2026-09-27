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
    enum class ObsStatus { waiting, connected, audioStopped, installNeeded, restartObs };
    void setObsStatus (ObsStatus status);
    void setObsInstalling (bool installing);
    std::function<void()> onObsEnabled; // document/sender are already enabled when this runs

    std::function<void()> onOpenChain;
    std::function<void()> onAddPlugin;
    /** The LUFS button: the loudness meter window. */
    std::function<void()> onOpenLoudness;
    /** The '+ 추가' button: the plugin menu opens next to it. */
    juce::Component& getAddPluginButton() noexcept { return addPluginButton; }
    std::function<void (int slotIndex)> onOpenPluginEditor;

    void resized() override;
    /** The height the card needs at 'width': the columns' height (more when the chips take more than two rows),
        the compact stack's height below narrowBelow, or the strip's when folded. */
    int getPreferredHeight (int width) const;
    /** The height of the columns or the compact stack at 'width', whether or not the card is folded right now. */
    int getUnfoldedHeight (int width) const;
    static constexpr int narrowBelow = 988;    // narrower than this: the compact stack (head with the output pair, the chain row, the meter row) - 988 = the top bar's one-row width less the card margins, so both change at once
    static constexpr int stripHeight = 58;     // folded: one row - badge, title, meter, chain button, output pair
    /** Folded to one row: a short window gives the mics their room and keeps the master's meter and output in view. */
    void setStrip (bool folded);
    bool isStrip() const noexcept { return strip; }
    void paint (juce::Graphics& g) override;

private:
    struct Chip;
    void rebuildChain();
    int obsToggleWidth() const;
    void refreshObsStatus();
    void refreshObsToggle();
    static constexpr int obsRowHeight = 28, obsRowGap = 8;

    MixDocument& document;
    juce::StringArray outputNames;
    juce::Label badge, title, note, chainCaption, latencyCaption, latencyValue, latencyNote, compactLatency, outputCaption, meterCaption;
    std::vector<std::unique_ptr<juce::TextButton>> chips;
    juce::TextButton openChainButton, addPluginButton, lufsButton;
    juce::ComboBox outputCombo;
    juce::ToggleButton obsToggle;
    juce::Label obsStatusLabel;
    ObsStatus obsStatus = ObsStatus::waiting;
    bool obsInstalling = false;
    MeterBar meter_ { true };
    bool refreshing = false;
    bool strip = false;
};

} // namespace gocue::livemix
