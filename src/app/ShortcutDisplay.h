#pragma once
#include "app/ShortcutService.h"
#include "app/MidiInputService.h"

namespace gocue
{
/** One vocabulary for current keys in menus, hints, capture and search. */
namespace ShortcutDisplay
{
juce::String key (const juce::KeyPress&);
juce::String keys (const ShortcutKeys&, int maximum = 0);
juce::String currentKeys (const ShortcutService*, juce::CommandID, int maximum = 0, bool showRemainder = true);
juce::String deviceName (const ShortcutService&, const juce::String& identifier);
juce::String midi (const ShortcutService&, const MidiTriggers&, int maximum = 0);
juce::String midiDetails (const ShortcutService&, const MidiTrigger&);
juce::String currentInputs (const ShortcutService*, juce::CommandID);
juce::String deviceStatus (MidiInputService::Status);
juce::String midiState (const ShortcutService&, const MidiTrigger&, const std::vector<MidiInputService::Device>&);
juce::String midiSummary (const MidiInputService&, const ShortcutService* = nullptr);
}
}
