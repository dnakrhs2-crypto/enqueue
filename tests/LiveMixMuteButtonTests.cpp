#include "ui/MasterCard.h"
#include "../livemix/src/ui/MainComponent.h"
#include "ui/FxDrawer.h"
#include "ui/LiveMixLookAndFeel.h"
#include "ui/TopBar.h"

namespace gocue::tests
{
using namespace gocue::livemix;

/** The top bar's 마이크 뮤트 / FX 뮤트 (2026-10-30, a user's request): every mic / every FX at once, no hotkey or
    '뮤트그룹' mark needed; their hotkeys; and how they sit with the mute groups. */
class LiveMixMuteButtonTests : public juce::UnitTest
{
public:
    LiveMixMuteButtonTests() : juce::UnitTest ("LiveMix mute group buttons", "LiveMix") {}

    struct Rig
    {
        struct IsolatedFolder
        {
            juce::File file = juce::File::createTempFile ("-livemix-mutebtn");
            ~IsolatedFolder() { file.deleteRecursively(); }
        } folder;
        LiveMixSettings settings { folder.file };
        MixEngine engine { "Local\\LiveMix.MuteButtonTest." + juce::Uuid().toString() };
        MixDocument document { engine };
        ObsPluginActions actions;
        std::unique_ptr<MainComponent> main;

        explicit Rig (bool fxInGroup, int mics = 1)
        {
            settings.setMicMuteHotkey ({});
            settings.setFxMuteHotkey ({});
            settings.setWindowHotkey ({});
            settings.setMicAllMuteHotkey ({});
            settings.setFxAllMuteHotkey ({});
            document.applyToEngine();
            while ((int) document.getSession().channels.size() < mics)
                document.addChannel();
            while (document.getSession().fx.size() < 2)
                document.addFx();
            if (fxInGroup)
                for (const auto& id : fxIds())
                    document.setFxMuteGroup (id, true);
            actions.roots = [path = folder.file] { return ObsPluginInstaller::Roots { path, path, path, [] { return false; }, {} }; };
            actions.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
            main = std::make_unique<MainComponent> (document, settings, actions);   // no native window or audio device
            main->setSize (1440, 900);
        }

        std::vector<juce::Uuid> fxIds() const
        {
            std::vector<juce::Uuid> ids;
            for (const auto& f : document.getSession().fx)
                ids.push_back (f.id);
            return ids;
        }

        MuteGroups& groups() { return main->getMuteGroups(); }
        juce::TextButton* button (const juce::String& id) { return dynamic_cast<juce::TextButton*> (find (*main, id)); }

        bool everyMicMuted()
        {
            for (const auto& c : document.getSession().channels)
                if (! groups().mutes (c)) return false;
            return true;
        }

        bool anyMicMuted()
        {
            for (const auto& c : document.getSession().channels)
                if (groups().mutes (c)) return true;
            return false;
        }

        bool everyFxMuted()
        {
            for (const auto& f : document.getSession().fx)
                if (! groups().mutes (f)) return false;
            return true;
        }

        bool anyFxMuted()
        {
            for (const auto& f : document.getSession().fx)
                if (groups().mutes (f)) return true;
            return false;
        }

        TopBar* topBar()
        {
            for (auto* child : main->getChildren())
                if (auto* t = dynamic_cast<TopBar*> (child))
                    return t;
            return nullptr;
        }

        bool fxPanelOpen()
        {
            for (auto* child : main->getChildren())
                if (auto* v = dynamic_cast<juce::Viewport*> (child); v != nullptr && dynamic_cast<FxDrawer*> (v->getViewedComponent()) != nullptr)
                    return v->isVisible();
            return false;
        }

        bool says (const juce::String& text) { return anyLabelContains (*main, text); }

        static juce::Component* find (juce::Component& root, const juce::String& id)
        {
            if (root.getComponentID() == id)
                return &root;
            for (auto* child : root.getChildren())
                if (auto* found = find (*child, id))
                    return found;
            return nullptr;
        }

