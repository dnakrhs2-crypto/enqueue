#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{
class ShortcutService;

/** Menu bar's right end: the edit / show mode segment, then the full screen button at the far right. Both only
    call MainComponent's existing actions (show mode, the full screen command). */
class ModeToggle : public juce::Component
{
public:
    ModeToggle();
    void setShowMode (bool showMode);
    /** "전체 화면", or "전체 화면 종료" while the main window is full screen; 'keys' is the command's current
        shortcut text for the tooltip (empty: none). */
    void setFullScreen (bool fullScreen, const juce::String& keys);
    /** Room for the segment and for the longer of the button's two labels, so the button never changes width when
        it toggles. */
    int getIdealWidth() const;
    std::function<void (bool showMode)> onShowModeChanged;
    std::function<void()> onFullScreenClicked;
    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    juce::TextButton editButton, showButton, fullScreenButton;
    juce::Rectangle<int> modeBounds;
};

/** Owns the status controls: this strip lives in ContainerTabs; audio lives in the menu bar. */
class FooterBar : public juce::Component
{
public:
    FooterBar();
    void attachAudioStatus (juce::Component& host);
    void setAudioBounds (juce::Rectangle<int> bounds);
    int getPreferredWidth() const;
    /** The cue count and, when shown, the broken-cue warnings button: the part that never gives way. */
    int getEssentialWidth() const;

    void setShowMode (bool showMode, const ShortcutService* shortcuts = nullptr);
    void setCueCount (int count);
    /** Number of broken cues (0 hides the button). */
    void setWarningCount (int count);
    /** 'warning' paints the status in the stop colour (a clipped output or an xrun); 'tooltip' replaces the text as the tooltip. */
    void setAudioStatus (juce::String text, bool warning = false, juce::String tooltip = {});
    void setAudioStatusParts (juce::String device, juce::StringArray settings, juce::StringArray measurements,
                              bool warning = false, juce::String tooltip = {});
    void setMidiStatus (const juce::String& text, const juce::String& tooltip, bool warning);

    std::function<void()> onWarningsClicked;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct AudioStatusLabel : public juce::Label
    {
        void paint (juce::Graphics&) override;
        juce::String device;
        juce::StringArray settings, measurements;
    };
    void updateLayout();
    juce::TextButton warningsButton;
    juce::Label countLabel, modeHint, midiStatus;
    AudioStatusLabel audioStatus;
    bool showMode = false;
    bool audioWarning = false;
};

} // namespace gocue
