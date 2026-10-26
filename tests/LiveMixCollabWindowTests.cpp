#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include "ui/CollabWindow.h"
#include "ui/LiveMixLookAndFeel.h"
#include "../livemix/src/ui/MainComponent.h"

namespace gocue::livemix
{
struct CollabPanelTestAccess
{
    static const auto& statistic (const CollabPanel& panel, size_t index) { return panel.stats[index]; }
    static const auto& reason (const CollabPanel& panel) { return panel.reason; }
};
}

namespace gocue::tests
{
using namespace livemix;
using State = collab::CollabSession::State;
namespace
{
struct Folder
{
    Folder() { file.createDirectory(); }
    ~Folder() { file.deleteRecursively(); }
    juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveMixCollabUi-" + juce::Uuid().toString());
};
struct Theme
{
    Theme() { juce::LookAndFeel::setDefaultLookAndFeel (&look); }
    ~Theme() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
    juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
    LiveMixLookAndFeel look;
};
void dispatch()
{
    MSG message {};
    for (int i = 0; i < 100 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++i)
    {
        TranslateMessage (&message); DispatchMessageW (&message);
    }
}
struct InputWindow : juce::DocumentWindow
{
    explicit InputWindow (juce::Component& panel) : DocumentWindow ("test", livemix::Palette::background, 0)
    {
        setUsingNativeTitleBar (true); setContentNonOwned (&panel, true); setVisible (true); toFront (true);
    }
    ~InputWindow() override { clearContentComponent(); }
    void closeButtonPressed() override { setVisible (false); }
};
template<class T> T& child (juce::Component& c, const char* id) { return *dynamic_cast<T*> (c.findChildWithID (id)); }
}

class LiveMixCollabWindowTests : public juce::UnitTest
{
public:
    LiveMixCollabWindowTests() : UnitTest ("LiveMix Collab window layout and actions", "LiveMix") {}
    void runTest() override
    {
        Theme theme;
        Folder folder;
        LiveMixSettings settings (folder.file);
        settings.setCollabNick (ko ("하람"));
        CollabPanel::Actions renderActions;
        renderActions.outputs = []
        {
            CollabPanel::OutputPairs pairs;
            pairs.names = { "Out 1", "Out 2", "Out 3", "Out 4" }; pairs.asio = pairs.hasOutput = pairs.running = true; pairs.numOutputs = 4;
            return pairs;
        };
        CollabPanel panel (settings, renderActions);

        beginTest ("all states render at 440x430, 400x400 and tall sizes, at 100/125/150 percent");
        std::vector<collab::CollabSession::Snapshot> snapshots;
        for (auto state : { State::off, State::checkingHost, State::joining, State::hostWaiting, State::connected, State::disconnected })
        {
            collab::CollabSession::Snapshot s;
            s.state = state; s.isHost = state != State::joining; s.code = "4827"; s.myNick = ko ("하람");
            s.peerNick = ko ("윤슬"); s.estimatedOneWayMs = 18.4; s.bufferMs = 5; s.lossPercent = 0.04;
            snapshots.push_back (s);
        }
        for (auto upnp : { collab::CollabUpnp::Result {}, collab::CollabUpnp::Result { true, true, "10.0.0.1", {} },
                           collab::CollabUpnp::Result { true, true, "203.0.113.8", {} } })
        {
            const auto check = collab::CollabHostCheck::evaluate (false, upnp, "203.0.113.8");
            collab::CollabSession::Snapshot s;
            s.state = State::hostUnavailable; s.isHost = true; s.message = check.message(); s.reason = check.reason;
            snapshots.push_back (s);
        }
        for (int error : { 0, 404, 410, 425, 409, 503 })
        {
            collab::CollabSession::Snapshot s; s.state = State::error; s.message = collab::CollabRoom::errorMessage (error);
            snapshots.push_back (s);
        }
        collab::CollabSession::Snapshot closed;
        closed.message = ko ("방장이 방을 닫았습니다"); snapshots.push_back (closed);
        for (int index : { 3, 4, 5 })
        {
            auto s = snapshots[(size_t) index]; s.wireRate = 44100;
            snapshots.push_back (s);
        }
        for (auto route : { collab::CollabAudio::Route::direct, collab::CollabAudio::Route::obs })
        {
            auto s = snapshots[4]; s.route = route; s.routeFirst = 2;
            snapshots.push_back (s);
        }
        auto longName = snapshots[4];
        longName.isHost = false; longName.peerNick = juce::String::repeatedString (ko ("긴닉네임"), 10);
        longName.bufferMs = 6.25; longName.lossPercent = 12.3; snapshots.push_back (longName);
        for (size_t n = 0; n < snapshots.size(); ++n)
            for (auto size : { juce::Point<int> (440, 430), juce::Point<int> (400, 400), juce::Point<int> (440, 800) })
            {
                panel.setSize (size.x, size.y); panel.setSnapshot (snapshots[n]);
                if (snapshots[n].state == State::connected) panel.pushMeter ({ 0.18f, 0.14f }, false);
                const auto tag = juce::String ((int) n) + "-" + juce::String (size.x) + "x" + juce::String (size.y);
                checkLayout (panel, tag);
                if (snapshots[n].state == State::hostWaiting)
                    expectEquals (child<juce::Label> (panel, "room-rate").getText(),
                                  juce::String (snapshots[n].wireRate == 44100 ? "44.1 kHz" : "48 kHz"));
                if (snapshots[n].state == State::connected || snapshots[n].state == State::disconnected)
                {
                    for (size_t i = 0; i < 4; ++i)
                    {
                        const auto& stat = CollabPanelTestAccess::statistic (panel, i);
                        expect (stat.isVisible());
                        expect (juce::GlyphArrangement::getStringWidthInt (captionFont(), stat.caption) <= stat.getWidth() - 16,
                                "stat caption fits " + tag + "/" + juce::String ((int) i));
                    }
                    // the partner's pair box reads a named ASIO pair up to '31-32 …', as a channel card's direct output
                    for (auto* c : child<juce::ComboBox> (panel, "route-pair").getChildren())
                        if (auto* text = dynamic_cast<juce::Label*> (c))
                            expect (juce::GlyphArrangement::getStringWidthInt (text->getFont(), "31-32 ...")
                                        <= text->getWidth() - text->getBorderSize().getLeftAndRight(), "the widest pair reads in full " + tag);
                    const auto& rate = CollabPanelTestAccess::statistic (panel, 3);
                    expectEquals (rate.caption, ko ("샘플레이트")); expectEquals (rate.unit, juce::String ("kHz"));
                    expectEquals (rate.value, juce::String (snapshots[n].wireRate == 44100 ? "44.1" : "48"));
                }
                if (snapshots[n].state == State::hostUnavailable)
                {
                    const auto& block = *panel.findChildWithID ("host-message");
                    juce::AttributedString text;
                    text.append (snapshots[n].message.upToFirstOccurrenceOf ("\n", false, false), bodyFont (15.0f), livemix::Palette::text);
                    juce::TextLayout layout; layout.createLayout (text, (float) block.getWidth());
                    expect (layout.getHeight() <= block.getHeight(), "host verdict fits " + tag);
                    // the cause wraps whole words (keep-all, as the verdict) into the height it measured, never …
                    const auto& cause = CollabPanelTestAccess::reason (panel);
                    expectEquals (cause.text, snapshots[n].reason);
                    expect (cause.getHeight() >= cause.heightFor (cause.getWidth()), "host cause fits " + tag);
                    if (snapshots[n].reason.contains (ko ("통신사")))   // the longest cause takes two lines in every size
                        expect (cause.heightFor (cause.getWidth()) > juce::roundToInt (bodyFont (14.0f).getHeight() * 1.5f), tag);
                }
                if (n == snapshots.size() - 1)
                {
                    auto& nick = child<juce::Label> (panel, "peer-nick");
                    expect (nick.getText().endsWith (ko ("…"))); expectEquals (nick.getTooltip(), longName.peerNick);
                    expectEquals (child<juce::TextButton> (panel, "leave").getButtonText(), ko ("나가기"));
                }
                for (float scale : { 1.0f, 1.25f, 1.5f }) snapshot (panel, "collab-" + tag, scale);
            }

        beginTest ("rate chips persist across reopening, use one selection, and disable while creating or joining");
        {
            Folder rateFolder;
            LiveMixSettings prefs (rateFolder.file);
            expectEquals (prefs.getCollabRate(), 48000);
            prefs.setCollabNick (ko ("하람"));
            int createdRate = 0;
            CollabPanel::Actions choose;
            choose.create = [&] (const auto&) { createdRate = prefs.getCollabRate(); };
            CollabPanel selection (prefs, choose);
            auto& rate48 = child<Chip> (selection, "rate-48000");
            auto& rate44 = child<Chip> (selection, "rate-44100");
            expect (rate48.getToggleState() && ! rate44.getToggleState());
            rate44.onClick();
            expect (! rate48.getToggleState() && rate44.getToggleState());
            rate44.onClick(); expect (rate44.getToggleState());
            expectEquals (prefs.getCollabRate(), 44100);
            child<juce::TextButton> (selection, "create").onClick(); expectEquals (createdRate, 44100);
            LiveMixSettings reloaded (rateFolder.file);
            CollabPanel reopened (reloaded, {});
            expectEquals (reloaded.getCollabRate(), 44100);
            expect (child<Chip> (reopened, "rate-44100").getToggleState());
            for (auto state : { State::checkingHost, State::joining })
            {
                collab::CollabSession::Snapshot busy; busy.state = state;
                selection.setSnapshot (busy);
                expect (! rate44.isEnabled() && ! rate48.isEnabled());
                expect (rate44.getAlpha() < 1.0f && rate48.getAlpha() < 1.0f);
            }
            selection.setSnapshot ({}); rate48.onClick();
            expect (rate48.getToggleState() && ! rate44.getToggleState());
            expectEquals (prefs.getCollabRate(), 48000);
            prefs.setCollabRate (96000); expectEquals (prefs.getCollabRate(), 48000);
            collab::CollabAudio audio;
            expect (! audio.setWireRate (96000)); expectEquals (audio.getWireRate(), 48000u);
        }

        beginTest ("digits, next focus, backspace, clipboard paste and Enter use the join action");
        int creates = 0, joins = 0, leaves = 0, rebuffers = 0, hides = 0;
        float monitorVolume = -1; bool monitorOn = false;
        juce::String joinedCode, joinedNick;
        CollabPanel::Actions actions;
        actions.create = [&] (const auto& nick) { ++creates; joinedNick = nick; };
        actions.join = [&] (const auto& code, const auto& nick) { ++joins; joinedCode = code; joinedNick = nick; };
        actions.leave = [&] { ++leaves; }; actions.rebuffer = [&] { ++rebuffers; }; actions.hide = [&] { ++hides; };
        actions.monitor = [&] (float value, bool on) { monitorVolume = value; monitorOn = on; };
        auto routed = collab::CollabAudio::Route::master; int routedFirst = -1;
        actions.route = [&] (collab::CollabAudio::Route route, int first) { routed = route; routedFirst = first; };
        actions.outputs = []
        {
            CollabPanel::OutputPairs pairs;
            pairs.names = { "Out 1", "Out 2", "Out 3", "Out 4" }; pairs.asio = pairs.hasOutput = pairs.running = true; pairs.numOutputs = 4;
            return pairs;
        };
        CollabPanel input (settings, actions);
        InputWindow window (input);
        auto& code = child<CollabCodeEntry> (input, "code");
        auto& first = child<juce::TextEditor> (code, "digit-0");
        auto& second = child<juce::TextEditor> (code, "digit-1");
        first.grabKeyboardFocus();
        first.keyPressed (juce::KeyPress ('a', {}, 'a')); expect (code.getCode().isEmpty());
        first.keyPressed (juce::KeyPress ('4', {}, '4'));
        expectEquals (code.getCode(), juce::String ("4")); expect (second.hasKeyboardFocus (false));
        input.setSnapshot ({}); expect (second.hasKeyboardFocus (false), "snapshot refresh keeps input focus");
        second.keyPressed (juce::KeyPress (juce::KeyPress::backspaceKey));
        expect (code.getCode().isEmpty()); expect (first.hasKeyboardFocus (false));
        const auto clipboard = juce::SystemClipboard::getTextFromClipboard();
        // RustDesk on this PC syncs the clipboard and holds it for a moment after each change: retry until the paste lands.
        for (int attempt = 0; attempt < 40 && code.getCode() != "4827"; ++attempt)
        {
            if (attempt > 0) juce::Thread::sleep (25);
            for (const char* id : { "digit-0", "digit-1", "digit-2", "digit-3" }) child<juce::TextEditor> (code, id).clear();
            first.grabKeyboardFocus();
            juce::SystemClipboard::copyTextToClipboard ("4827"); first.pasteFromClipboard();
        }
        juce::SystemClipboard::copyTextToClipboard (clipboard);
        expectEquals (code.getCode(), juce::String ("4827")); expect (code.complete());
        auto& last = child<juce::TextEditor> (code, "digit-3");
        expect (last.hasKeyboardFocus (false));
        last.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        expectEquals (joins, 1); expectEquals (joinedCode, juce::String ("4827")); expectEquals (joinedNick, ko ("하람"));
        first.insertTextAtCaret ("x1y2z3"); expectEquals (code.getCode(), juce::String ("1237"));
        last.keyPressed (juce::KeyPress (juce::KeyPress::backspaceKey)); last.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)); expectEquals (joins, 1);

