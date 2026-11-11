#include "Localisation.h"
#include <LiveMixStrings.h>
#include <map>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::livemix
{
namespace
{
    std::atomic<Language> language { Language::ko };
    struct Translations
    {
        juce::var rows = juce::JSON::parse (juce::String::fromUTF8 (LiveMixStrings::strings_json,
                                                                 LiveMixStrings::strings_jsonSize));
        std::map<juce::String, std::pair<juce::String, juce::String>> byKorean;
        Translations()
        {
            if (const auto* array = rows.getArray())
                for (const auto& row : *array)
                    byKorean.emplace (row["ko"].toString(),
                                      std::make_pair (row["en"].toString(), row["ja"].toString()));
        }
    };
    const Translations& translations()
    {
        static const Translations table;
        return table;
    }
}

const char* languageCode (Language value) noexcept
{
    switch (value)
    {
        case Language::en: return "en";
        case Language::ja: return "ja";
        default: return "ko";
    }
}

std::optional<Language> parseLanguage (const juce::String& value)
{
    if (value == "ko") return Language::ko;
    if (value == "en") return Language::en;
    if (value == "ja") return Language::ja;
    return std::nullopt;
}

LanguageChoice chooseLanguage (const juce::String& saved, bool hasLastRunVersion,
                               unsigned displayLanguage, const juce::String& environment)
{
    if (const auto override = parseLanguage (environment)) return { *override, false };
    if (const auto selected = parseLanguage (saved)) return { *selected, false };
    if (hasLastRunVersion) return { Language::ko, true };
    const auto primary = displayLanguage & 0x3ffu;
    return { primary == 0x12 ? Language::ko : primary == 0x11 ? Language::ja : Language::en, true };
}

unsigned windowsDisplayLanguage()
{
   #if JUCE_WINDOWS
    return GetUserDefaultUILanguage();
   #else
    return 0;
   #endif
}

void initialiseLanguage (Language selected)
{
    (void) translations(); // Build the immutable table before any audio/collab workers start.
    language.store (selected, std::memory_order_release);
    uiTranslator.store (&tr, std::memory_order_release);
}
Language currentLanguage() noexcept { return language.load (std::memory_order_acquire); }
// Malgun Gothic draws kana and kanji from other fonts, a line at a time uneven; Yu Gothic UI is in every Windows 10 / 11.
// Malgun Gothic's backslash is the won sign, which reads as a wrong path outside Korea: English takes Segoe UI, whose
// Latin letters have the same height and nearly the same width (a Korean device or plugin name still comes from Malgun Gothic).
const char* uiTypefaceFor (Language selected) noexcept
{
    return selected == Language::ja ? "Yu Gothic UI" : selected == Language::en ? "Segoe UI" : "Malgun Gothic";
}
const juce::var& translationTable() { return translations().rows; }

juce::String translate (Language selected, const char* korean)
{
    const auto source = juce::String::fromUTF8 (korean);
    if (selected == Language::ko) return source;
    const auto& table = translations().byKorean;
    const auto found = table.find (source);
    if (found == table.end()) return source;
    const auto& translated = selected == Language::ja ? found->second.second : found->second.first;
    return translated.isEmpty() ? source : translated;
}
juce::String tr (const char* korean) { return translate (currentLanguage(), korean); }
juce::String trIn (const char* korean, Language selected) { return translate (selected, korean); }
juce::String newMicName (int number) { return trf ("마이크 {0}", number); }
bool isDefaultFxName (const juce::String& name)
{
    for (auto selected : { Language::ko, Language::en, Language::ja })
        if (name == trIn ("리버브", selected)) return true;
    return false;
}
juce::String defaultSessionName() { return tr ("기본 세션"); }
juce::File defaultRecordingDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (tr ("LiveMix 녹음"));
}
juce::String defaultRecordingTrackName (int track)
{
    switch (track)
    {
        case 0: return tr ("내소리");
        case 1: return tr ("합방상대1");
        case 2: return tr ("합방상대2");
        default: return tr ("D_전체");
    }
}
}
