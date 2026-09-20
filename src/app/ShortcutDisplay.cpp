#include "app/ShortcutDisplay.h"
#include <set>

namespace gocue::ShortcutDisplay
{
juce::String key (const juce::KeyPress& input)
{
    const auto normal = ShortcutKeyCodec::normalise (input);
    const auto mods = normal.getModifiers();
    return juce::String (mods.isCtrlDown() ? "Ctrl+" : "")
         + (mods.isAltDown() ? "Alt+" : "") + (mods.isShiftDown() ? "Shift+" : "")
         + ShortcutKeyCodec::keyName (normal);
}

juce::String keys (const ShortcutKeys& values, int maximum)
{
    if (values.isEmpty())
        return juce::String::fromUTF8 ("미지정");
    const int count = maximum > 0 ? juce::jmin (maximum, values.size()) : values.size();
    juce::StringArray text;
    for (int i = 0; i < count; ++i)
        text.add (key (values[i]));
    auto result = text.joinIntoString (", ");
    if (count < values.size())
        result += " +" + juce::String (values.size() - count) + juce::String::fromUTF8 ("개");
    return result;
}

juce::String currentKeys (const ShortcutService* service, juce::CommandID id, int maximum, bool showRemainder)
{
    const auto* entry = ShortcutCatalog::get().find (id);
    const auto values = service != nullptr ? service->getKeys (id) : entry != nullptr ? entry->defaultKeys : ShortcutKeys();
    if (! showRemainder && maximum > 0)
    {
        ShortcutKeys shown;
        for (int i = 0; i < juce::jmin (maximum, values.size()); ++i) shown.add (values[i]);
        return keys (shown);
    }
    return keys (values, maximum);
}

juce::String deviceName (const ShortcutService& service, const juce::String& identifier)
{
    if (identifier == "any") return juce::String::fromUTF8 ("허용 장치 모두");
    for (const auto* names : { &service.getAvailableMidiDevices(), &service.getMidiInputSettings().selected, &service.getMidiProfile().deviceNames })
        if (const auto found = names->find (identifier); found != names->end() && found->second.isNotEmpty()) return found->second;
    return identifier;
}
juce::String midi (const ShortcutService& service, const MidiTriggers& triggers, int maximum)
{
    if (triggers.empty()) return juce::String::fromUTF8 ("미지정");
    const auto count = maximum > 0 ? juce::jmin (static_cast<size_t> (maximum), triggers.size()) : triggers.size();
    juce::StringArray texts;
    for (size_t i = 0; i < count; ++i) texts.add (triggers[i].display (deviceName (service, triggers[i].source)));
    auto text = texts.joinIntoString (" / ");
    if (count < triggers.size()) text += " +" + juce::String (static_cast<int> (triggers.size() - count));
    return text;
}
juce::String currentInputs (const ShortcutService* service, juce::CommandID id)
{
    auto text = currentKeys (service, id);
    if (service != nullptr)
        if (const auto* entry = ShortcutCatalog::get().find (id); entry != nullptr && ! service->getMidiTriggers (entry->id).empty())
            text += "\nMIDI: " + midi (*service, service->getMidiTriggers (entry->id));
    return text;
}
juce::String midiDetails (const ShortcutService& service, const MidiTrigger& trigger)
{
    auto text = midi (service, { trigger });
    text += trigger.kind == MidiTrigger::Kind::note ? juce::String::fromUTF8 (" · 최소 velocity ") + juce::String (trigger.minVelocity)
        : juce::String (" · high ") + juce::String (trigger.highThreshold) + " / low " + juce::String (trigger.lowThreshold)
            + (trigger.behavior == MidiTrigger::Behavior::gate ? " · gate" : " · pulse");
    return text + juce::String::fromUTF8 (" · 바운스 ") + juce::String (trigger.debounceMs) + "ms";
}
juce::String deviceStatus (MidiInputService::Status status)
{
    using S = MidiInputService::Status;
    return juce::String::fromUTF8 (status == S::connected ? "연결" : status == S::notSelected ? "선택 안 함"
        : status == S::disconnected ? "미연결" : status == S::unavailable ? "사용 불가" : "준비 대기");
}
juce::String midiState (const ShortcutService& service, const MidiTrigger& trigger, const std::vector<MidiInputService::Device>& devices)
{
    using S = MidiInputService::Status;
    bool unavailable = false, waiting = false;
    for (const auto& device : devices)
    {
        if (trigger.source != "any" && device.identifier != trigger.source) continue;
        if (device.status == S::connected) return deviceStatus (S::connected);
        waiting |= device.status == S::waiting;
        unavailable |= device.status == S::unavailable;
    }
    if (waiting) return deviceStatus (S::waiting);
    if (unavailable) return deviceStatus (S::unavailable);
    if (trigger.source != "any" && std::any_of (devices.begin(), devices.end(), [&] (const auto& device)
        { return device.identifier == trigger.source && device.status == S::notSelected; })) return deviceStatus (S::notSelected);
    juce::ignoreUnused (service);
    return deviceStatus (S::disconnected);
}
juce::String midiSummary (const MidiInputService& input, const ShortcutService* service)
{
    int active = 0, missing = 0, waiting = 0, unavailable = 0;
    std::set<juce::String> mapped, seen;
    if (service != nullptr)
        for (const auto& binding : service->midiCommandBindings()) if (binding.trigger.source != "any") mapped.insert (binding.trigger.source);
    for (const auto& device : input.devices())
    {
        seen.insert (device.identifier);
        using S = MidiInputService::Status;
        active += device.status == S::connected || device.status == S::waiting;
        const bool relevant = service == nullptr || service->getMidiInputSettings().autoUseAll
            || service->getMidiInputSettings().selected.count (device.identifier) != 0 || mapped.count (device.identifier) != 0;
        missing += relevant && device.status == S::disconnected;
        waiting += device.status == S::waiting;
        unavailable += device.status == S::unavailable;
    }
    for (const auto& id : mapped) missing += seen.count (id) == 0;
    auto text = "MIDI " + juce::String (active);
    if (missing != 0) text += juce::String::fromUTF8 (" · 미연결 ") + juce::String (missing);
    if (waiting != 0) text += juce::String::fromUTF8 (" · 준비 대기 ") + juce::String (waiting);
    if (unavailable != 0) text += juce::String::fromUTF8 (" · 사용 불가 ") + juce::String (unavailable);
    if (input.hasInputFault()) text += juce::String::fromUTF8 (" · 과부하");
    return text;
}
}
