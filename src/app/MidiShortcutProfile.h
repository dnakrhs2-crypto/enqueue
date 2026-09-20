#pragma once

#include "model/MidiTrigger.h"
#include <map>
#include <optional>

namespace gocue
{
struct MidiShortcutProfileParseResult;
struct MidiShortcutProfile
{
    std::map<juce::String, MidiTriggers> overrides;
    // Display hints only. Import never selects or opens a device.
    std::map<juce::String, juce::String> deviceNames;
    juce::Result validate() const;
    juce::Result serialise (juce::String&) const;
    static MidiShortcutProfileParseResult parse (const juce::String&);
    bool operator== (const MidiShortcutProfile& b) const { return overrides == b.overrides && deviceNames == b.deviceNames; }
};
struct MidiShortcutProfileParseResult
{
    juce::Result status = juce::Result::ok();
    juce::String originalXml;
    MidiShortcutProfile profile;
    bool wasOk() const { return status.wasOk(); }
};
struct MidiInputSettings
{
    std::map<juce::String, juce::String> selected; // identifier -> last display name; absent devices retained
    bool autoUseAll = false, allowBackgroundPlayback = true;
    juce::Result validate() const;
    juce::Result serialise (juce::String&) const;
    static juce::Result parse (const juce::String&, MidiInputSettings&);
    bool operator== (const MidiInputSettings& b) const { return selected == b.selected && autoUseAll == b.autoUseAll && allowBackgroundPlayback == b.allowBackgroundPlayback; }
};
/** Only supplied pairs change. A combined import writes both profiles in one disk transaction. */
struct InputSettingsTransaction
{
    struct Pair { juce::String current, lastGood; };
    std::optional<Pair> keyboard, midi, devices;
};
}
