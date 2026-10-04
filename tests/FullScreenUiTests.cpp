#include "VolumeCueUiTestSupport.h"
#include "ui/KeyCapture.h"
#include "ui/ShortcutSettingsTab.h"
#include "model/Hotkeys.h"

namespace gocue::tests
{
namespace
{
using namespace volume_ui;
using K = juce::KeyPress;
constexpr auto fullScreenID = "view.toggleFullScreen";

class FullScreenUiTests : public juce::UnitTest
{
public:
    FullScreenUiTests() : UnitTest ("FullScreen UI and commands", "Enqueue") {}
    void runTest() override
    {
        Fixture f; // no native window, visibility or activation calls
        auto& service = f.main->getShortcutService();
        bool active = false;
        int toggles = 0;
        f.main->onToggleFullScreen = [&] { active = ! active; ++toggles; };
        f.main->isFullScreenActive = [&] { return active; };

        beginTest ("edit menu follows inspector; constant label and tick follow the main-window callback");
        const auto checkMenu = [&] (bool ticked)
        {
            juce::ApplicationCommandInfo info (CommandIDs::toggleFullScreen);
            f.main->getCommandInfo (CommandIDs::toggleFullScreen, info);
            expectEquals (info.shortName, ko ("전체 화면"));
            expect ((info.flags & juce::ApplicationCommandInfo::isDisabled) == 0);
            expect (((info.flags & juce::ApplicationCommandInfo::isTicked) != 0) == ticked);
            auto menu = f.main->getMenuForIndex (1, ko ("편집"));
            juce::CommandID previous = 0;
            int found = 0;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            {
                const auto& item = it.getItem();
                if (item.itemID == CommandIDs::toggleFullScreen)
                {
                    ++found;
                    expectEquals (previous, juce::CommandID (CommandIDs::toggleInspector));
                    expectEquals (item.text, ko ("전체 화면"));
                    expect (item.isEnabled && item.isTicked == ticked);
                }
                previous = item.itemID;
            }
            expectEquals (found, 1);
        };
        checkMenu (false);
        expect (f.command (CommandIDs::toggleFullScreen));
        expectEquals (toggles, 1);
        checkMenu (true);
        f.command (CommandIDs::toggleShowMode);
        checkMenu (true);
        expect (f.command (CommandIDs::toggleFullScreen));
        expectEquals (toggles, 2);
        checkMenu (false);
        f.command (CommandIDs::toggleShowMode);

        beginTest ("F11 routed from the big-view scope invokes the main-window callback");
        {
            HiddenWindow bigView (f.engine, f.document().cues, f.settings);
            expect (f.key (K (K::F11Key), bigView));
            expectEquals (toggles, 3);
            expect (! bigView.isKioskMode());
            f.release (K (K::F11Key));
        }

        beginTest ("inspector accepts new F11 cue bindings and shows no disabled conflict for the default");
        Cue cue;
        cue.type = CueType::control;
        cue.control.kind = ControlKind::wait;
        cue.control.seconds = 5;
        cue.name = "F11 wait";
        f.document().cues.add (cue);
        f.document().cues.setSelectedIndex (0);
        dispatch();
        auto* tabs = child<juce::TabbedComponent> (f.inspector());
        expect (tabs != nullptr);
        if (tabs == nullptr) return;
        tabs->setCurrentTabIndex (0); // control cues initially select their action tab
        auto* button = child<KeyCaptureButton> (f.inspector());
        expect (button != nullptr && button->validate != nullptr);
        if (button == nullptr || ! button->validate) return;
        const K f11 (K::F11Key);
        const auto conflictLabel = [&]
        {
            return child<juce::Label> (f.inspector(), [] (const auto& label)
                { return label.getText().contains (ko ("단축키와 충돌 → 비활성")); });
        };
        expect (! Hotkeys::isReservedKey (f11));
        expect (button->validate (f11).isEmpty());
        button->onHotkeyChanged (f11.getTextDescription());
        dispatch();
        expectEquals (f.document().cues.get (0).hotkey, f11.getTextDescription());
        expect (conflictLabel() == nullptr);
        expect (button->validate (K ('P')).startsWith (ko ("앱이 쓰는 키입니다: ")));

        beginTest ("project F11 routes to its cue through MainComponent, leaving the fullscreen callback untouched");
        const int before = toggles;
        expect (f.key (f11, *f.main));
        f.release (f11);
        expectEquals (toggles, before);
        expect (ReopenLastProjectTestAccess::controller (*f.main).hasPendingStarts());
        ReopenLastProjectTestAccess::controller (*f.main).cancelPending();

        beginTest ("settings and inspector keep cue priority for defaults and all fullscreen overrides");
        const ShortcutSettingsModel::Cues cues { { "1 F11 wait", f11 } };
        const auto inspect = [&] { return ShortcutSettingsModel::inspect (service, fullScreenID, f11, cues); };
        const auto yielding = ko ("현재 프로젝트 큐 핫키가 우선: 1 F11 wait → 이 프로젝트에서는 이 키로 실행되지 않음");
        const auto cueDisabled = ko ("현재 프로젝트 큐와 충돌: 1 F11 wait → 큐 핫키 비활성");
        expect (inspect().conflicts == juce::StringArray { yielding });
        expect (service.setKeys (fullScreenID, { f11 }).wasOk());
        dispatch();
        expect (inspect().conflicts == juce::StringArray { yielding });
        expect (button->validate (f11).isEmpty() && conflictLabel() == nullptr);
        const K letter ('J');
        expect (service.setKeys (fullScreenID, { letter }).wasOk());
        expect (ShortcutSettingsModel::inspect (service, fullScreenID, letter, { { "3 Letter", letter } }).conflicts
                == juce::StringArray { ko ("현재 프로젝트 큐 핫키가 우선: 3 Letter → 이 프로젝트에서는 이 키로 실행되지 않음") });
        expect (button->validate (letter).isEmpty());
        expect (service.restoreCommandDefaults (fullScreenID).wasOk());
        dispatch();
        expect (inspect().conflicts == juce::StringArray { yielding });
        expect (button->validate (f11).isEmpty() && conflictLabel() == nullptr);

        beginTest ("F11 learn detects the yielding fullscreen mapping, then move assigns preview and disables the cue");
        const auto moveIssues = ShortcutSettingsModel::inspect (service, "transport.preview", f11, cues);
        expectEquals (moveIssues.commandOwner, juce::String (fullScreenID));
        expect (moveIssues.conflicts.contains (ko ("다른 기능에서 사용: 전체 화면")));
        expect (moveIssues.conflicts.contains (cueDisabled));
        expect (service.setKeys ("transport.preview", { f11 }, ShortcutService::ConflictPolicy::reject).failed());
        expect (service.setKeys ("transport.preview", { f11 }, ShortcutService::ConflictPolicy::move).wasOk());
        dispatch();
        expect (service.getKeys ("transport.preview") == ShortcutKeys { f11 });
        expect (service.getKeys (fullScreenID).isEmpty());
        const auto movedIssues = ShortcutSettingsModel::inspect (service, "transport.preview", f11, cues);
        expect (movedIssues.commandOwner.isEmpty());
        expect (movedIssues.conflicts == juce::StringArray { cueDisabled });
        ShortcutKeyContext context;
        context.cueHotkeys = cues;
        const auto owner = service.resolveKeyOwner (f11, context);
        expect (owner.commandID == CommandIDs::preview && owner.reason == ShortcutKeyOwner::Reason::commandOverCue);
        expectEquals (button->validate (f11), ko ("이 PC의 미리듣기 단축키와 충돌 → 비활성"));
        auto* label = conflictLabel();
        expect (label != nullptr && label->getText() == ko ("이 PC의 미리듣기 단축키와 충돌 → 비활성"));

        beginTest ("other command conflicts and reserved-key rejection keep the previous wording");
        const K f12 (K::F12Key);
        expect (service.setKeys ("transport.preview", { f12 }).wasOk());
        expectEquals (button->validate (f12), ko ("이 PC의 미리듣기 단축키와 충돌 → 비활성"));
        button->onHotkeyChanged (f12.getTextDescription());
        dispatch();
        label = conflictLabel();
        expect (label != nullptr && label->getText() == ko ("이 PC의 미리듣기 단축키와 충돌 → 비활성"));
        const auto issues = ShortcutSettingsModel::inspect (service, "transport.preview", f12, { { "2 Preview", f12 } });
        expect (issues.conflicts == juce::StringArray { ko ("현재 프로젝트 큐와 충돌: 2 Preview → 큐 핫키 비활성") });
        expect (button->validate (K ('P')).startsWith (ko ("앱이 쓰는 키입니다: ")));
        f.main->onToggleFullScreen = {};
        f.main->isFullScreenActive = {};
    }
};
static FullScreenUiTests fullScreenUiTests;
} // namespace
} // namespace gocue::tests
