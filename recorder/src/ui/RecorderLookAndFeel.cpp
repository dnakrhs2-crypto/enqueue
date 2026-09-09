#include "RecorderLookAndFeel.h"

namespace gocue::recorder
{
RecorderLookAndFeel::RecorderLookAndFeel()
{
    setDefaultSansSerifTypefaceName("Malgun Gothic");
    setColour(juce::DocumentWindow::backgroundColourId, Palette::background);
}
}
