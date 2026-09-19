#include "app/ShortcutProfile.h"

#include <set>

namespace gocue
{
namespace
{
using juce::KeyPress;
using juce::ModifierKeys;
constexpr int supportedModifiers = ModifierKeys::ctrlModifier | ModifierKeys::altModifier | ModifierKeys::shiftModifier;

bool hasOnlyAttributes (const juce::XmlElement& xml, std::initializer_list<const char*> allowed)
{
    std::set<juce::String> seen;
    for (int i = 0; i < xml.getNumAttributes(); ++i)
    {
        const auto name = xml.getAttributeName (i);
        if (! seen.insert (name).second || std::none_of (allowed.begin(), allowed.end(), [&name] (const char* a) { return name == a; }))
            return false;
    }
    return true;
}

bool validID (const juce::String& id)
{
    return id.isNotEmpty() && id.length() <= 200
        && id.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");
}

/** JUCE's XML reader accepts mismatched closing names and ignores trailing roots. Check the small,
    element-only profile grammar before asking JUCE to decode attributes; no DTD/CDATA/entities to load. */
bool hasStrictXmlStructure (const juce::String& source)
{
    std::vector<juce::String> stack;
    bool sawRoot = false, sawDeclaration = false;
    const int length = source.length();
    int position = 0;
    if (length > 0 && source[0] == 0xfeff)
        ++position;
    while (position < length)
    {
        if (juce::CharacterFunctions::isWhitespace (source[position]))
        {
            ++position;
            continue;
        }
        if (source.substring (position, position + 4) == "<!--")
        {
            const int end = source.indexOf (position + 4, "-->");
            if (end < 0 || source.substring (position + 4, end).contains ("--"))
                return false;
            position = end + 3;
            continue;
        }
        if (source.substring (position, position + 5) == "<?xml")
        {
            if (sawRoot || sawDeclaration || position + 5 >= length || ! juce::CharacterFunctions::isWhitespace (source[position + 5]))
                return false;
            const int end = source.indexOf (position + 5, "?>");
            if (end < 0)
                return false;
            sawDeclaration = true;
            position = end + 2;
            continue;
        }
        if (source[position] != '<')
            return false; // profiles contain no text nodes

        juce::juce_wchar quote = 0;
        int end = position + 1;
        for (; end < length; ++end)
        {
            const auto character = source[end];
            if (character == '<')
                return false;
            if (quote != 0)
            {
                if (character == quote)
                    quote = 0;
            }
            else if (character == '\'' || character == '"')
                quote = character;
            else if (character == '>')
                break;
        }
        if (end == length || quote != 0)
            return false;
        auto tag = source.substring (position + 1, end);
        if (tag.startsWithChar ('/'))
        {
            tag = tag.substring (1).trimEnd();
            if (stack.empty() || stack.back() != tag)
                return false;
            stack.pop_back();
        }
        else
        {
            int nameEnd = 0;
            while (nameEnd < tag.length() && ((tag[nameEnd] >= 'A' && tag[nameEnd] <= 'Z') || tag[nameEnd] == '_'))
                ++nameEnd;
            const auto name = tag.substring (0, nameEnd);
            if (name != "ENQUEUE_SHORTCUTS" && name != "ACTION" && name != "KEY")
                return false;
            if (stack.empty())
            {
                if (sawRoot)
                    return false;
                sawRoot = true;
            }
            if (! tag.endsWithChar ('/'))
                stack.push_back (name);
            if (stack.size() > 3)
                return false;
        }
        position = end + 1;
    }
    return sawRoot && stack.empty();
}
} // namespace

const std::vector<ShortcutKeyCodec::NamedKey>& ShortcutKeyCodec::allowedKeys()
{
    static const auto keys = []
    {
        std::vector<NamedKey> result {
            { "Space", KeyPress::spaceKey }, { "Esc", KeyPress::escapeKey }, { "Enter", KeyPress::returnKey },
            { "Tab", KeyPress::tabKey }, { "Backspace", KeyPress::backspaceKey }, { "Delete", KeyPress::deleteKey },
            { "Insert", KeyPress::insertKey }, { "Left", KeyPress::leftKey }, { "Right", KeyPress::rightKey },
            { "Up", KeyPress::upKey }, { "Down", KeyPress::downKey }, { "PageUp", KeyPress::pageUpKey },
            { "PageDown", KeyPress::pageDownKey }, { "Home", KeyPress::homeKey }, { "End", KeyPress::endKey },
            { "Comma", ',' }, { "Period", '.' }, { "Slash", '/' }, { "Backslash", '\\' }, { "Semicolon", ';' },
            { "Quote", '\'' }, { "LeftBracket", '[' }, { "RightBracket", ']' }, { "Minus", '-' },
            { "Equals", '=' }, { "Backtick", '`' }, { "Plus", '+' },
            { "NumPadAdd", KeyPress::numberPadAdd }, { "NumPadSubtract", KeyPress::numberPadSubtract },
            { "NumPadMultiply", KeyPress::numberPadMultiply }, { "NumPadDivide", KeyPress::numberPadDivide },
            { "NumPadSeparator", KeyPress::numberPadSeparator }, { "NumPadDecimal", KeyPress::numberPadDecimalPoint },
            { "NumPadEquals", KeyPress::numberPadEquals }
        };
        for (int code = 'A'; code <= 'Z'; ++code)
            result.push_back ({ juce::String::charToString (static_cast<juce::juce_wchar> (code)), code });
        for (int code = '0'; code <= '9'; ++code)
            result.push_back ({ juce::String::charToString (static_cast<juce::juce_wchar> (code)), code });

        // Explicit JUCE constants: their numeric layout is not a persistence contract.
        const int functionKeys[] { KeyPress::F1Key, KeyPress::F2Key, KeyPress::F3Key, KeyPress::F4Key, KeyPress::F5Key, KeyPress::F6Key,
                                   KeyPress::F7Key, KeyPress::F8Key, KeyPress::F9Key, KeyPress::F10Key, KeyPress::F11Key, KeyPress::F12Key,
                                   KeyPress::F13Key, KeyPress::F14Key, KeyPress::F15Key, KeyPress::F16Key, KeyPress::F17Key, KeyPress::F18Key,
                                   KeyPress::F19Key, KeyPress::F20Key, KeyPress::F21Key, KeyPress::F22Key, KeyPress::F23Key, KeyPress::F24Key };
        for (int i = 0; i < 24; ++i)
            result.push_back ({ "F" + juce::String (i + 1), functionKeys[i] });
        const int padKeys[] { KeyPress::numberPad0, KeyPress::numberPad1, KeyPress::numberPad2, KeyPress::numberPad3, KeyPress::numberPad4,
                             KeyPress::numberPad5, KeyPress::numberPad6, KeyPress::numberPad7, KeyPress::numberPad8, KeyPress::numberPad9 };
        for (int i = 0; i < 10; ++i)
            result.push_back ({ "NumPad" + juce::String (i), padKeys[i] });
        // NumPad Enter/Delete are not separate names: JUCE Windows reports Enter/Delete.
        return result;
    }();
    return keys;
}

juce::KeyPress ShortcutKeyCodec::normalise (const juce::KeyPress& key)
{
    int code = key.getKeyCode();
    if (code >= 'a' && code <= 'z')
        code += 'A' - 'a';
    return { code, key.getModifiers(), 0 };
}

juce::String ShortcutKeyCodec::keyName (const juce::KeyPress& key)
{
    const auto code = normalise (key).getKeyCode();
    for (const auto& named : allowedKeys())
        if (named.code == code)
            return named.name;
    return {};
}

juce::Result ShortcutKeyCodec::validate (const juce::KeyPress& key)
{
    if ((key.getModifiers().getRawFlags() & ~supportedModifiers) != 0)
        return juce::Result::fail ("Unsupported shortcut modifier (only Ctrl/Alt/Shift are allowed)");
    if (keyName (key).isEmpty())
        return juce::Result::fail ("Unsupported shortcut key");
    return juce::Result::ok();
}

juce::Result ShortcutKeyCodec::parse (const juce::XmlElement& xml, juce::KeyPress& key)
{
    if (! xml.hasTagName ("KEY") || ! hasOnlyAttributes (xml, { "key", "ctrl", "alt", "shift" }) || xml.getFirstChildElement() != nullptr)
        return juce::Result::fail ("Invalid KEY element or modifier attribute");
    const auto name = xml.getStringAttribute ("key");
    const auto found = std::find_if (allowedKeys().begin(), allowedKeys().end(), [&name] (const NamedKey& n) { return n.name == name; });
    if (found == allowedKeys().end())
        return juce::Result::fail ("Unknown or empty key name: " + name);

    int modifiers = 0;
    for (const auto& modifier : { std::pair<const char*, int> { "ctrl", ModifierKeys::ctrlModifier },
                                 { "alt", ModifierKeys::altModifier }, { "shift", ModifierKeys::shiftModifier } })
    {
        if (! xml.hasAttribute (modifier.first))
            continue;
        const auto value = xml.getStringAttribute (modifier.first);
        if (value != "0" && value != "1")
            return juce::Result::fail ("Modifier must be 0 or 1: " + juce::String (modifier.first));
        if (value == "1")
            modifiers |= modifier.second;
    }
    key = { found->code, modifiers, 0 };
    return juce::Result::ok();
}

std::unique_ptr<juce::XmlElement> ShortcutKeyCodec::toXml (const juce::KeyPress& key)
{
    if (validate (key).failed())
        return nullptr;
    auto xml = std::make_unique<juce::XmlElement> ("KEY");
    xml->setAttribute ("key", keyName (key));
    if (key.getModifiers().isCtrlDown())  xml->setAttribute ("ctrl", 1);
    if (key.getModifiers().isAltDown())   xml->setAttribute ("alt", 1);
    if (key.getModifiers().isShiftDown()) xml->setAttribute ("shift", 1);
    return xml;
}

juce::Result ShortcutProfile::validate() const
{
    for (const auto& [id, keys] : overrides)
    {
        if (! validID (id))
            return juce::Result::fail ("Invalid action ID: " + id);
        ShortcutKeys seen;
        for (const auto& key : keys)
        {
            if (auto checked = ShortcutKeyCodec::validate (key); checked.failed())
                return checked;
            const auto normalised = ShortcutKeyCodec::normalise (key);
            if (seen.contains (normalised))
                return juce::Result::fail ("Duplicate key in action: " + id);
            seen.add (normalised);
        }
    }
    return juce::Result::ok();
}

ShortcutProfileParseResult ShortcutProfile::parse (const juce::String& source)
{
    using Error = ShortcutProfileParseResult::Error;
    const auto failure = [&source] (Error error, const juce::String& message)
    {
        return ShortcutProfileParseResult { error, message, source, {} };
    };
    // Profiles never need a DTD or entity definitions (including file-backed entities).
    if (! hasStrictXmlStructure (source))
        return failure (Error::malformedXml, "Invalid shortcut XML structure (DTD, entities and text are not supported)");
    juce::XmlDocument document (source);
    const auto xml = document.getDocumentElement();
    if (xml == nullptr || document.getLastParseError().isNotEmpty())
        return failure (Error::malformedXml, "Invalid shortcut XML: " + document.getLastParseError());
    if (! xml->hasTagName ("ENQUEUE_SHORTCUTS") || ! hasOnlyAttributes (*xml, { "schemaVersion", "platform" }))
        return failure (Error::structure, "Invalid ENQUEUE_SHORTCUTS root");

    const auto version = xml->getStringAttribute ("schemaVersion");
    if (version.isEmpty() || ! version.containsOnly ("0123456789") || (version.length() > 1 && version.startsWithChar ('0')))
        return failure (Error::structure, "Missing or invalid schemaVersion");
    if (version == "0")
        return failure (Error::oldSchema, "Unsupported older shortcut schema (no pre-v1 migration is defined)");
    if (version != "1")
        return failure (Error::futureSchema, "This shortcut schema is newer than this app supports");
    if (xml->getStringAttribute ("platform") != "windows")
        return failure (Error::platform, "Only platform=windows is supported");

    ShortcutProfile candidate;
    for (const auto* action : xml->getChildIterator())
    {
        if (! action->hasTagName ("ACTION") || ! hasOnlyAttributes (*action, { "id" }))
            return failure (Error::structure, "Expected ACTION with an id attribute");
        const auto id = action->getStringAttribute ("id");
        if (! validID (id) || candidate.overrides.count (id) != 0)
            return failure (Error::structure, "Empty, invalid or duplicate action ID: " + id);
        ShortcutKeys keys;
        for (const auto* element : action->getChildIterator())
        {
            juce::KeyPress key;
            if (auto parsed = ShortcutKeyCodec::parse (*element, key); parsed.failed())
                return failure (Error::key, id + ": " + parsed.getErrorMessage());
            if (keys.contains (key))
                return failure (Error::key, "Duplicate key in action: " + id);
            keys.add (key);
        }
        candidate.overrides.emplace (id, std::move (keys));
    }
    return { Error::none, {}, source, std::move (candidate) };
}

juce::Result ShortcutProfile::serialise (juce::String& output) const
{
    if (auto checked = validate(); checked.failed())
        return checked;
    juce::XmlElement xml ("ENQUEUE_SHORTCUTS");
    xml.setAttribute ("schemaVersion", 1);
    xml.setAttribute ("platform", "windows");
    for (const auto& [id, keys] : overrides)
    {
        auto* action = xml.createNewChildElement ("ACTION");
        action->setAttribute ("id", id);
        for (const auto& key : keys)
            action->addChildElement (ShortcutKeyCodec::toXml (key).release());
    }
    output = xml.toString();
    return juce::Result::ok();
}

} // namespace gocue
