#include "app/CoupangShortcut.h"
#include "app/Links.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/UpdateNotice.h"
#include "../livemix/src/ui/MainComponent.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::livemix
{
struct UpdateNoticeTestAccess
{
    static void stopTimer (MainComponent& main) { main.stopTimer(); }
    static bool isError (const MainComponent& main) { return main.noticeIsError; }
    static juce::String text (const MainComponent& main) { return main.noticeText.getText(); }
    static juce::TextButton& button (MainComponent& main) { return main.noticeCoupang; }
};
}

namespace gocue::tests
{
namespace
{
struct ShortcutTempRoot
{
    const juce::File parent = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File root = parent.getChildFile ("coupang-shortcut-test-" + juce::Uuid().toString());
    ShortcutTempRoot() { root.createDirectory(); }
    ~ShortcutTempRoot()
    {
        if (root.isAChildOf (parent) && root.getFileName().startsWith ("coupang-shortcut-test-"))
            root.deleteRecursively();
    }
};

juce::String ko (const char* text) { return juce::String::fromUTF8 (text); }

class CoupangShortcutTests final : public juce::UnitTest
{
public:
    CoupangShortcutTests() : UnitTest ("Coupang shortcut and update notices", "Enqueue") {}

    void runTest() override
    {
        using namespace CoupangShortcut;
        beginTest ("New installation / same version / update, with and without either shortcut");
        for (const auto& previous : juce::StringArray { "", "2.0", "1.0" })
            for (bool shortcutExists : { false, true })
            {
                const auto decision = decideUpdate (previous, "2.0", shortcutExists);
                expect (decision.announce == (previous == "1.0"));
                expect (decision.offerShortcut == (previous == "1.0" && ! shortcutExists));
            }

        beginTest ("Creation writes the installer URL and a Unicode absolute icon path, verified through Win32 INI");
        {
            ShortcutTempRoot temp;
            const auto desktop = temp.root.getChildFile (ko ("테스트 바탕화면"));
            const auto icon = temp.root.getChildFile (ko ("한글 설치 폴더/coupang.ico"));
            expect (desktop.createDirectory().wasOk());
            expect (icon.getParentDirectory().createDirectory().wasOk());
            expect (icon.replaceWithText ("test icon"));
            expect (! existsOn (desktop));
            const auto result = createOn (desktop, icon);
            expect (result.wasOk(), result.getErrorMessage());
            expect (existsOn (desktop));
            const auto shortcut = desktop.getChildFile (ko ("쿠팡.url"));
            const auto contents = shortcut.loadFileAsString();
            expect (contents.contains ("[InternetShortcut]"));
            expect (contents.contains (juce::String ("URL=") + Links::coupangShortcut));
            expect (contents.contains ("IconFile=" + icon.getFullPathName()));
            expect (contents.contains ("IconIndex=0"));
           #if JUCE_WINDOWS
            for (const auto& item : { std::pair<const wchar_t*, juce::String> { L"URL", Links::coupangShortcut },
                                     { L"IconFile", icon.getFullPathName() }, { L"IconIndex", "0" } })
            {
                wchar_t value[2048] {};
                GetPrivateProfileStringW (L"InternetShortcut", item.first, L"", value, 2048,
                                          shortcut.getFullPathName().toWideCharPointer());
                expectEquals (juce::String (value), item.second);
            }
           #endif
        }

        beginTest ("A missing icon omits both icon keys");
        {
            ShortcutTempRoot temp;
            expect (createOn (temp.root, temp.root.getChildFile ("missing.ico")).wasOk());
            const auto contents = temp.root.getChildFile (ko ("쿠팡.url")).loadFileAsString();
            expect (contents.contains (Links::coupangShortcut));
            expect (! contents.contains ("IconFile"));
            expect (! contents.contains ("IconIndex"));
        }

        for (const auto& shortcutName : juce::StringArray { ko ("쿠팡.url"), "Coupang.url" })
        {
            beginTest ("Existing " + shortcutName + " is detected and preserved byte for byte regardless of contents");
            ShortcutTempRoot temp;
            const auto existing = temp.root.getChildFile (shortcutName);
            const juce::String contents = "[InternetShortcut]\r\nURL=https://example.com/another-vendor\r\n";
            expect (existing.replaceWithText (contents));
            juce::MemoryBlock before, after;
            expect (existing.loadFileAsData (before));
            expect (existsOn (temp.root));
            expect (createOn (temp.root, temp.root.getChildFile ("absent.ico")).wasOk());
            expect (existing.loadFileAsData (after));
            expect (before == after);
            expectEquals (temp.root.getNumberOfChildFiles (juce::File::findFiles), 1);
            expect (existing.replaceWithText (""));
            expect (existsOn (temp.root));
            expect (createOn (temp.root, {}).wasOk());
            expectEquals (existing.getSize(), juce::int64 (0));
        }

        beginTest ("A missing desktop fails without creating directories or files");
        {
            ShortcutTempRoot temp;
            const auto missing = temp.root.getChildFile ("absent/desktop");
            const auto result = createOn (missing, {});
            expect (result.failed());
            expectEquals (result.getErrorMessage(), ko ("바탕화면 폴더가 없습니다."));
            expect (! missing.exists());
        }

        beginTest ("Styled alert: OK owns Enter/Esc, the Coupang button has no shortcuts or keyboard focus");
        {
            struct Style : GoCueLookAndFeel
            {
                juce::String lastMessage;
                juce::AlertWindow* createAlertWindow (const juce::String& title, const juce::String& message,
                    const juce::String& button1, const juce::String& button2, const juce::String& button3,
                    juce::MessageBoxIconType icon, int numButtons, juce::Component* associated) override
                {
                    lastMessage = message;
                    return GoCueLookAndFeel::createAlertWindow (title, message, button1, button2, button3, icon, numButtons, associated);
                }
            } style;
            for (bool offer : { false, true })
            {
                auto alert = UpdateNotice::create (style, "version message", offer);
                expectEquals (alert->getNumButtons(), offer ? 2 : 1);
                expectEquals (alert->getName(), ko ("업데이트 완료"));
                auto* ok = alert->getButton (0);
                expectEquals (ok->getButtonText(), ko ("확인"));
                expect (ok->isRegisteredForShortcut (juce::KeyPress (juce::KeyPress::returnKey)));
                expect (ok->isRegisteredForShortcut (juce::KeyPress (juce::KeyPress::escapeKey)));
                expectEquals (style.lastMessage, offer ? "version message\n\n" + guidance + "\n" + disclosure
                                                        : juce::String ("version message"));
                if (offer)
                {
                    auto* button = alert->getButton (1);
                    expectEquals (button->getButtonText(), buttonText);
                    expectEquals (button->getCommandID(), 2);
                    expect (! button->getWantsKeyboardFocus());
                    for (const int key : { juce::KeyPress::returnKey, juce::KeyPress::escapeKey, juce::KeyPress::spaceKey })
                        expect (! button->isRegisteredForShortcut (juce::KeyPress (key)));
                    for (const auto letter : buttonText)
                        expect (! button->isRegisteredForShortcut (juce::KeyPress (int (letter))));
                }
            }
        }
        liveMixNotice();
    }

private:
    void liveMixNotice()
    {
        using namespace livemix;
        using Access = UpdateNoticeTestAccess;
        beginTest ("LiveMix adds a neutral update line, then creates only on click and hides the button");
        ShortcutTempRoot temp;
        LiveMixSettings settings (temp.root.getChildFile ("settings"));
        MixEngine engine;
        engine.prepare (48000.0, 256);
        MixDocument document (engine);
        document.applyToEngine();
        ObsPluginActions actions;
        actions.roots = [root = temp.root]
        {
            ObsPluginInstaller::Roots roots;
            roots.programData = root.getChildFile ("program-data");
            roots.obsInstallDir = root.getChildFile ("obs");
            roots.bundledPlugin = root.getChildFile ("bundle");
            roots.isObsRunning = [] { return false; };
            return roots;
        };
        actions.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::failed; };
        MainComponent main (document, settings, std::move (actions));
        Access::stopTimer (main);
        main.hideNotice();
        const int modals = juce::Component::getNumCurrentlyModalComponents();
        const auto desktop = temp.root.getChildFile ("desktop");
        expect (desktop.createDirectory().wasOk());
        main.setSessionNote ("session note", false);
        main.setStartupNote ("safe mode", false, true);
        main.setUpdateNotice ("1.0", "2.0", desktop, {});
        auto& button = Access::button (main);
        const auto versionText = ko ("LiveMix가 1.0 → 2.0(으)로 업데이트되었습니다.");
        expect (button.isVisible());
        expect (! button.getWantsKeyboardFocus());
        expect (! Access::isError (main));
        expectEquals (Access::text (main), "session note\nsafe mode\n" + versionText + " "
            + CoupangShortcut::guidance + " " + CoupangShortcut::disclosure);
        expect (! CoupangShortcut::existsOn (desktop));
        button.onClick();
        expect (CoupangShortcut::existsOn (desktop));
        expect (! button.isVisible());
        expect (! Access::isError (main));
        expectEquals (Access::text (main), "session note\nsafe mode\n" + versionText
            + ko (" 바탕화면에 쿠팡 바로가기를 만들었습니다."));
        expectEquals (juce::Component::getNumCurrentlyModalComponents(), modals);

