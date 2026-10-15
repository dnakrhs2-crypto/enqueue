#pragma once

#include "LiveMixSettings.h"
#include "MixEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::livemix
{

/** LUFS 미터: the master output's loudness (ITU-R BS.1770-4 / EBU R128) in a window of its own - momentary, short-term,
    integrated, loudness range, true peak, a bar against a target and the last minutes' short-term curve, like the
    loudness meter plugins. Opened from the master card's LUFS button (and the 설정 menu); hidden on close, kept. */
class LoudnessWindow : public juce::DocumentWindow
{
public:
    LoudnessWindow (MixEngine& engine, LiveMixSettings& settings);
    ~LoudnessWindow() override;

    void open();
    void closeButtonPressed() override;

private:
    class Content;
    Content* content = nullptr;
    juce::TooltipWindow tooltips { this, 700 };   // LiveMix has no app-wide one: this window's tips

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessWindow)
};

} // namespace gocue::livemix
