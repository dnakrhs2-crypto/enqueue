#pragma once

#include "app/UiTranslation.h"
#include <optional>

namespace gocue::livemix
{
enum class Language { ko, en, ja };

const char* languageCode (Language) noexcept;
std::optional<Language> parseLanguage (const juce::String&);
struct LanguageChoice
{
    Language language = Language::ko;
    bool save = false;
};
// windowsDisplayLanguage is a Windows LANGID (not the region or keyboard language).
LanguageChoice chooseLanguage (const juce::String& saved, bool hasLastRunVersion,
                               unsigned windowsDisplayLanguage, const juce::String& environment);
unsigned windowsDisplayLanguage();
void initialiseLanguage (Language);
Language currentLanguage() noexcept;
const char* uiTypefaceFor (Language) noexcept;
juce::String tr (const char* korean);
/** tr() in a language other than the one that runs: a language just picked is confirmed in that language. */
juce::String trIn (const char* korean, Language);
juce::String translate (Language, const char* korean);
const juce::var& translationTable();

template <typename... Values>
juce::String trf (const char* korean, const Values&... values)
{
    return formatUiText (tr (korean), { juce::String (values)... });
}

juce::String newMicName (int number);
/** Exact factory FX names across supported languages; user-assigned names are not translated. */
bool isDefaultFxName (const juce::String& name);
juce::String defaultSessionName();
juce::File defaultRecordingDirectory();
juce::String defaultRecordingTrackName (int track);
}
