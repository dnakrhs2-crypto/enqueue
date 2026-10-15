#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include "ui/CollabWindow.h"
#include "ui/LiveMixLookAndFeel.h"
#include "../livemix/src/ui/MainComponent.h"

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
        CollabPanel panel (settings, {});

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
                if (snapshots[n].state == State::hostUnavailable)
                {
                    const auto& block = *panel.findChildWithID ("host-message");
                    juce::AttributedString text;
                    text.append (snapshots[n].message.upToFirstOccurrenceOf ("\n", false, false), bodyFont (15.0f), livemix::Palette::text);
                    juce::TextLayout layout; layout.createLayout (text, (float) block.getWidth());
                    expect (layout.getHeight() <= block.getHeight(), "host verdict fits " + tag);
                    expectEquals (child<juce::Label> (panel, "reason").getText(), snapshots[n].reason);
                }
                if (n == snapshots.size() - 1)
                {
                    auto& nick = child<juce::Label> (panel, "peer-nick");
                    expect (nick.getText().endsWith (ko ("…"))); expectEquals (nick.getTooltip(), longName.peerNick);
                    expectEquals (child<juce::TextButton> (panel, "leave").getButtonText(), ko ("나가기"));
                }
                for (float scale : { 1.0f, 1.25f, 1.5f }) snapshot (panel, "collab-" + tag, scale);
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
        juce::SystemClipboard::copyTextToClipboard ("4827"); first.pasteFromClipboard();
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
        auto busy = snapshots[1]; input.setSnapshot (busy);
        expectEquals (create.getButtonText(), ko ("포트 확인 중")); expect (! create.isEnabled() && ! join.isEnabled());
        input.setSnapshot (snapshots[2]); expectEquals (join.getButtonText(), ko ("연결 중"));
        expect (! create.isEnabled() && ! join.isEnabled());

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

        beginTest ("top bar controls and reserved mute badges fit in every mode, including long peer names");
        MixEngine engine ("Local\\LiveMix.CollabUi." + juce::Uuid().toString());
        MixDocument document (engine);
        TopBar bar (document);
        bar.setDevices ({ "HDMI(StreamLine Mini+ GC311G2)" }, "HDMI(StreamLine Mini+ GC311G2)", "Windows Audio");
        MixEngine::DeviceFormat format; format.inputBits = 24;
        bar.setStatus (48000, 256, 10.7, 0.2, true, format); bar.setFxCount (4);
        int opened = 0; bar.onCollab = [&] { ++opened; };
        child<juce::TextButton> (bar, "collab").onClick(); expectEquals (opened, 1);
        for (int width : { 1440, 1420, 1220, 1000, 900, 700, 460, 420 })
            for (auto state : { State::off, State::connected, State::disconnected })
            {
                bar.setSize (width, bar.preferredHeight (width)); bar.setCollabState (state, longName.peerNick);
                bar.setMuteGroups (false, false);
                const auto sessionBounds = bar.findChildWithID ("session-name")->getBounds();
                const auto mode = bar.modeFor (width);
                for (bool muted : { false, true })
                {
                    bar.setMuteGroups (muted, muted); checkLayout (bar, "topbar " + juce::String (width));
                    expect (mode == bar.modeFor (width));
                    expect (sessionBounds == bar.findChildWithID ("session-name")->getBounds());
                }
                const auto& button = child<juce::TextButton> (bar, "collab");
                if (state == State::connected) { expect (button.getToggleState()); expect (button.getButtonText().endsWith (ko ("…"))); }
                if (state == State::disconnected) expectEquals (button.getButtonText(), ko ("합방 끊김"));
                snapshot (bar, "collab-topbar-" + juce::String (width) + "-" + juce::String ((int) state), 1.0f);
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
            top->onCollab();
            CollabWindow* collabWindow = nullptr;
            for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
                if (auto* w = dynamic_cast<CollabWindow*> (juce::TopLevelWindow::getTopLevelWindow (i))) collabWindow = w;
            expect (collabWindow != nullptr);
            if (collabWindow != nullptr)
            {
                expect (collabWindow->isUsingNativeTitleBar()); expectEquals (collabWindow->getName(), ko ("합방"));
                auto* content = collabWindow->getContentComponent();
                expectEquals (content->getWidth(), 440); expectEquals (content->getHeight(), 430);
                top->onCollab(); expect (collabWindow->getContentComponent() == content);
                collabWindow->closeButtonPressed(); expect (! collabWindow->isVisible());
                top->onCollab(); expect (collabWindow->isVisible());
                child<juce::TextEditor> (*content, "nickname").keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)); dispatch();
                expect (! collabWindow->isVisible()); expect (session.getSnapshot().state == State::off);
            }
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
                if (id != "reason" && id != "session-name")
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