        static bool anyLabelContains (juce::Component& root, const juce::String& text)
        {
            if (auto* label = dynamic_cast<juce::Label*> (&root); label != nullptr && label->isVisible() && label->getText().contains (text))
                return true;
            for (auto* child : root.getChildren())
                if (anyLabelContains (*child, text))
                    return true;
            return false;
        }
    };

    void runTest() override
    {
        LiveMixLookAndFeel lookAndFeel;
        struct RestoreLookAndFeel
        {
            juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
            ~RestoreLookAndFeel() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
        } restoreLookAndFeel;
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);   // the app's text measurements and painting

        using Group = MuteGroups::Group;

        beginTest ("FX 뮤트 mutes every FX with no '뮤트그룹' mark set, lit red, and gives them back as they were");
        {
            Rig rig (false);
            auto* fx = rig.button ("fx-mute-group");
            auto* mic = rig.button ("mic-mute-group");
            expect (fx != nullptr && mic != nullptr);
            if (fx == nullptr || mic == nullptr) return;
            expect (fx->isVisible() && mic->isVisible());
            expectEquals (fx->getButtonText(), ko ("FX 뮤트"));
            expectEquals (mic->getButtonText(), ko ("마이크 뮤트"));
            expect (! fx->getToggleState() && ! rig.anyFxMuted());

            const auto second = rig.fxIds()[1];
            rig.document.setFxOn (second, false);   // its own switch, before: it must stay as it is

            fx->onClick();
            expect (rig.groups().isAllMuted (Group::fx));
            expect (rig.engine.areAllFxMuted() && ! rig.engine.areAllChannelsMuted());   // the engine's own switch
            expect (rig.everyFxMuted());
            expect (! rig.anyMicMuted());
            expect (fx->getToggleState() && ! mic->getToggleState());
            expect (fx->findColour (juce::TextButton::buttonOnColourId) == livemix::Palette::dangerFill);   // white text reads on it
            expect (rig.says (ko ("모든 FX 뮤트 중")));
            expect (! rig.fxPanelOpen());
            expect (rig.document.getSession().fx[0].on && ! rig.document.getSession().fx[1].on);   // switches untouched

            fx->onClick();
            expect (! rig.groups().isAnyMuted (Group::fx));
            expect (! rig.engine.areAllFxMuted());
            expect (! rig.anyFxMuted());
            expect (! fx->getToggleState());
            expect (rig.says (ko ("뮤트 해제")));
            expect (rig.document.getSession().fx[0].on && ! rig.document.getSession().fx[1].on);   // back as it was
        }

        beginTest ("마이크 뮤트 mutes every mic with no mark set; each mic's own switch is kept");
        {
            Rig rig (false, 2);
            auto* mic = rig.button ("mic-mute-group");
            expect (mic != nullptr);
            if (mic == nullptr) return;
            const auto second = rig.document.getSession().channels[1].id;
            rig.document.setChannelOn (second, false);

            mic->onClick();
            expect (rig.groups().isAllMuted (Group::mic) && ! rig.groups().isAnyMuted (Group::fx));
            expect (rig.everyMicMuted() && ! rig.anyFxMuted());
            expect (mic->getToggleState());
            expect (rig.says (ko ("모든 마이크 뮤트 중")));

            mic->onClick();
            expect (! rig.anyMicMuted() && ! mic->getToggleState());
            expect (rig.document.getSession().channels[0].on && ! rig.document.getSession().channels[1].on);
        }

