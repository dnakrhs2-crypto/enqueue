#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace gocue
{
/** A binding, independent of a physical port's current state. Channel 0 means any.
    Device identifiers are opaque; project cues currently accept source=any only. */
struct MidiTrigger
{
    enum class Kind { note, cc };
    enum class Edge { rising, falling, both };
    enum class Behavior { gate, pulse };
    juce::String source = "any";
    Kind kind = Kind::note;
    int channel = 0, number = 0, minVelocity = 1;
    Edge edge = Edge::rising;
    int highThreshold = 64, lowThreshold = 63;
    Behavior behavior = Behavior::gate;
    int debounceMs = 20;

    juce::Result validate (bool projectCue = false) const;
    juce::String display (const juce::String& deviceName = {}) const;
    static juce::String noteName (int number); // C4 = 60, on every platform
    std::unique_ptr<juce::XmlElement> toXml() const;
    static juce::Result fromXml (const juce::XmlElement&, MidiTrigger&, bool projectCue = false);
    juce::var toVar() const;
    static juce::Result fromVar (const juce::var&, MidiTrigger&, bool projectCue = false);
    bool operator== (const MidiTrigger&) const;
    bool operator!= (const MidiTrigger& b) const { return ! (*this == b); }
};
using MidiTriggers = std::vector<MidiTrigger>;
}
