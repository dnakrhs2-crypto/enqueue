#pragma once

#include "MixDocument.h"
#include "ObsPluginInstaller.h"
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
    enum class ObsStatus { waiting, connected, audioStopped, installNeeded, restartObs, sendFailed, addSource, updateObs, portableObs };
    struct ObsAdvice { ObsStatus status; juce::String text, reason; };
    static ObsAdvice obsStatusFor (const juce::String& sendError, bool pluginCurrent, bool restartNeeded, bool deviceRunning,
                                   ObsSender::ReaderState reader, const std::vector<ObsPluginInstaller::RunningObs>& runningObs);
    static juce::String obsStatusText (ObsStatus);
    void setObsStatus (ObsStatus status, const juce::String& reason = {});
    void setObsInstalling (bool installing);
    std::function<void()> onObsEnabled; // document/sender are already enabled when this runs
    std::function<void()> onObsInstallRequested;
    void mouseUp (const juce::MouseEvent&) override;

    std::function<void()> onOpenChain;
    std::function<void()> onAddPlugin;
    /** The LUFS button: the loudness meter window. */
    std::function<void()> onOpenLoudness;
    /** The '+ 추가' button: the plugin menu opens next to it. */
    juce::Component& getAddPluginButton() noexcept { return addPluginButton; }
    std::function<void (int slotIndex)> onOpenPluginEditor;

    void resized() override;
    /** The height the card needs at 'width': the columns' height (including wrapped chips and actions),
        the compact stack's height below narrowBelow, or the strip's when folded. */
    int getPreferredHeight (int width) const;
    /** The height of the columns or the compact stack at 'width', whether or not the card is folded right now. */
    int getUnfoldedHeight (int width) const;
    static constexpr int narrowBelow = 760;    // below this: the compact stack; otherwise two tiers until wideBelow
    static constexpr int wideBelow = 1385;     // the five signal-flow columns, matching the channel cards
    static constexpr int stripHeight = 58;     // folded: one row - badge, title, meter, chain button, output pair
    /** Folded to one row: a short window gives the mics their room and keeps the master's meter and output in view. */
    void setStrip (bool folded);
    bool isStrip() const noexcept { return strip; }
    void paint (juce::Graphics& g) override;

private:
    struct Chip;
    void rebuildChain();
    int obsToggleWidth() const;
    int obsHintHeight (int width) const;
    int obsControlsHeight (int width, int hintGap) const;
    struct ChainLayout
    {
        juce::Rectangle<int> caption, open, add;
        std::vector<juce::Rectangle<int>> chips;
        int bottom = 0;
    };
    ChainLayout measureChain (int width) const;
    static bool mediumLatencyFitsBesideChain (int width);
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
    juce::Label obsStatusLabel, obsSourceHint;
    ObsStatus obsStatus = ObsStatus::waiting;
    juce::String obsStatusReason;
    bool obsInstalling = false;
    MeterBar meter_ { true };
    bool refreshing = false;
    bool strip = false;
};

} // namespace gocue::livemix