        beginTest ("the 전체 뮤트 hotkeys do what the switches do");
        {
            Rig rig (false, 2);
            auto* fx = rig.button ("fx-mute-group");
            auto* mic = rig.button ("mic-mute-group");
            expect (fx != nullptr && mic != nullptr);
            if (fx == nullptr || mic == nullptr) return;

            rig.main->hotkeyPressed (MainComponent::allFxHotkeyId);
            expect (rig.everyFxMuted() && fx->getToggleState());
            rig.main->hotkeyPressed (MainComponent::allMicsHotkeyId);
            expect (rig.everyMicMuted() && mic->getToggleState());
            expect (rig.says (ko ("모든 마이크·모든 FX 뮤트 중")));

            rig.main->hotkeyPressed (MainComponent::allFxHotkeyId);
            expect (! rig.anyFxMuted() && ! fx->getToggleState());
            expect (rig.everyMicMuted());   // the other kind stays
            rig.main->hotkeyPressed (MainComponent::allMicsHotkeyId);
            expect (! rig.anyMicMuted() && ! mic->getToggleState());

            // the ids never clash with the other global hotkeys
            expectGreaterThan (MainComponent::allMicsHotkeyId, TrackHotkeys::lastId);
            expectEquals (MainComponent::allFxHotkeyId, MainComponent::allMicsHotkeyId + 1);
        }

        beginTest ("a mute group shows on its kind's switch as a red edge; pressing then mutes everything and never lets the group go");
        {
            Rig rig (true);
            auto* fx = dynamic_cast<MuteSwitch*> (rig.button ("fx-mute-group"));
            expect (fx != nullptr);
            if (fx == nullptr) return;
            const auto first = rig.fxIds()[0], second = rig.fxIds()[1];
            rig.document.setFxMuteGroup (second, false);   // the group: FX 1 only

            rig.main->hotkeyPressed (2);   // the FX mute group hotkey
            expect (rig.groups().isMuted (Group::fx) && ! rig.groups().isAllMuted (Group::fx));
            expect (fx->isPartial() && ! fx->getToggleState());   // only some FX: the edge, not the fill
            expect (rig.says (ko ("FX 뮤트그룹 뮤트 중")));
            expect (rig.groups().mutes (*rig.document.getSession().findFx (first)));
            expect (! rig.groups().mutes (*rig.document.getSession().findFx (second)));

            fx->onClick();   // "mute everything": the rest goes silent, the group's FX stays silent
            expect (rig.groups().isAllMuted (Group::fx) && rig.groups().isMuted (Group::fx));
            expect (rig.everyFxMuted() && fx->getToggleState());

            fx->onClick();   // released: back to exactly what was muted before - the group's FX still muted
            expect (! rig.groups().isAllMuted (Group::fx) && rig.groups().isMuted (Group::fx));
            expect (rig.groups().mutes (*rig.document.getSession().findFx (first)));
            expect (! rig.groups().mutes (*rig.document.getSession().findFx (second)));
            expect (fx->isPartial() && ! fx->getToggleState());

            rig.main->hotkeyPressed (2);   // the group's own hotkey releases it
            expect (! rig.groups().isAnyMuted (Group::fx) && ! rig.anyFxMuted());
            expect (! fx->isPartial() && ! fx->getToggleState());
        }

        beginTest ("a mute group's hotkey with nobody in it mutes nothing and says where the '뮤트그룹' marks are");
        {
            Rig rig (false);
            auto* fx = rig.button ("fx-mute-group");
            auto* mic = rig.button ("mic-mute-group");
            expect (fx != nullptr && mic != nullptr);
            if (fx == nullptr || mic == nullptr) return;

            rig.main->hotkeyPressed (2);
            expect (! rig.groups().isAnyMuted (Group::fx) && ! fx->getToggleState());
            expect (rig.says (ko ("FX 뮤트그룹에 넣은 FX가 없습니다")));
            expect (rig.fxPanelOpen());   // the FX panel shows its '뮤트그룹' chip

            rig.main->hotkeyPressed (1);
            expect (! rig.groups().isAnyMuted (Group::mic) && ! mic->getToggleState());
            expect (rig.says (ko ("마이크 뮤트그룹에 넣은 마이크가 없습니다")));

            rig.main->toggleMuteGroup (Group::fx);   // the tray menu's item: the same
            expect (! rig.groups().isAnyMuted (Group::fx));

            // the switch mutes everything even with nobody in the group (it needs no mark)
            fx->onClick();
            expect (rig.groups().isAllMuted (Group::fx) && rig.everyFxMuted() && fx->getToggleState());
            fx->onClick();
            expect (! rig.groups().isAnyMuted (Group::fx) && ! fx->getToggleState());
        }