        beginTest ("nickname persists trimmed, limits input to forty characters and gates both actions");
        auto& nick = child<juce::TextEditor> (input, "nickname");
        nick.setText ("   "); dispatch();
        auto& create = child<juce::TextButton> (input, "create");
        auto& join = child<juce::TextButton> (input, "join");
        expect (! create.isEnabled() && ! join.isEnabled());
        nick.setText (ko ("  하람  ")); dispatch();
        expectEquals (settings.getCollabNick(), ko ("하람")); expect (create.isEnabled());
        LiveMixSettings reloaded (folder.file); expectEquals (reloaded.getCollabNick(), ko ("하람"));
        nick.selectAll(); nick.insertTextAtCaret (juce::String::repeatedString (ko ("가"), 50)); dispatch();
        expectEquals (nick.getText().length(), 40); expectEquals (settings.getCollabNick().length(), 40);
        create.onClick(); expectEquals (creates, 1); expectEquals (joinedNick.length(), 40);
        // busy: the pressed button keeps full strength so 포트 확인 중 / 연결 중 reads, but it takes no click
        const auto takesClicks = [] (juce::Component& c) { bool self = false, children = false; c.getInterceptsMouseClicks (self, children); return self; };
        auto busy = snapshots[1]; input.setSnapshot (busy);
        expectEquals (create.getButtonText(), ko ("포트 확인 중")); expectEquals (create.getAlpha(), 1.0f);
        expect (! takesClicks (create) && ! join.isEnabled());
        create.onClick(); expectEquals (creates, 1);
        input.setSnapshot (snapshots[2]); expectEquals (join.getButtonText(), ko ("연결 중")); expectEquals (join.getAlpha(), 1.0f);
        expect (! takesClicks (join) && ! create.isEnabled());
        const int joinsBefore = joins; join.onClick(); expectEquals (joins, joinsBefore);

