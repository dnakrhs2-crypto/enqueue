#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{

/** Bottom strip: edit / show mode toggle, cue count, broken-cue warnings. */
class FooterBar : public juce::Component
{
public:
    FooterBar();

    void setShowMode (bool showMode);
    void setCueCount (int count);
    /** Number of broken cues (0 hides the button). */
    void setWarningCount (int count);

    std::function<void (bool showMode)> onShowModeChanged;
    std::function<void()> onWarningsClicked;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    juce::TextButton editButton, showButton, warningsButton;
    juce::Label countLabel, modeHint;
    bool showMode = false;
};

} // namespace gocue