        beginTest ("a session opened releases everything");
        {
            Rig rig (false);
            rig.main->toggleMuteAll (Group::fx);
            rig.main->toggleMuteAll (Group::mic);
            expect (rig.groups().isAllMuted (Group::fx) && rig.groups().isAllMuted (Group::mic));
            rig.groups().reset();   // what opening / a new session does
            expect (! rig.groups().isAnyMuted (Group::fx) && ! rig.groups().isAnyMuted (Group::mic));
            auto* fx = rig.button ("fx-mute-group");
            expect (fx != nullptr && ! fx->getToggleState());
        }

        beginTest ("the 전체 뮤트 hotkeys are kept in the settings");
        {
            Rig rig (false);
            rig.settings.setMicAllMuteHotkey ("ctrl + alt + F11");
            rig.settings.setFxAllMuteHotkey ("ctrl + alt + F12");
            rig.settings.saveIfNeeded();
            LiveMixSettings reloaded (rig.folder.file);
            expectEquals (reloaded.getMicAllMuteHotkey(), juce::String ("ctrl + alt + F11"));
            expectEquals (reloaded.getFxAllMuteHotkey(), juce::String ("ctrl + alt + F12"));
            rig.settings.setMicAllMuteHotkey ({});
            rig.settings.setFxAllMuteHotkey ({});
        }

        beginTest ("a tall window (the plugin manager, 960 x 760) is placed title bar and all inside small screens");
        {
            const juce::BorderSize<int> frame (31, 8, 8, 8);
            for (const auto screen : { juce::Rectangle<int> (0, 0, 1280, 688),       // 1920 x 1080 at 150%, taskbar off
                                       juce::Rectangle<int> (0, 0, 1366, 728),       // 1366 x 768
                                       juce::Rectangle<int> (1920, -40, 1920, 1040) })   // a big second display
            {
                const auto inside = placeWithFrame ({ 960, 760 }, screen.getCentre(), screen, frame);
                const auto whole = frame.addedTo (inside);
                expect (screen.contains (whole), screen.toString() + " -> " + whole.toString());
                expectGreaterOrEqual (whole.getY(), screen.getY() + 12);   // the title bar on the screen
                expect (inside.getWidth() <= 960 && inside.getHeight() <= 760);
                if (screen.getHeight() >= 760 + 31 + 8 + 24)
                    expectEquals (inside.getHeight(), 760);   // room enough: the size it asked for
                expectGreaterOrEqual (inside.getHeight(), 480);   // its smallest size still fits these screens
            }
        }