        beginTest ("LiveMix failure keeps the action and other lines; retry clears only its error");
        const auto missing = temp.root.getChildFile ("retry-desktop");
        main.setUpdateNotice ("1.0", "2.0", missing, {});
        button.onClick();
        expect (button.isVisible());
        expect (Access::isError (main));
        expect (Access::text (main).endsWith (ko ("바탕화면 폴더가 없습니다.")));
        expect (Access::text (main).startsWith ("session note\nsafe mode\n"));
        expect (missing.createDirectory().wasOk());
        button.onClick();
        expect (! button.isVisible());
        expect (! Access::isError (main));
        main.setSessionNote ("device failure", true);
        main.setUpdateNotice ("1.0", "2.0", desktop, {});
        expect (Access::isError (main));
        expect (! button.isVisible());

        beginTest ("LiveMix close clears the update/action; first run and same version add no line");
        main.hideNotice();
        expect (Access::text (main).isEmpty());
        expect (! button.isVisible());
        for (const auto& previous : juce::StringArray { "", "2.0" })
        {
            main.setUpdateNotice (previous, "2.0", missing, {});
            expect (Access::text (main).isEmpty());
            expect (! button.isVisible());
        }

        beginTest ("LiveMix never offers to overwrite a foreign English shortcut");
        const auto foreign = temp.root.getChildFile ("foreign-desktop");
        expect (foreign.createDirectory().wasOk());
        expect (foreign.getChildFile ("Coupang.url").replaceWithText ("foreign shortcut"));
        main.setUpdateNotice ("1.0", "2.0", foreign, {});
        expectEquals (Access::text (main), versionText);
        expect (! button.isVisible());
        expectEquals (juce::Component::getNumCurrentlyModalComponents(), modals);
    }
};

static CoupangShortcutTests coupangShortcutTests;
}
}
