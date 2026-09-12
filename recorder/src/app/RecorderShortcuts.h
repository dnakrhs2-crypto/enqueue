#pragma once
#include <juce_core/juce_core.h>
#include <array>

namespace gocue::recorder
{
enum class RecorderCommand { recordStart, recordStop, playStop, split, marker, count };
struct RecorderShortcuts
{
    static constexpr std::size_t count = std::size_t(RecorderCommand::count);
    // JUCE KeyPress::getTextDescription strings, also readable by older settings readers.
    std::array<juce::String, count> keys {"F9", "spacebar", "spacebar", "S", "M"};
    const juce::String& operator[](RecorderCommand command) const { return keys[std::size_t(command)]; }
    static juce::String name(RecorderCommand);
    static const char* field(RecorderCommand);
    juce::Result validate() const;
};
}