        beginTest ("beside the switches a 24-bit ASIO status keeps its bit depth: the meter bar (then the logo's word) gives way first");
        {
            Rig rig (false);
            auto* top = rig.topBar();
            expect (top != nullptr);
            if (top == nullptr) return;
            MixEngine::DeviceFormat format;
            format.kind = MixEngine::DeviceFormat::Kind::asio;
            format.inputBits = 24;
            top->setDevices ({ "Yamaha Steinberg USB ASIO" }, "Yamaha Steinberg USB ASIO", "ASIO");
            const auto meterShown = [top]   // the CPU meter: the bar's only plain component
            {
                for (auto* c : top->getChildren())
                    if (c->isVisible() && dynamic_cast<juce::Label*> (c) == nullptr && dynamic_cast<juce::Button*> (c) == nullptr
                        && dynamic_cast<juce::ComboBox*> (c) == nullptr)
                        return true;
                return false;
            };

            // the widths people run it at (1280 = 1920 x 1080 at 150%); right at a mode's edge (1220, 700) the status may
            // still shorten to its minimum, the old staged fallback
            for (int width : { 1920, 1440, 1366, 1280, 1219, 1000, 900, 460 })
            {
                rig.main->setSize (width, 900);
                for (const double load : { 0.04, 0.09, 0.10, 0.57 })   // one and two digits: the bar must not move
                {
                    const auto before = [top] { std::vector<juce::Rectangle<int>> r; for (auto* c : top->getChildren()) r.push_back (c->isVisible() ? c->getBounds() : juce::Rectangle<int>()); return r; };
                    const auto shape = before();
                    top->setStatus (48000.0, 128, 5.3, load, true, format);
                    if (load > 0.04)
                        expect (before() == shape, "the CPU figure moved the bar @" + juce::String (width) + " " + juce::String (load));
                }
                top->setStatus (48000.0, 128, 5.3, 0.04, true, format);
                auto* status = dynamic_cast<juce::Label*> (top->findChildWithID ("device-status"));
                expect (status != nullptr);
                if (status == nullptr) continue;
                expect (status->getText().contains (ko ("24비트")), status->getText() + " @" + juce::String (width));
                expectGreaterOrEqual (status->getWidth(), labelWidthForText (*status, status->getText()), "status fit @" + juce::String (width));
                if (width == 1920 || width == 1440 || width == 1000)
                    expect (meterShown(), "the meter bar stays where it fits @" + juce::String (width));
            }
        }

        beginTest ("at every window width the switches are there, fit their text, overlap nothing and never move the bar");
        {
            Rig rig (true);
            auto* top = rig.topBar();
            expect (top != nullptr);
            if (top == nullptr) return;

            const auto layout = [top]
            {
                std::vector<juce::Rectangle<int>> r;
                for (auto* c : top->getChildren())
                    r.push_back (c->isVisible() ? c->getBounds() : juce::Rectangle<int>());
                return r;
            };

            for (int width : { 1920, 1440, 1300, 1220, 1219, 1000, 760, 700, 699, 560, 460, 420 })
            {
                rig.main->setSize (width, 900);
                const auto before = layout();

                for (auto* id : { "mic-mute-group", "fx-mute-group" })
                {
                    auto* b = rig.button (id);
                    expect (b != nullptr && b->isVisible(), id);
                    if (b == nullptr) continue;
                    expect (top->getLocalBounds().contains (b->getBounds()), juce::String (id) + " @" + juce::String (width));
                    const auto font = b->getLookAndFeel().getTextButtonFont (*b, b->getHeight());
                    expectGreaterOrEqual (b->getWidth(), juce::GlyphArrangement::getStringWidthInt (font, b->getButtonText()) + 8,
                                          juce::String (id) + " @" + juce::String (width));
                }

                std::vector<juce::Component*> shown;
                for (auto* c : top->getChildren())
                    if (c->isVisible() && ! c->getBounds().isEmpty())
                        shown.push_back (c);
                for (size_t i = 0; i < shown.size(); ++i)
                    for (size_t j = i + 1; j < shown.size(); ++j)
                        expect (! shown[i]->getBounds().intersects (shown[j]->getBounds()),
                                "@" + juce::String (width) + ": " + shown[i]->getBounds().toString() + " / " + shown[j]->getBounds().toString());

                rig.groups().setAll (Group::fx, true);
                rig.groups().set (Group::mic, true);
                expect (layout() == before, "muting moved the bar @" + juce::String (width));
                rig.groups().release (Group::fx);
                rig.groups().release (Group::mic);
                expect (layout() == before, "releasing moved the bar @" + juce::String (width));
            }
        }
    }
};

static LiveMixMuteButtonTests liveMixMuteButtonTests;
}
