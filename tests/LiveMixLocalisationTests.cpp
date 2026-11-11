#include "Localisation.h"
#include "AppRestart.h"
#include "LiveMixSettings.h"
#include "MixModel.h"
#include "CollabHostCheck.h"
#include "ObsPluginInstaller.h"
#include "recording/Recorder.h"
#include <algorithm>
#include <map>
#include <set>
#include <regex>
#include <thread>
#include <string>

namespace gocue::tests
{
namespace
{
    using namespace livemix;
    struct RestoreLanguage
    {
        Language language = currentLanguage();
        UiTranslator hook = uiTranslator.load();
        ~RestoreLanguage() { initialiseLanguage (language); uiTranslator.store (hook); }
    };
    struct TestDirectory
    {
        juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("LiveMixLanguage-" + juce::Uuid().toString());
        TestDirectory() { file.createDirectory(); }
        ~TestDirectory() { file.deleteRecursively(); }
    };
    bool hasKorean (const juce::String& text)
    {
        for (auto c : text)
            if ((c >= 0xac00 && c <= 0xd7a3) || (c >= 0x1100 && c <= 0x11ff) || (c >= 0x3130 && c <= 0x318f))
                return true;
        return false;
    }
    std::set<int> placeholders (const juce::String& text)
    {
        const auto utf8 = text.toStdString();
        const std::regex placeholder (R"(\{([0-9]+)\})");
        std::set<int> found;
        for (auto it = std::sregex_iterator (utf8.begin(), utf8.end(), placeholder); it != std::sregex_iterator(); ++it)
            found.insert (std::stoi ((*it)[1]));
        return found;
    }
    struct Token { juce::String text; bool string = false; };
    std::vector<Token> lex (const juce::String& source)
    {
        std::vector<Token> tokens;
        const auto chars = source.toUTF32();
        const int length = source.length();
        for (int i = 0; i < length;)
        {
            const auto c = chars[i];
            if (juce::CharacterFunctions::isWhitespace (c)) { ++i; continue; }
            if (c == '/' && chars[i + 1] == '/')
            {
                while (i < source.length() && chars[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && chars[i + 1] == '*')
            {
                const int end = source.indexOf (i + 2, "*/");
                i = end < 0 ? source.length() : end + 2;
                continue;
            }
            if (c == 'R' && chars[i + 1] == '"')
            {
                const int open = source.indexOfChar (i + 2, '(');
                const auto delimiter = source.substring (i + 2, open);
                const auto ending = ")" + delimiter + "\"";
                const int end = source.indexOf (open + 1, ending);
                tokens.push_back ({ source.substring (open + 1, end), true });
                i = end < 0 ? source.length() : end + ending.length();
                continue;
            }
            if (c == '"' || c == '\'')
            {
                const int start = i++;
                std::string bytes;
                while (i < source.length() && chars[i] != c)
                {
                    auto ch = chars[i++];
                    if (ch == '\\' && i < source.length())
                    {
                        ch = chars[i++];
                        if (ch == 'n') ch = '\n';
                        else if (ch == 'r') ch = '\r';
                        else if (ch == 't') ch = '\t';
                        else if (ch == 'x' && i + 1 < length
                                 && juce::CharacterFunctions::getHexDigitValue (chars[i]) >= 0
                                 && juce::CharacterFunctions::getHexDigitValue (chars[i + 1]) >= 0)
                        {
                            const auto byte = juce::CharacterFunctions::getHexDigitValue (chars[i]) * 16
                                            + juce::CharacterFunctions::getHexDigitValue (chars[i + 1]);
                            bytes += (char) byte;
                            i += 2;
                            continue;
                        }
                        else if (ch == 'u' || ch == 'U')
                        {
                            const int digits = ch == 'u' ? 4 : 8;
                            ch = (juce::juce_wchar) source.substring (i, i + digits).getHexValue64();
                            i += digits;
                        }
                    }
                    bytes += juce::String::charToString (ch).toStdString();
                }
                ++i;
                if (c == '"') tokens.push_back ({ juce::String::fromUTF8 (bytes.data(), (int) bytes.size()), true });
                else tokens.push_back ({ source.substring (start, i), false });
                continue;
            }
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '_')
            {
                const int start = i++;
                while (i < source.length() && (juce::CharacterFunctions::isLetterOrDigit (chars[i]) || chars[i] == '_')) ++i;
                tokens.push_back ({ source.substring (start, i), false });
            }
            else tokens.push_back ({ juce::String::charToString (chars[i++]), false });
        }
        return tokens;
    }
}

class LiveMixLocalisationTests : public juce::UnitTest
{
public:
    LiveMixLocalisationTests() : UnitTest ("LiveMix localisation", "LiveMix") {}
    void runTest() override
    {
        using namespace livemix;
        RestoreLanguage restore;
        beginTest ("Korean is the default; missing keys and formatting preserve values");
        expect (currentLanguage() == Language::ko);
        expectEquals (tr ("취소"), juce::String::fromUTF8 ("취소"));
        for (auto language : { Language::ko, Language::en, Language::ja })
        {
            initialiseLanguage (language);
            expectEquals (tr ("표에 없는 문장"), juce::String::fromUTF8 ("표에 없는 문장"));
            expectEquals (trf ("없는 키 {1}/{0}/{1}", "{1}", 42),
                          juce::String::fromUTF8 ("없는 키 42/{1}/42"));
            expectEquals (formatUiText ("{10} {0} {x} {12}", { "0","1","2","3","4","5","6","7","8","9","10" }),
                          juce::String ("10 0 {x} {12}"));
        }
        initialiseLanguage (Language::en);
        expectEquals (trf ("목표 {0} 대비 {1} LU", -14, 2), juce::String ("2 LU from target -14"));
        expectEquals (sharedTr ("취소"), juce::String ("Cancel"));
        uiTranslator.store (nullptr);
        expectEquals (sharedTrf ("숫자 {0}", 4), juce::String::fromUTF8 ("숫자 4"));

        beginTest ("Language precedence: override, preference, legacy user, Windows display language");
        struct Case { const char* saved; bool old; unsigned windows; const char* env; Language language; bool save; };
        const Case cases[] {
            { "", false, 0x0412, "", Language::ko, true },
            { "", false, 0x0411, "", Language::ja, true },
            { "", false, 0x0409, "", Language::en, true },
            { "", false, 0x0804, "", Language::en, true },
            { "", true, 0x0411, "", Language::ko, true },
            { "en", true, 0x0412, "", Language::en, false },
            { "ja", false, 0x0412, "", Language::ja, false },
            { "ko", false, 0x0411, "", Language::ko, false },
            { "en", true, 0x0412, "ja", Language::ja, false },
            { "ja", false, 0x0411, "ko", Language::ko, false },
            { "", true, 0x0412, "en", Language::en, false },
            { "invalid", false, 0x0411, "invalid", Language::ja, true },
            { "en", false, 0x0411, "JA", Language::en, false }
        };
        for (const auto& c : cases)
        {
            const auto chosen = chooseLanguage (c.saved, c.old, c.windows, c.env);
            expect (chosen.language == c.language);
            expect (chosen.save == c.save);
        }
        beginTest ("Installer-created settings stay new; language is persisted immediately; override is temporary");
        TestDirectory fresh, legacy, overrideOnly;
        {
            LiveMixSettings settings (fresh.file);
            settings.setBundledPluginsStamp ("installer-registration");
            expect (settings.resolveLanguage (0x0411, "") == Language::ja);
            expectEquals (settings.getLanguage(), juce::String ("ja"));
        }
        {
            LiveMixSettings settings (fresh.file);
            expect (settings.resolveLanguage (0x0412, "") == Language::ja);
            expect (settings.resolveLanguage (0x0412, "en") == Language::en);
            expectEquals (settings.getLanguage(), juce::String ("ja"));
            settings.setLanguage (Language::en);
        }
        {
            LiveMixSettings settings (fresh.file);
            expectEquals (settings.getLanguage(), juce::String ("en"));
            LiveMixSettings old (legacy.file);
            old.setLastRunVersion (""); // Presence, even an empty value, is the compatibility marker.
            expect (old.resolveLanguage (0x0411, "") == Language::ko);
            LiveMixSettings temporary (overrideOnly.file);
            expect (temporary.resolveLanguage (0x0411, "en") == Language::en);
            expect (temporary.getLanguage().isEmpty());
        }

        beginTest ("Restart argument parsing preserves the normal command line");
        expect (! restartAfterPid ("show.livemix"));
        expect (restartAfterPid ("--restart-after 123") == 123u);
        expect (restartAfterPid ("\"C:/sessions/my show.livemix\" --restart-after 4294967295 --safe-mode") == 4294967295u);
        for (const auto* bad : { "--restart-after", "--restart-after 0", "--restart-after -1",
                                "--restart-after abc", "--restart-after 12x", "--restart-after 4294967296",
                                "--restart-after 12 --restart-after 13", "--restart-after --safe-mode" })
            expect (! restartAfterPid (bad), bad);

        beginTest ("New channel, session and recording names follow language; saved names survive");
        const char* mic[] { "마이크 2", "Mic 2", "マイク 2" };
        const char* folders[] { "LiveMix 녹음", "LiveMix Recordings", "LiveMix 録音" };
        const char* session[] { "기본 세션", "Default session", "デフォルトセッション" };
        const char* tracks[][4] { { "내소리", "합방상대1", "합방상대2", "D_전체" },
                                 { "Me", "Partner1", "Partner2", "D_Mix" },
                                 { "自分", "コラボ相手1", "コラボ相手2", "D_全体" } };
        for (int i = 0; i < 3; ++i)
        {
            initialiseLanguage (static_cast<Language> (i));
            MixSession model;
            model.addChannel ("saved name");
            model.addChannel();
            expectEquals (model.channels[0].name, juce::String ("saved name"));
            expectEquals (model.channels[1].name, juce::String::fromUTF8 (mic[i]));
            expectEquals (defaultSessionName(), juce::String::fromUTF8 (session[i]));
            expectEquals (defaultRecordingDirectory().getFileName(), juce::String::fromUTF8 (folders[i]));
            for (int track = 0; track < 4; ++track)
                expectEquals (defaultRecordingTrackName (track), juce::String::fromUTF8 (tracks[i][track]));
        }
        initialiseLanguage (Language::ko);

        beginTest ("Elevated OBS helper carries language without reading or changing settings");
        for (auto language : { Language::ko, Language::en, Language::ja })
        {
            initialiseLanguage (language);
            const auto report = fresh.file.getChildFile ("installer reply.txt");
            const auto command = ObsPluginInstaller::elevatedCommandLine (report);
            const auto option = juce::ArgumentList ("LiveMix", command).getValueForOption ("--ui-language");
            expectEquals (option, juce::String (languageCode (language)));
            expect (chooseLanguage (option, true, 0, "").language == language);
            expect (ObsPluginInstaller::resultFile (command) == report);
        }
        initialiseLanguage (Language::ko);

        beginTest ("Collab host failure translates the display, preserving the server JSON reason");
        const auto koreanReason = juce::String::fromUTF8 ("공유기 자동 포트 열기(UPnP)가 꺼져 있습니다");
        for (auto language : { Language::ko, Language::en, Language::ja })
        {
            initialiseLanguage (language);
            const auto result = collab::CollabHostCheck::evaluate (false, {}, {});
            expectEquals (result.reason, koreanReason);
            expectEquals (result.displayReason(), translate (language, koreanReason.toRawUTF8()));
            expect (result.message().endsWith ("\n" + result.displayReason()));
        }
        initialiseLanguage (Language::ko);

        beginTest ("Embedded translation table: unique Korean, complete translations, matching placeholders");
        const auto& table = translationTable();
        expect (table.isArray());
        if (! table.isArray()) return;
        std::set<juce::String> keys;
        for (const auto& row : *table.getArray())
        {
            const auto ko = row["ko"].toString();
            expect (ko.isNotEmpty() && keys.insert (ko).second, "Duplicate/empty key: " + ko);
            expect (row["where"].toString().isNotEmpty(), ko);
            for (const auto* language : { "en", "ja" })
            {
                const auto translated = row[language].toString();
                expect (translated.isNotEmpty(), ko + ": " + language);
                expect (placeholders (ko) == placeholders (translated), ko + ": " + language);
            }
        }
        expect (keys.size() > 700);

        beginTest ("Source lexer decodes UTF-8 hex bytes and preserves literal backslashes");
        {
            const auto encoded = lex (R"cpp(label.setText ("-23 LUFS   \xEB\xB0\xA9\xEC\x86\xA1 (EBU R128)"))cpp");
            const auto found = std::find_if (encoded.begin(), encoded.end(), [] (const auto& token) { return token.string; });
            expect (found != encoded.end());
            if (found != encoded.end())
            {
                expectEquals (found->text, juce::String::fromUTF8 ("-23 LUFS   방송 (EBU R128)"));
                expect (hasKorean (found->text));
            }
            const auto mixed = lex (juce::String::fromUTF8 (R"cpp("한글 \xEB\xB0\xA9\xEC\x86\xA1\n끝")cpp"));
            expectEquals ((int) mixed.size(), 1);
            if (mixed.size() == 1) expectEquals (mixed[0].text, juce::String::fromUTF8 ("한글 방송\n끝"));
            const auto escaped = lex (R"cpp("\\xEB\\xB0\\xA9")cpp");
            expectEquals ((int) escaped.size(), 1);
            if (escaped.size() == 1)
            {
                expect (! hasKorean (escaped[0].text));
                expectEquals (escaped[0].text, juce::String (R"(\xEB\xB0\xA9)"));
            }
        }

        beginTest ("All source keys are in the table; no unused keys or unapproved Korean literals");
        const juce::File root (LIVEMIX_TEST_SOURCE_ROOT);
        expect (root.getChildFile ("livemix/src/Main.cpp").existsAsFile(), "Source root must be available");
        const auto allowance = juce::JSON::parse (root.getChildFile ("livemix/i18n/korean-allowlist.json"));
        expect (allowance.isArray());
        std::map<juce::String, int> allowed;
        if (allowance.isArray())
            for (const auto& row : *allowance.getArray())
            {
                expect (row["reason"].toString().isNotEmpty());
                const auto key = row["file"].toString() + "\n" + row["ko"].toString();
                expect (allowed.emplace (key, (int) row["count"]).second, "Duplicate allowance: " + key);
            }
        auto files = root.getChildFile ("livemix/src").findChildFiles (juce::File::findFiles, true, "*.cpp;*.h");
        for (const auto* shared : { "src/ui/PluginRuntimeHint.cpp", "src/ui/PluginScanListComponent.h",
                                    "src/audio/PluginHost.cpp", "src/audio/PluginChain.cpp", "src/audio/PluginScan.cpp",
                                    "src/app/Updater.cpp", "src/ui/PluginWindows.cpp" })
            files.add (root.getChildFile (shared));
        std::set<juce::String> used;
        for (const auto& file : files)
        {
            const auto sourceName = file.getRelativePathFrom (root).replaceCharacter ('\\', '/');
            const auto tokens = lex (file.loadFileAsString());
            for (size_t i = 0; i < tokens.size(); ++i)
            {
                if (! tokens[i].string) continue;
                auto literal = tokens[i].text;
                size_t end = i + 1;
                while (end < tokens.size() && tokens[end].string) literal += tokens[end++].text;
                const auto function = i >= 2 && tokens[i - 1].text == "(" ? tokens[i - 2].text : juce::String();
                if (function == "tr" || function == "trf" || function == "trIn" || function == "sharedTr" || function == "sharedTrf")
                {
                    used.insert (literal);
                    expect (keys.count (literal) == 1, sourceName + ": missing key: " + literal);
                    const auto slots = placeholders (literal);
                    if (function.endsWithChar ('f'))
                    {
                        int depth = 0, count = 0;
                        for (size_t j = end; j < tokens.size(); ++j)
                        {
                            if (tokens[j].string) continue;
                            const auto& t = tokens[j].text;
                            if (t == ")" && depth == 0) break;
                            if (t == "(" || t == "[" || t == "{") ++depth;
                            if (t == ")" || t == "]" || t == "}") --depth;
                            if (t == "," && depth == 0) ++count;
                        }
                        std::set<int> expected;
                        for (int j = 0; j < count; ++j) expected.insert (j);
                        expect (slots == expected, sourceName + ": format arguments: " + literal);
                    }
                    else expect (slots.empty(), sourceName + ": use trf: " + literal);
                }
                else if (hasKorean (literal))
                {
                    const auto key = sourceName + "\n" + literal;
                    auto entry = allowed.find (key);
                    expect (entry != allowed.end() && entry->second > 0, "Unapproved Korean: " + key);
                    if (entry != allowed.end()) --entry->second;
                }
                i = end - 1;
            }
        }
        for (const auto& key : keys) expect (used.count (key) == 1, "Unused key: " + key);
        for (const auto& [key, count] : allowed) expectEquals (count, 0, "Unused allowance: " + key);

        beginTest ("Translations are immutable and readable on worker threads");
        std::atomic<bool> matches { true };
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i)
            readers.emplace_back ([&matches]
            {
                for (int n = 0; n < 100; ++n)
                    if (translate (Language::en, "취소") != "Cancel"
                        || translate (Language::ja, "취소") != juce::String::fromUTF8 ("キャンセル"))
                        matches = false;
            });
        for (auto& thread : readers) thread.join();
        expect (matches.load());
    }
};
static LiveMixLocalisationTests localisationTests;
}
