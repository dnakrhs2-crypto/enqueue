#include "app/MidiShortcutProfile.h"
#include "model/InputProfileXml.h"

namespace gocue
{
namespace
{
juce::Result validDevices (const std::map<juce::String, juce::String>& devices)
{
    for (const auto& [id, name] : devices)
    {
        MidiTrigger t;
        t.source = id;
        if (id == "any" || t.validate().failed() || name.length() > 2048) return juce::Result::fail ("Invalid MIDI device identifier/name");
    }
    return juce::Result::ok();
}
void writeDevices (juce::XmlElement& xml, const std::map<juce::String, juce::String>& devices)
{
    for (const auto& [id, name] : devices)
    {
        auto* d = xml.createNewChildElement ("DEVICE");
        d->setAttribute ("identifier", id);
        d->setAttribute ("name", name);
    }
}
bool readDevice (const juce::XmlElement& x, std::map<juce::String, juce::String>& devices)
{
    return x.hasTagName ("DEVICE") && input_xml::attributes (x, { "identifier", "name" }) && x.getFirstChildElement() == nullptr
        && x.hasAttribute ("name") && devices.emplace (x.getStringAttribute ("identifier"), x.getStringAttribute ("name")).second;
}
}
juce::Result MidiShortcutProfile::validate() const
{
    if (auto result = validDevices (deviceNames); result.failed()) return result;
    for (const auto& [id, triggers] : overrides)
    {
        if (! input_xml::id (id)) return juce::Result::fail ("Invalid MIDI action ID");
        for (size_t i = 0; i < triggers.size(); ++i)
        {
            if (auto result = triggers[i].validate(); result.failed()) return result;
            if (std::find (triggers.begin(), triggers.begin() + static_cast<ptrdiff_t> (i), triggers[i]) != triggers.begin() + static_cast<ptrdiff_t> (i))
                return juce::Result::fail ("Duplicate MIDI trigger");
        }
    }
    return juce::Result::ok();
}
juce::Result MidiShortcutProfile::serialise (juce::String& output) const
{
    if (auto r = validate(); r.failed()) return r;
    juce::XmlElement x ("ENQUEUE_MIDI_SHORTCUTS");
    x.setAttribute ("schemaVersion", 1);
    writeDevices (x, deviceNames);
    for (const auto& [id, triggers] : overrides)
    {
        auto* action = x.createNewChildElement ("ACTION");
        action->setAttribute ("id", id);
        for (const auto& t : triggers) action->addChildElement (t.toXml().release());
    }
    output = x.toString();
    return juce::Result::ok();
}
MidiShortcutProfileParseResult MidiShortcutProfile::parse (const juce::String& source)
{
    const auto fail = [&] (const juce::String& message) { return MidiShortcutProfileParseResult { juce::Result::fail (message), source, {} }; };
    if (! input_xml::structure (source, { "ENQUEUE_MIDI_SHORTCUTS", "ACTION", "TRIGGER", "DEVICE" }, 3)) return fail ("Invalid MIDI profile XML structure");
    juce::XmlDocument doc (source);
    auto xml = doc.getDocumentElement();
    if (! xml || doc.getLastParseError().isNotEmpty() || ! xml->hasTagName ("ENQUEUE_MIDI_SHORTCUTS")
        || ! input_xml::attributes (*xml, { "schemaVersion" })) return fail ("Invalid MIDI profile root");
    if (xml->getStringAttribute ("schemaVersion") != "1") return fail ("Unsupported MIDI profile schemaVersion");
    MidiShortcutProfile candidate;
    for (const auto* x : xml->getChildIterator())
    {
        if (x->hasTagName ("DEVICE"))
        {
            if (! readDevice (*x, candidate.deviceNames)) return fail ("Invalid MIDI device hint");
            continue;
        }
        const auto id = x->getStringAttribute ("id");
        if (! x->hasTagName ("ACTION") || ! input_xml::attributes (*x, { "id" }) || ! input_xml::id (id)
            || candidate.overrides.count (id) != 0) return fail ("Invalid/duplicate MIDI action");
        auto& list = candidate.overrides[id];
        for (const auto* t : x->getChildIterator())
        {
            MidiTrigger trigger;
            if (auto r = MidiTrigger::fromXml (*t, trigger); r.failed()) return fail (r.getErrorMessage());
            list.push_back (trigger);
        }
    }
    if (auto r = candidate.validate(); r.failed()) return fail (r.getErrorMessage());
    return { juce::Result::ok(), source, std::move (candidate) };
}
juce::Result MidiInputSettings::validate() const { return validDevices (selected); }
juce::Result MidiInputSettings::serialise (juce::String& output) const
{
    if (auto r = validate(); r.failed()) return r;
    juce::XmlElement x ("ENQUEUE_MIDI_INPUTS");
    x.setAttribute ("schemaVersion", 1);
    x.setAttribute ("autoUseAll", autoUseAll ? 1 : 0);
    x.setAttribute ("allowBackgroundPlayback", allowBackgroundPlayback ? 1 : 0);
    writeDevices (x, selected);
    output = x.toString();
    return juce::Result::ok();
}
juce::Result MidiInputSettings::parse (const juce::String& source, MidiInputSettings& out)
{
    const auto fail = [] { return juce::Result::fail ("Invalid MIDI input settings"); };
    if (! input_xml::structure (source, { "ENQUEUE_MIDI_INPUTS", "DEVICE" }, 2)) return fail();
    juce::XmlDocument doc (source);
    auto xml = doc.getDocumentElement();
    if (! xml || doc.getLastParseError().isNotEmpty() || ! xml->hasTagName ("ENQUEUE_MIDI_INPUTS")
        || ! input_xml::attributes (*xml, { "schemaVersion", "autoUseAll", "allowBackgroundPlayback" })
        || xml->getStringAttribute ("schemaVersion") != "1") return fail();
    MidiInputSettings s;
    const auto all = xml->getStringAttribute ("autoUseAll"), background = xml->getStringAttribute ("allowBackgroundPlayback");
    if ((all != "0" && all != "1") || (background != "0" && background != "1")) return fail();
    s.autoUseAll = all == "1";
    s.allowBackgroundPlayback = background == "1";
    for (const auto* x : xml->getChildIterator()) if (! readDevice (*x, s.selected)) return fail();
    if (auto r = s.validate(); r.failed()) return r;
    out = std::move (s);
    return juce::Result::ok();
}
}