        beginTest ("a partner arriving while the nickname has focus leaves Enter nothing to press");
        input.setSnapshot ({}); expect (takesClicks (create) && takesClicks (join));
        nick.grabKeyboardFocus(); expect (nick.hasKeyboardFocus (false));
        input.setSnapshot (snapshots[3]); input.setSnapshot (snapshots[4]);
        for (auto* c : input.getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (c)) expect (! b->getWantsKeyboardFocus(), "focusable " + b->getComponentID());
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        expect (dynamic_cast<juce::Button*> (focused) == nullptr);
        const int leavesBefore = leaves;
        if (focused != nullptr) focused->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        expectEquals (leaves, leavesBefore);

        beginTest ("retry, dismiss, leave, rebuffer, headphones and wheel-disabled volume use their actions");
        input.setSnapshot (snapshots[6]);
        child<juce::TextButton> (input, "retry").onClick(); expectEquals (creates, 2);
        child<juce::TextButton> (input, "dismiss").onClick(); expectEquals (leaves, 1);
        input.setSnapshot (snapshots[4]);
        child<juce::TextButton> (input, "leave").onClick(); expectEquals (leaves, 2);
        child<juce::TextButton> (input, "rebuffer").onClick(); expectEquals (rebuffers, 1);
        child<LampButton> (input, "headphones").onClick(); expect (! monitorOn); expectEquals (monitorVolume, 1.0f);
        auto& slider = child<juce::Slider> (input, "volume");
        expect (! slider.isScrollWheelEnabled()); slider.setValue (37, juce::sendNotificationSync);
        expectWithinAbsoluteError (monitorVolume, 0.37f, 0.0001f); expect (monitorOn);
        expectEquals (child<juce::Label> (input, "volume-value").getText(), juce::String ("37%"));
        input.setSnapshot ({}); nick.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)); dispatch(); expectEquals (hides, 1);

        beginTest ("the partner's output chips and pair choose the route, and listening in OBS dims the headphone controls");
        {
            using Route = collab::CollabAudio::Route;
            auto call = snapshots[4];
            input.setSnapshot (call);
            auto& toMaster = child<Chip> (input, "route-master");
            auto& toDirect = child<Chip> (input, "route-direct");
            auto& inObs = child<Chip> (input, "route-obs");
            auto& pair = child<juce::ComboBox> (input, "route-pair");
            expect (toMaster.getToggleState() && ! toDirect.getToggleState() && ! inObs.getToggleState());
            expect (toMaster.isVisible() && pair.isVisible() && ! pair.isEnabled());
            toDirect.onClick(); expect (routed == Route::direct); expectEquals (routedFirst, 0);
            call.route = Route::direct; input.setSnapshot (call);
            expect (toDirect.getToggleState() && pair.isEnabled()); expectEquals (pair.getNumItems(), 2);
            pair.setSelectedId (3, juce::sendNotificationSync); expect (routed == Route::direct); expectEquals (routedFirst, 2);
            inObs.onClick(); expect (routed == Route::obs);
            call.route = Route::obs; call.routeFirst = 2; input.setSnapshot (call);
            expect (inObs.getToggleState() && ! pair.isEnabled());
            expect (! child<LampButton> (input, "headphones").isEnabled() && ! child<juce::Slider> (input, "volume").isEnabled());
            auto& volumeCaption = child<juce::Label> (input, "volume-caption");   // faded once, not also disabled (half again)
            expect (volumeCaption.isEnabled() && volumeCaption.getAlpha() < 1.0f);
            call.route = Route::master; input.setSnapshot (call);
            expect (child<LampButton> (input, "headphones").isEnabled() && child<juce::Slider> (input, "volume").isEnabled());
            settings.setCollabRoute (2); settings.setCollabRouteFirst (4); settings.saveIfNeeded();
            {
                LiveMixSettings reloaded (folder.file);
                expectEquals (reloaded.getCollabRoute(), 2); expectEquals (reloaded.getCollabRouteFirst(), 4);
            }
            settings.setCollabRoute (7); expectEquals (settings.getCollabRoute(), 0);
            settings.setCollabRouteFirst (0); settings.saveIfNeeded();
            input.setSnapshot ({});
        }

        beginTest ("the top bar has no 합방 button (합방 is in the menu only) and still fits in every mode");
        MixEngine engine ("Local\\LiveMix.CollabUi." + juce::Uuid().toString());
        MixDocument document (engine);
        TopBar bar (document);
        bar.setDevices ({ "HDMI(StreamLine Mini+ GC311G2)" }, "HDMI(StreamLine Mini+ GC311G2)", "Windows Audio");
        MixEngine::DeviceFormat format; format.inputBits = 24;
        bar.setStatus (48000, 256, 10.7, 0.2, true, format); bar.setFxCount (4);
        expect (bar.findChildWithID ("collab") == nullptr);
        for (auto* c : bar.getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (c)) expect (! b->getButtonText().contains (ko ("합방")), b->getButtonText());
        for (int width : { 1440, 1420, 1220, 1000, 900, 700, 460, 420 })
        {
            bar.setSize (width, bar.preferredHeight (width));
            bar.setMuteGroups (false, false);
            const auto sessionBounds = bar.findChildWithID ("session-name")->getBounds();
            const auto mode = bar.modeFor (width);
            for (bool muted : { false, true })
            {
                bar.setMuteGroups (muted, muted); checkLayout (bar, "topbar " + juce::String (width));
                expect (mode == bar.modeFor (width));
                expect (sessionBounds == bar.findChildWithID ("session-name")->getBounds());
            }
            snapshot (bar, "topbar-" + juce::String (width), 1.0f);
        }

        beginTest ("main opens one native collab window, brings it forward, and X or Escape only hide it");
        settings.setMicMuteHotkey ({}); settings.setFxMuteHotkey ({}); settings.setWindowHotkey ({});
        collab::CollabSession::Options options; options.loopbackOnly = true;
        collab::CollabSession session (engine.getCollab(), settings,
            std::make_unique<collab::CollabRoom> ([] (const auto&) { return collab::CollabRoom::Response { 404, "{}" }; }),
            collab::CollabUpnp::makeMiniUpnp(), options);
        ObsPluginActions obs;
        obs.roots = [path = folder.file] { return ObsPluginInstaller::Roots { path, path, path, [] { return false; }, {} }; };
        obs.elevate = [] (juce::String&) { return ObsPluginInstaller::Result::needsElevation; };
        livemix::MainComponent main (document, settings, obs); main.setCollabSession (&session);
        TopBar* top = nullptr;
        for (auto* c : main.getChildren()) if (auto* t = dynamic_cast<TopBar*> (c)) top = t;
        expect (top != nullptr);
        if (top != nullptr)
        {
            main.menuItemSelected (1, 3);   // 합방 > 합방 창 열기...
            CollabWindow* collabWindow = nullptr;
            for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
                if (auto* w = dynamic_cast<CollabWindow*> (juce::TopLevelWindow::getTopLevelWindow (i))) collabWindow = w;
            expect (collabWindow != nullptr);
            if (collabWindow != nullptr)
            {
                expect (collabWindow->isUsingNativeTitleBar()); expectEquals (collabWindow->getName(), ko ("합방"));
                auto* content = collabWindow->getContentComponent();
                expectEquals (content->getWidth(), 440); expectEquals (content->getHeight(), 430);
                main.menuItemSelected (1, 3); expect (collabWindow->getContentComponent() == content);
                collabWindow->closeButtonPressed(); expect (! collabWindow->isVisible());
                main.menuItemSelected (1, 3); expect (collabWindow->isVisible());
                auto* viewport = dynamic_cast<CollabViewport*> (content);
                expect (viewport != nullptr);
                if (viewport != nullptr)
                    child<juce::TextEditor> (*viewport->getViewedComponent(), "nickname").keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
                dispatch();
                expect (! collabWindow->isVisible()); expect (session.getSnapshot().state == State::off);
            }
        }

        beginTest ("the main, collab and loudness windows each own a tooltip window, and the main one shows its tips");
        {
            // LiveMix has no app-wide TooltipWindow; one per window shows only that window's tips (JUCE matches the peer)
            const auto tipsOf = [] (juce::Component& owner) -> juce::TooltipWindow*
            {
                for (auto* c : owner.getChildren()) if (auto* t = dynamic_cast<juce::TooltipWindow*> (c)) return t;
                return nullptr;
            };
            auto* mainTips = tipsOf (main);
            expect (mainTips != nullptr, "main window");
            for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
                if (auto* w = dynamic_cast<CollabWindow*> (juce::TopLevelWindow::getTopLevelWindow (i)))
                    expect (tipsOf (*w) != nullptr, "collab window");
            LoudnessWindow loudness (engine, settings);
            expect (tipsOf (loudness) != nullptr, "loudness window");
            if (mainTips != nullptr)
            {
                main.setSize (1100, 640);
                mainTips->displayTip (main.localPointToGlobal (juce::Point<int> (420, 150)), ko ("마스터를 거쳐 메인 출력으로"));
                expect (mainTips->isVisible() && main.getLocalBounds().contains (mainTips->getBounds()));
                for (float scale : { 1.0f, 1.5f }) snapshot (main, "tooltip-main", scale);
                mainTips->hideTip(); expect (! mainTips->isVisible());
            }
        }

        beginTest ("the menu bar has 합방 between 설정 and 도움말, and its item opens the collab window");
        {
            expect (main.getMenuBarNames() == juce::StringArray { ko ("세션"), ko ("온라인 백업"), ko ("설정"), ko ("합방"), ko ("도움말") });
            const auto itemsOf = [&main] (int index)
            {
                juce::StringArray texts;
                juce::Array<bool> enabled;
                const auto menu = main.getMenuForIndex (index, {});   // the iterator keeps a reference: no temporary
                for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                    if (! it.getItem().isSeparator) { texts.add (it.getItem().text); enabled.add (it.getItem().isEnabled); }
                return std::pair<juce::StringArray, juce::Array<bool>> { texts, enabled };
            };
            const auto collabMenu = itemsOf (3);
            expect (collabMenu.first == juce::StringArray { ko ("합방 창 열기...") });
            expect (collabMenu.second.size() == 1 && collabMenu.second[0]);
            const auto help = itemsOf (4).first;   // 도움말 moved one place, unchanged
            expect (help.size() == 3 && help[0] == ko ("커뮤니티 (카카오톡 오픈채팅)") && help[2] == ko ("LiveMix 정보"));
            const auto findCollab = []
            {
                for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
                    if (auto* w = dynamic_cast<CollabWindow*> (juce::TopLevelWindow::getTopLevelWindow (i))) return w;
                return static_cast<CollabWindow*> (nullptr);
            };
            if (auto* w = findCollab()) { w->closeButtonPressed(); expect (! w->isVisible()); }
            main.menuItemSelected (1, 3);
            auto* opened = findCollab();
            expect (opened != nullptr && opened->isVisible());
            if (opened != nullptr) opened->closeButtonPressed();
            main.setSize (1100, 640);
            for (float scale : { 1.0f, 1.5f }) snapshot (main, "menu-collab", scale);
        }
    }

