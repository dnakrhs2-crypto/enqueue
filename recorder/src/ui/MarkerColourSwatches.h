#pragma once
#include "RecorderLookAndFeel.h"

namespace gocue::recorder
{
class MarkerColourSwatches final : public juce::Component
{
public:
    MarkerColourSwatches();
    void setSelected(const juce::String& hex);
    juce::String selected() const { return selectedColour; }
    void resized() override;
    int heightForWidth(int width) const;
    std::function<void(const juce::String&)> onPick;
private:
    juce::String selectedColour;
    juce::OwnedArray<juce::Button> buttons;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MarkerColourSwatches)
};
}
