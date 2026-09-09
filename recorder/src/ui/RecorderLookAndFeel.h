#pragma once
#include "../../../livemix/src/ui/LiveMixLookAndFeel.h"

namespace gocue::recorder
{
namespace Palette = gocue::livemix::Palette;
inline juce::String ko(const char* text) { return juce::String::fromUTF8(text); }
class RecorderLookAndFeel : public gocue::livemix::LiveMixLookAndFeel
{
public:
    RecorderLookAndFeel();
};
}
