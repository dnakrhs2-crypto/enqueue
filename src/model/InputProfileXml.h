#pragma once

#include <juce_core/juce_core.h>
#include <set>
#include <vector>

namespace gocue::input_xml
{
inline bool attributes (const juce::XmlElement& xml, std::initializer_list<const char*> names)
{
    std::set<juce::String> seen;
    for (int i = 0; i < xml.getNumAttributes(); ++i)
    {
        const auto name = xml.getAttributeName (i);
        if (! seen.insert (name).second || std::none_of (names.begin(), names.end(), [&] (const char* n) { return name == n; }))
            return false;
    }
    return true;
}
inline bool id (const juce::String& s)
{
    return s.isNotEmpty() && s.length() <= 200 && s.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");
}
inline bool integer (const juce::String& s, int low, int high, int& value)
{
    if (s.isEmpty() || s.length() > 9 || ! s.containsOnly ("0123456789") || (s.length() > 1 && s[0] == '0'))
        return false;
    const int n = s.getIntValue();
    if (n < low || n > high) return false;
    value = n;
    return true;
}
/** JUCE's parser tolerates mismatched end tags/trailing roots. Validate a bounded,
    element-only grammar first, including duplicate attributes. No DTD or custom entities. */
inline bool structure (const juce::String& s, std::initializer_list<const char*> tags, size_t maxDepth)
{
    if (s.getNumBytesAsUTF8() > 4 * 1024 * 1024) return false;
    std::vector<juce::String> stack;
    bool root = false, declaration = false;
    int p = s.startsWithChar (0xfeff) ? 1 : 0;
    const auto space = [&] { while (p < s.length() && juce::CharacterFunctions::isWhitespace (s[p])) ++p; };
    while (p < s.length())
    {
        space();
        if (p == s.length()) break;
        if (s.substring (p, p + 4) == "<!--")
        {
            const int end = s.indexOf (p + 4, "-->");
            if (end < 0 || s.substring (p + 4, end).contains ("--")) return false;
            p = end + 3;
            continue;
        }
        if (s.substring (p, p + 5) == "<?xml")
        {
            if (root || declaration || ! juce::CharacterFunctions::isWhitespace (s[p + 5])) return false;
            const int end = s.indexOf (p + 5, "?>");
            if (end < 0) return false;
            declaration = true;
            p = end + 2;
            continue;
        }
        if (s[p++] != '<') return false;
        const bool closing = s[p] == '/';
        if (closing) ++p;
        const int start = p;
        while (p < s.length() && ((s[p] >= 'A' && s[p] <= 'Z') || s[p] == '_')) ++p;
        const auto name = s.substring (start, p);
        if (std::none_of (tags.begin(), tags.end(), [&] (const char* t) { return name == t; })) return false;
        if (closing)
        {
            space();
            if (s[p++] != '>' || stack.empty() || stack.back() != name) return false;
            stack.pop_back();
            continue;
        }
        if (stack.empty()) { if (root) return false; root = true; }
        std::set<juce::String> attrs;
        for (;;)
        {
            const int before = p;
            space();
            if (s[p] == '/' || s[p] == '>') break;
            if (p == before) return false;
            const int a = p;
            while (p < s.length() && (juce::CharacterFunctions::isLetterOrDigit (s[p]) || s[p] == '_')) ++p;
            if (p == a || ! attrs.insert (s.substring (a, p)).second) return false;
            space();
            if (s[p++] != '=') return false;
            space();
            const auto q = s[p++];
            if (q != '\'' && q != '"') return false;
            while (p < s.length() && s[p] != q)
            {
                if (s[p] == '<') return false;
                if (s[p] == '&')
                {
                    const int end = s.indexOfChar (p, ';');
                    if (end < 0) return false;
                    const auto entity = s.substring (p + 1, end);
                    if (entity != "amp" && entity != "lt" && entity != "gt" && entity != "quot" && entity != "apos"
                        && ! (entity.startsWithChar ('#') && entity.substring (1).isNotEmpty() && entity.substring (1).containsOnly ("0123456789"))) return false;
                    p = end;
                }
                ++p;
            }
            if (p >= s.length()) return false;
            ++p;
        }
        const bool empty = s[p] == '/';
        if (empty) ++p;
        if (s[p++] != '>') return false;
        if (! empty) stack.push_back (name);
        if (stack.size() > maxDepth) return false;
    }
    return root && stack.empty();
}
}