private:
    void checkLayout (juce::Component& parent, const juce::String& context)
    {
        for (auto* c : parent.getChildren())
        {
            if (! c->isVisible()) continue;
            const auto id = c->getComponentID();
            expect (! c->getBounds().isEmpty(), context + " empty " + id);
            expect (parent.getLocalBounds().contains (c->getBounds()), context + " outside " + id + ": " + c->getBounds().toString());
            for (auto* other : parent.getChildren())
                if (other != c && other->isVisible())
                    expect (! c->getBounds().intersects (other->getBounds()), context + " overlap " + id + " / " + other->getComponentID());
            if (auto* label = dynamic_cast<juce::Label*> (c))
            {
                if (id != "session-name")
                    expect (juce::GlyphArrangement::getStringWidthInt (label->getFont(), label->getText()) <= label->getWidth(), context + " text " + id);
            }
            if (auto* button = dynamic_cast<juce::TextButton*> (c))
                expect (juce::GlyphArrangement::getStringWidthInt (button->getLookAndFeel().getTextButtonFont (*button, button->getHeight()),
                                                                  button->getButtonText()) <= button->getWidth() - 12, context + " button " + id);
            if (dynamic_cast<CollabCodeEntry*> (c)) checkLayout (*c, context + " digits");
        }
    }
    void snapshot (juce::Component& c, const juce::String& filename, float scale)
    {
        const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, scale);
        expect (image.isValid()); expectEquals (image.getWidth(), juce::roundToInt (c.getWidth() * scale));
        const auto path = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_UI_SCREENSHOT_DIR", {});
        if (path.isEmpty()) return;
        const auto directory = juce::File (path); expect (directory.createDirectory().wasOk());
        juce::FileOutputStream output (directory.getChildFile (filename + "-" + juce::String ((int) (scale * 100)) + ".png"));
        expect (output.openedOk());
        if (output.openedOk())
        {
            output.setPosition (0); output.truncate();
            expect (juce::PNGImageFormat().writeImageToStream (image, output));
        }
    }
};
static LiveMixCollabWindowTests liveMixCollabWindowTests;
}
