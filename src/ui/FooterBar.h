#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{
class ShortcutService;

/** Menu bar's edit / show mode segment, sharing MainComponent's existing mode action. */
class ModeToggle : public juce::Component
{
public:
    ModeToggle();
    void setShowMode (bool showMode);
    std::function<void (bool showMode)> onShowModeChanged;
    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    juce::TextButton editButton, showButton;
    juce::Rectangle<int> modeBounds;
};

/** Bottom strip: cue count, broken-cue warnings, mode hint and audio status. */
class FooterBar : public juce::Component
{
public:
    FooterBar();

    void setShowMode (bool showMode, const ShortcutService* shortcuts = nullptr);
    void setCueCount (int count);
    /** Number of broken cues (0 hides the button). */
    void setWarningCount (int count);
    /** 'warning' paints the status in the stop colour (a clipped output or an xrun); 'tooltip' replaces the text as the tooltip. */
    void setAudioStatus (juce::String text, bool warning = false, juce::String tooltip = {});
    void setMidiStatus (const juce::String& text, const juce::String& tooltip, bool warning);

    std::function<void()> onWarningsClicked;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    juce::TextButton warningsButton;
    juce::Label countLabel, modeHint, audioStatus, midiStatus;
    bool showMode = false;
    bool audioWarning = false;
};

} // namespace gocue
