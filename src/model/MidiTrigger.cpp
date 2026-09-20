#include "model/MidiTrigger.h"
#include "model/InputProfileXml.h"

namespace gocue
{
juce::Result MidiTrigger::validate (bool projectCue) const
{
    if (source.isEmpty() || source.length() > 2048 || source.containsAnyOf ("\r\n\t") || (projectCue && source != "any"))
        return juce::Result::fail ("Invalid MIDI source");
    if (channel < 0 || channel > 16 || number < 0 || number > 127 || debounceMs < 0 || debounceMs > 500)
        return juce::Result::fail ("MIDI channel, number or debounce out of range");
    if (kind != Kind::note && kind != Kind::cc) return juce::Result::fail ("Invalid MIDI kind");
    if (minVelocity < 1 || minVelocity > 127 || lowThreshold < 0 || lowThreshold >= highThreshold || highThreshold > 127)
        return juce::Result::fail ("Invalid MIDI velocity or thresholds");
    if ((edge != Edge::rising && edge != Edge::falling && edge != Edge::both)
        || (behavior != Behavior::gate && behavior != Behavior::pulse) || (behavior == Behavior::gate && edge == Edge::both))
        return juce::Result::fail ("Invalid MIDI edge/behavior");
    return juce::Result::ok();
}
juce::String MidiTrigger::noteName (int n)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return n >= 0 && n <= 127 ? juce::String (names[n % 12]) + juce::String (n / 12 - 1) : juce::String();
}
juce::String MidiTrigger::display (const juce::String& deviceName) const
{
    auto text = kind == Kind::note ? juce::String::fromUTF8 ("노트 ") + noteName (number) + " (" + juce::String (number) + ")"
        : "CC " + juce::String (number) + (edge == Edge::falling ? juce::String::fromUTF8 (" ≤") + juce::String (lowThreshold)
            : juce::String::fromUTF8 (" ≥") + juce::String (highThreshold))
            + juce::String::fromUTF8 (" · ") + juce::String::fromUTF8 (edge == Edge::rising ? "상승" : edge == Edge::falling ? "하강" : "양쪽");
    text += juce::String::fromUTF8 (" · ch") + (channel == 0 ? juce::String::fromUTF8 ("전체") : juce::String (channel));
    if (deviceName.isNotEmpty() || source != "any") text += juce::String::fromUTF8 (" · ") + (deviceName.isNotEmpty() ? deviceName : source);
    return text;
}
std::unique_ptr<juce::XmlElement> MidiTrigger::toXml() const
{
    if (validate().failed()) return {};
    auto x = std::make_unique<juce::XmlElement> ("TRIGGER");
    x->setAttribute ("source", source);
    x->setAttribute ("kind", kind == Kind::note ? "note" : "cc");
    x->setAttribute ("channel", channel == 0 ? juce::String ("any") : juce::String (channel));
    x->setAttribute ("number", number);
    x->setAttribute ("debounceMs", debounceMs);
    if (kind == Kind::note) x->setAttribute ("minVelocity", minVelocity);
    else
    {
        x->setAttribute ("edge", edge == Edge::rising ? "rising" : edge == Edge::falling ? "falling" : "both");
        x->setAttribute ("highThreshold", highThreshold);
        x->setAttribute ("lowThreshold", lowThreshold);
        x->setAttribute ("behavior", behavior == Behavior::gate ? "gate" : "pulse");
    }
    return x;
}
juce::Result MidiTrigger::fromXml (const juce::XmlElement& x, MidiTrigger& out, bool projectCue)
{
    const auto fail = [] { return juce::Result::fail ("Invalid MIDI TRIGGER"); };
    if (! x.hasTagName ("TRIGGER") || x.getFirstChildElement() != nullptr) return fail();
    MidiTrigger t;
    const auto k = x.getStringAttribute ("kind");
    if (k != "note" && k != "cc") return fail();
    t.kind = k == "note" ? Kind::note : Kind::cc;
    if (t.kind == Kind::note ? ! input_xml::attributes (x, { "source", "kind", "channel", "number", "minVelocity", "debounceMs" })
                           : ! input_xml::attributes (x, { "source", "kind", "channel", "number", "edge", "highThreshold", "lowThreshold", "behavior", "debounceMs" })) return fail();
    t.source = x.getStringAttribute ("source");
    const auto read = [&] (const char* name, int lo, int hi, int& value, bool required = true)
    { return ! required && ! x.hasAttribute (name) ? true : input_xml::integer (x.getStringAttribute (name), lo, hi, value); };
    if ((x.getStringAttribute ("channel") != "any" && ! read ("channel", 1, 16, t.channel))
        || ! read ("number", 0, 127, t.number) || ! read ("debounceMs", 0, 500, t.debounceMs, false)) return fail();
    if (t.kind == Kind::note)
    {
        if (! read ("minVelocity", 1, 127, t.minVelocity, false)) return fail();
    }
    else
    {
        const auto e = x.getStringAttribute ("edge"), b = x.getStringAttribute ("behavior");
        if (e != "rising" && e != "falling" && e != "both") return fail();
        if (b != "gate" && b != "pulse") return fail();
        t.edge = e == "rising" ? Edge::rising : e == "falling" ? Edge::falling : Edge::both;
        t.behavior = b == "gate" ? Behavior::gate : Behavior::pulse;
        if (! read ("highThreshold", 1, 127, t.highThreshold) || ! read ("lowThreshold", 0, 126, t.lowThreshold)) return fail();
    }
    if (auto checked = t.validate (projectCue); checked.failed()) return checked;
    out = t;
    return juce::Result::ok();
}
juce::var MidiTrigger::toVar() const
{
    auto* obj = new juce::DynamicObject();
    if (auto x = toXml())
        for (int i = 0; i < x->getNumAttributes(); ++i)
        {
            const auto name = x->getAttributeName (i), value = x->getAttributeValue (i);
            if (name == "source" || name == "kind" || name == "edge" || name == "behavior" || (name == "channel" && channel == 0))
                obj->setProperty (name, value);
            else obj->setProperty (name, value.getIntValue());
        }
    return juce::var (obj);
}
juce::Result MidiTrigger::fromVar (const juce::var& v, MidiTrigger& out, bool projectCue)
{
    const auto* obj = v.getDynamicObject();
    if (obj == nullptr) return juce::Result::fail ("MIDI trigger must be an object");
    juce::XmlElement x ("TRIGGER");
    for (const auto& p : obj->getProperties())
    {
        const auto name = p.name.toString();
        const bool string = name == "source" || name == "kind" || name == "edge" || name == "behavior"
                            || (name == "channel" && p.value.isString() && p.value.toString() == "any");
        if (string ? ! p.value.isString() : (! p.value.isInt() && ! p.value.isInt64()))
            return juce::Result::fail ("Invalid MIDI field type: " + name);
        x.setAttribute (name, p.value.toString());
    }
    return fromXml (x, out, projectCue);
}
bool MidiTrigger::operator== (const MidiTrigger& b) const
{
    return source == b.source && kind == b.kind && channel == b.channel && number == b.number && debounceMs == b.debounceMs
        && (kind == Kind::note ? minVelocity == b.minVelocity : edge == b.edge && behavior == b.behavior
            && highThreshold == b.highThreshold && lowThreshold == b.lowThreshold);
}
}
