#pragma once

#include "app/ShortcutCatalog.h"
#include "app/MidiShortcutProfile.h"

#include <map>

namespace gocue
{

/** Schema-v1 key vocabulary. Names are case-sensitive; modifiers are separate 0/1 XML attributes.
    No localized descriptions, modifier-only keys, Win/IME/media keys or F25+ are accepted. */
class ShortcutKeyCodec
{
public:
    struct NamedKey { juce::String name; int code; };
    static const std::vector<NamedKey>& allowedKeys();
    static juce::Result parse (const juce::XmlElement& xml, juce::KeyPress& key);
    static juce::Result validate (const juce::KeyPress& key);
    static juce::KeyPress normalise (const juce::KeyPress& key);
    static juce::String keyName (const juce::KeyPress& key);
    static std::unique_ptr<juce::XmlElement> toXml (const juce::KeyPress& key);
};

struct ShortcutProfileParseResult;
struct ShortcutExchangeParseResult;

/** Only overrides are stored: missing action = inheritance, empty keys = explicitly unassigned.
    Unknown IDs use exactly the same schema and survive subsequent edits and exports. */
struct ShortcutProfile
{
    using Overrides = std::map<juce::String, ShortcutKeys>;
    Overrides overrides;

    static ShortcutProfileParseResult parse (const juce::String& source);
    juce::Result validate() const;
    juce::Result serialise (juce::String& xml) const;
    static ShortcutExchangeParseResult parseExchange (const juce::String&);
    juce::Result serialiseExchange (const MidiShortcutProfile&, juce::String&) const;

    bool operator== (const ShortcutProfile& other) const { return overrides == other.overrides; }
    bool operator!= (const ShortcutProfile& other) const { return ! (*this == other); }
};

struct ShortcutProfileParseResult
{
    enum class Error { none, malformedXml, oldSchema, futureSchema, platform, structure, key };
    Error error = Error::none;
    juce::String message;
    juce::String originalXml; // including rejected input, never replaced by a repaired/default document
    ShortcutProfile profile; // populated only on complete success

    bool wasOk() const noexcept { return error == Error::none; }
};

struct ShortcutExchangeParseResult
{
    juce::Result status = juce::Result::ok();
    juce::String originalXml;
    ShortcutProfile keyboard;
    MidiShortcutProfile midi;
    bool replacesMidi = false;
    bool wasOk() const { return status.wasOk(); }
};

} // namespace gocue
