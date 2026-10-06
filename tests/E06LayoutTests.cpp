#include "VolumeCueUiTestSupport.h"
#include "ui/CueMidiPanel.h"
#include "ui/InspectorLayout.h"
#include "ui/SplitLayout.h"

namespace gocue::tests
{
namespace
{
using namespace volume_ui;

class E06LayoutTests : public juce::UnitTest
{
public:
    E06LayoutTests() : UnitTest ("E06 main window layout", "Enqueue") {}

    void runTest() override
    {
        Fixture f; // HiddenPeer only: no application process, native window or audio device is opened.
        auto& main = *f.main;
        auto& document = f.document();
        document.renameContainer (0, ko ("메인 큐 리스트"));
        document.addContainer (ko ("효과음 카트"), true);
        document.setActiveContainer (0);
        Cue cue;
        cue.number = "2";
        cue.name = ko ("타이틀 시그널");
        cue.file = f.scratch.folder.getChildFile ("title_signal.wav");
        cue.durationSeconds = 12;
        document.cues.add (cue);
        document.cues.setSelectedIndex (0);
        dispatch();
        main.setSize (1440, 900);

        auto* transport = child<TransportBar> (main);
        auto* status = child<FooterBar> (main);
        auto* containers = child<ContainerTabs> (main);
        auto* tabs = child<juce::TabbedComponent> (f.inspector());
        beginTest ("1440x900 default split frees twelve full cue rows and removes the footer strip");
        expect (transport != nullptr && status != nullptr && containers != nullptr && tabs != nullptr);
        if (transport == nullptr || status == nullptr || containers == nullptr || tabs == nullptr) return;
        expectWithinAbsoluteError (f.settings.getInspectorFraction(), 0.326, 1.0e-9);
        expectWithinAbsoluteError (SplitLayout::defaultInspectorFraction, 0.326, 1.0e-9);
        rect (*transport, { 12, 44, 1416, 112 });
        rect (f.inspector(), { 12, 653, 1416, 235 });
        rect (f.active(), { 1046, 168, 382, 473 });
        rect (*containers, { 13, 169, 1020, 41 });
        rect (f.table(), { 13, 210, 1020, 430 });
        expect (status->getParentComponent() == containers);
        expectEquals ((f.table().getHeight() - Palette::tableHeaderHeight) / 32, 12);

        beginTest ("transport columns and three cue lines match e06");
        transport->setStandbyCue (0, &cue);
        checkButton (*transport, "GO", { 0, 0, 112, 112 });   // square
        checkButton (*transport, ko ("일시정지"), { 1084, 0, 146, 52 });
        checkButton (*transport, ko ("페이드아웃"), { 1084, 60, 146, 52 });
        checkButton (*transport, ko ("전체 페이드 정지"), { 1238, 0, 178, 78 });
        auto* gear = child<juce::Button> (*transport, [] (const auto& b) { return b.getName() == "panicSettings"; });
        expect (gear != nullptr);
        if (gear != nullptr) rect (*gear, { 1238, 86, 178, 26 });
        checkLabel (*transport, ko ("다음 큐"), { 140, 17, 40, 18 });
        checkLabel (*transport, "2", { 140, 39, 15, 33 });
        checkLabel (*transport, cue.name, { 165, 39, 646, 33 });
        checkLabel (*transport, "title_signal.wav", { 140, 76, 97, 18 });
        checkLabel (*transport, ko ("실시간 · LUFS"), { 842, 17, 92, 18 });
        checkLabel (*transport, ko ("평균 · LUFS"), { 950, 17, 50, 18 });
        checkButton (*transport, ko ("20초 ▾"), { 1008, 17, 48, 18 });
        cue.file = f.scratch.folder.getChildFile ("different_long_filename.wav");
        transport->setStandbyCue (0, &cue); // same cue number: file width must still update
        auto* file = label (*transport, cue.file.getFileName());
        expect (file != nullptr && file->getWidth() > 97);

        beginTest ("migrated status labels retain text, colour, tooltips and warning actions");
        status->setCueCount (10);
        status->setWarningCount (0);
        status->setMidiStatus ("MIDI 0", "MIDI detail", false);
        status->setShowMode (false, &main.getShortcutService());
        containers->setInfoText (ko ("선택 2 · 다음 2 · Space = GO"));
        const auto audioText = ko ("Windows Audio · AVT GC311G2(2- NVIDIA High Definition Audio) · 48 kHz · 480 samples · 출력 지연 10.0 ms · CPU 0.5% · 피크 -- · xrun 0");
        status->setAudioStatus (audioText, true, "audio detail");
        auto* audio = label (main, audioText);
        expect (audio != nullptr);
        if (audio != nullptr)
        {
            expect (dynamic_cast<juce::MenuBarComponent*> (audio->getParentComponent()) != nullptr);
            rect (*audio, { 409, 3, 770, 26 });
            expect (audio->getWidth() >= juce::GlyphArrangement::getStringWidthInt (audio->getFont(), audioText));
            expectEquals (audio->getTooltip(), juce::String ("audio detail"));
            expect (audio->findColour (juce::Label::textColourId) == Palette::stopButton);
            status->setAudioStatus (audioText);
            expectEquals (audio->getTooltip(), audioText);
            expect (audio->findColour (juce::Label::textColourId) == Palette::muted);
        }
        auto* count = label (*status, ko ("큐 10개"));
        auto* midi = label (*status, "MIDI 0");
        auto* mode = child<juce::Label> (*status, [] (const auto& l) { return l.getText().startsWith (ko ("편집 모드")); });
        expect (count != nullptr && midi != nullptr && mode != nullptr);
        if (count == nullptr || midi == nullptr || mode == nullptr) return;
        rectIn (main, *count, { 635, 181, 49, 24 });
        // Native glyph metrics: mode hint is 145px and selection info 114px (mock: 144/115).
        rectIn (main, *mode, { 700, 175, 145, 35 });
        rectIn (main, *midi, { 861, 175, 28, 35 });
        expect (count->isVisible() && mode->isVisible() && midi->isVisible());
        status->setMidiStatus ("MIDI 0", "MIDI detail", true);
        expect (midi->findColour (juce::Label::textColourId) == Palette::warn);
        expectEquals (midi->getTooltip(), juce::String ("MIDI detail"));
        int warned = 0;
        status->onWarningsClicked = [&] { ++warned; };
        status->setWarningCount (3);
        auto* warnings = child<juce::TextButton> (*status);
        expect (warnings != nullptr && warnings->isVisible());
        if (warnings != nullptr)
        {
            expectEquals (warnings->getButtonText(), ko ("경고 3"));
            expectEquals (warnings->getX() - count->getRight(), 16);
            warnings->onClick();
            expectEquals (warned, 1);
            status->setWarningCount (0);
            expect (! warnings->isVisible());
        }
        status->onWarningsClicked = {};
        f.command (CommandIDs::toggleShowMode);
        expect (mode->getText().startsWith (ko ("쇼 모드: 편집 잠김")));
        expectEquals (mode->getTooltip(), mode->getText());
        f.command (CommandIDs::toggleShowMode);

        beginTest ("single inspector header and basic clusters fit the 192px page without scrolling");
        tabs->setCurrentTabIndex (0);
        auto* page = dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent());
        expect (page != nullptr);
        if (page == nullptr) return;
        auto* basic = page->getViewedComponent();
        checkLabel (f.inspector(), ko ("큐 인스펙터"), { 18, 7, 73, 35 });
        checkLabel (f.inspector(), ko ("2 · 타이틀 시그널 · title_signal.wav"), { 430, 7, 967, 35 });
        rectIn (main, *tabs->getTabbedButtonBar().getTabButton (0), { 107, 654, 47, 41 });
        rectIn (main, *page, { 13, 695, 1414, 192 });
        expect (! page->getVerticalScrollBar().isVisible() && ! page->getHorizontalScrollBar().isVisible());
        checkLabel (*basic, ko ("번호"), { 12, 8, 36, 30 });
        checkLabel (*basic, ko ("이름"), { 12, 46, 36, 30 });
        checkLabel (*basic, ko ("파일"), { 12, 84, 36, 30 });
        checkLabel (*basic, ko ("프리웨이트"), { 675, 8, 64, 30 });
        checkLabel (*basic, ko ("포스트웨이트"), { 881, 8, 104, 30 });
        checkLabel (*basic, ko ("메모"), { 12, 122, 36, 30 });
        auto* memo = child<juce::TextEditor> (*basic, [] (const auto& e) { return e.isMultiLine(); });
        expect (memo != nullptr);
        if (memo != nullptr) rect (*memo, { 48, 122, 1348, 62 });
        auto* cueMidi = child<CueMidiPanel> (*basic);
        expect (cueMidi != nullptr);
        if (cueMidi != nullptr) rect (*cueMidi, { 1108, 48, 288, 26 });
        noOverlap (*basic);

        beginTest ("play, trigger and plugin pages fit and use the new row alignments");
        tabs->setCurrentTabIndex (1);
        page = dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent());
        expect (page != nullptr && ! page->getVerticalScrollBar().isVisible());
        if (page != nullptr)
        {
            auto& play = *page->getViewedComponent();
            checkLabel (play, ko ("시작"), { 12, 8, 64, 30 });
            checkLabel (play, ko ("페이드 엔벨로프"), { 675, 8, 77, 30 });   // its three toggles follow on the same row
            checkLabel (play, ko ("게인 (dB)"), { 675, 46, 64, 30 });        // the 기본 tab's gain row, same column
            if (auto* gain = child<juce::Slider> (play, [] (const juce::Slider& s) { return s.getTextValueSuffix() == " dB"; }))
                rect (*gain, { 739, 46, 336, 30 });
            else
                expect (false, "missing the 재생 tab's gain slider");
            checkButton (play, ko ("리셋"), { 1108, 8, 60, 30 });
            auto* wave = child<WaveformView> (play);
            expect (wave != nullptr);
            if (wave != nullptr) rect (*wave, { 10, 84, 1388, 102 });
            noOverlap (play);
        }
        tabs->setCurrentTabIndex (4);
        page = dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent());
        expect (page != nullptr && ! page->getVerticalScrollBar().isVisible());
        if (page != nullptr)
        {
            auto& triggers = *page->getViewedComponent();
            checkLabel (triggers, ko ("재생 중에 다시 GO 하면"), { 12, 8, 184, 30 });
            checkLabel (triggers, ko ("레벨 (dB, 음수 = 덕)"), { 340, 122, 94, 30 });
            noOverlap (triggers);
        }
        tabs->setCurrentTabIndex (5);
        page = dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent());
        expect (page != nullptr && ! page->getVerticalScrollBar().isVisible());
        if (page != nullptr)
        {
            auto& effects = *page->getViewedComponent();
            checkButton (effects, ko ("+ 플러그인"), { 1188, 8, 100, 30 });
            checkButton (effects, ko ("관리..."), { 1296, 8, 100, 30 });
            noOverlap (effects);
        }

        beginTest ("level and trim keep their existing editors under the compact header");
        tabs->setCurrentTabIndex (2);
        page = dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent());
        if (page != nullptr)
        {
            auto& levels = *page->getViewedComponent();
            checkLabel (levels, ko ("패치"), { 10, 8, 39, 30 });
            checkButton (levels, ko ("기본 레벨로"), { 280, 8, 98, 30 });
            checkButton (levels, ko ("전부 무음"), { 386, 8, 90, 30 });
            noOverlap (levels);
        }
        tabs->setCurrentTabIndex (3);
        page = dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent());
        if (page != nullptr)
        {
            auto& trim = *page->getViewedComponent();
            checkLabel (trim, ko ("메인 트림 (dB)"), { 12, 8, 120, 30 });
            auto* output = label (trim, ko ("출력 1"));
            expect (output != nullptr);
            if (output != nullptr) rectIn (trim, *output, { 16, 44, 64, 22 });
            noOverlap (trim);
        }

        beginTest ("short and scaled logical windows retain every control and scroll wrapped groups");
        for (float scale : { 1.0f, 1.1f, 1.25f, 1.5f })
        {
            main.setTransform (juce::AffineTransform::scale (scale));
            main.setSize (juce::roundToInt (1440.0f / scale), juce::roundToInt (720.0f / scale));
            noOverlap (*transport);
            noOverlap (*status);
            expect (containers->getLocalBounds().contains (status->getBounds()));
            for (int index : { 0, 1, 2, 3, 4, 5 })
            {
                tabs->setCurrentTabIndex (index);
                page = dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent());
                expect (page != nullptr);
                if (page == nullptr) continue;
                noOverlap (*page->getViewedComponent());
                if (index == 0 && cueMidi != nullptr)
                {
                    page->setViewPosition (0, cueMidi->getY());
                    expect (page->getViewArea().contains (cueMidi->getBounds()));
                    page->setViewPosition (0, memo->getBottom());
                    expect (page->getViewArea().contains (memo->getBounds()));
                    page->setViewPosition (0, 0);
                }
            }
        }
        main.setTransform ({});
        main.setSize (1440, 900);

        beginTest ("assigned MIDI and a conflicting hotkey remain reachable at the minimum window width");
        tabs->setCurrentTabIndex (0);
        auto* hotkey = child<KeyCaptureButton> (*basic);
        expect (hotkey != nullptr && cueMidi != nullptr && memo != nullptr);
        if (hotkey != nullptr && cueMidi != nullptr && memo != nullptr)
        {
            MidiTrigger trigger;
            trigger.number = 60;
            trigger.channel = 1;
            expect (cueMidi->apply (-1, trigger).wasOk());
            expect (main.getShortcutService().setKeys ("transport.go", { juce::KeyPress ('A') }).wasOk());
            hotkey->onHotkeyChanged ("A");
            f.inspector().setEditable (true);
            main.setSize (860, 640);
            auto* notice = child<juce::Label> (*basic, [] (const auto& l) { return l.getText().contains (ko ("단축키와 충돌")); });
            expect (notice != nullptr && notice->getHeight() == 22);
            expectEquals (basic->getHeight(), 296);
            expect (memo->getHeight() >= 30);
            noOverlap (*basic);
            noOverlap (*cueMidi);
            noOverlap (*transport);
            noOverlap (*status);
            page = dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent());
            expect (page != nullptr);
            if (page != nullptr)
            {
                expect (page->getVerticalScrollBar().isVisible() && page->getHorizontalScrollBar().isVisible());
                page->setViewPosition (juce::jmax (0, memo->getRight() - page->getViewArea().getWidth()), memo->getBottom());
                expect (page->getViewArea().contains (memo->getBounds()));
                page->setViewPosition (0, cueMidi->getY());
                expect (page->getViewArea().contains (cueMidi->getBounds()));
                page->setViewPosition (0, 0);
            }
            expect (cueMidi->remove (0).wasOk());
            hotkey->onHotkeyChanged ({});
            expect (main.getShortcutService().restoreCommandDefaults ("transport.go").wasOk());
            main.setSize (1440, 900);
        }

        beginTest ("folds, cart, fullscreen callback and big-view scope retain the same hosts");
        f.command (CommandIDs::toggleInspector);
        expect (! f.inspector().isVisible());
        expect (f.table().getHeight() > 446);
        f.command (CommandIDs::toggleInspector);
        rect (f.inspector(), { 12, 653, 1416, 235 });
        f.command (CommandIDs::toggleActiveCues);
        expect (! f.active().isVisible());
        expectEquals (containers->getWidth(), 1402); // the existing fold divider keeps its 12px
        f.command (CommandIDs::toggleActiveCues);
        document.setActiveContainer (1);
        dispatch();
        auto* cart = child<CueCartView> (main);
        expect (cart != nullptr && cart->isVisible());
        expect (status->isVisible() && status->getParentComponent() == containers);
        bool full = false;
        main.onToggleFullScreen = [&] { full = ! full; };
        main.isFullScreenActive = [&] { return full; };
        auto& big = f.makeWindow();
        expect (f.key (juce::KeyPress (juce::KeyPress::F11Key), big));
        f.release (juce::KeyPress (juce::KeyPress::F11Key));
        expect (full);
        main.setSize (1920, 1080);
        expect (status->getParentComponent() == containers);
        noOverlap (*transport);
        noOverlap (*status);
        main.onToggleFullScreen = {};
        main.isFullScreenActive = {};

        beginTest ("many lists at the minimum window keep the cue count and the warnings button whole and clickable");
        {
            for (const char* name : { "A", "B", "C", "D", "E", "F" })
                document.addContainer (name, false);
            document.setActiveContainer (0);
            main.setSize (860, 640);
            dispatch();
            status->setWarningCount (3);   // after the refresh (it recounts the project's broken cues); relays the host out
            auto* warn = child<juce::TextButton> (*status, [] (const auto& b) { return b.isVisible(); });
            auto* countLabel = child<juce::Label> (*status, [] (const auto& l) { return l.getText().startsWith (ko ("큐 ")); });
            expect (warn != nullptr && countLabel != nullptr);
            if (warn != nullptr && countLabel != nullptr)
            {
                const auto warnArea = containers->getLocalArea (warn, warn->getLocalBounds());
                const auto countArea = containers->getLocalArea (countLabel, countLabel->getLocalBounds());
                expect (warnArea.getWidth() >= juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::headerSize, true), warn->getButtonText()) + 18,
                        "warnings button whole: " + warnArea.toString());
                expect (countArea.getWidth() >= juce::GlyphArrangement::getStringWidthInt (countLabel->getFont(), countLabel->getText()) + 18,
                        "count whole: " + countArea.toString());
                expect (containers->getLocalBounds().contains (warnArea) && containers->getLocalBounds().contains (countArea));
                auto* hit = containers->getComponentAt (warnArea.getCentre());
                expect (hit == warn || (hit != nullptr && warn->isParentOf (hit)), "the warnings button takes the click");
            }
            // the last list made active (as Ctrl+PageDown would) is scrolled into view, clear of the status strip
            document.addContainer ("G", false);
            document.addContainer ("H", false);
            document.setActiveContainer (document.getNumContainers() - 1);
            dispatch();
            status->setWarningCount (3);
            if (warn != nullptr)
            {
                const auto last = containers->getTabBounds (document.getNumContainers() - 1);
                const auto warnArea = containers->getLocalArea (warn, warn->getLocalBounds());
                expect (! last.isEmpty() && last.getX() >= 0 && last.getRight() <= warnArea.getX(),
                        "active last tab in view: " + last.toString() + " / warnings at " + warnArea.toString());
            }
            // scrolled to the end: a click left of x = 8 hits nothing, and a double-click on a partly hidden tab
            // renames that tab even though the first click scrolls it into view
            {
                auto mouse = [&] (juce::Point<float> at, int clicks)
                {
                    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, containers, containers, juce::Time::getCurrentTime(), at,
                                             juce::Time::getCurrentTime(), clicks, false);
                };
                juce::MouseWheelDetails wheel {};
                wheel.deltaY = -50.0f;
                containers->mouseWheelMove (mouse ({ 60.0f, 20.0f }, 1), wheel);   // to the far end
                const int before = document.getActiveContainer();
                containers->mouseDown (mouse ({ 2.0f, 20.0f }, 1));
                dispatch();
                expectEquals (document.getActiveContainer(), before, "x < 8 is not a tab");
                int partial = -1;
                for (int i = 0; i < document.getNumContainers(); ++i)
                    if (const auto r = containers->getTabBounds (i); r.getX() < 8 && r.getRight() > 14) { partial = i; break; }
                if (partial >= 0)
                {
                    int renamed = -1;
                    auto keep = containers->onRename;
                    containers->onRename = [&] (int i) { renamed = i; };
                    containers->mouseDown (mouse ({ 10.0f, 20.0f }, 1));
                    dispatch();
                    containers->mouseDoubleClick (mouse ({ 10.0f, 20.0f }, 2));
                    expectEquals (document.getActiveContainer(), partial, "the first click selects the partly hidden tab");
                    expectEquals (renamed, partial, "the double-click renames the same tab after it scrolled into view");
                    containers->onRename = keep;
                }
            }
            // squeezed long tabs at 1440: the first click selects a list whose cue count widens the status, the tabs
            // shrink a little before the second click - the double-click still renames the first click's list
            {
                main.setSize (1440, 900);
                for (int i = 0; i < document.getNumContainers(); ++i)
                    document.renameContainer (i, "Long cue list " + juce::String (i + 1));
                document.setActiveContainer (0);
                dispatch();
                status->setWarningCount (3);
                status->setCueCount (10);
                auto mouse = [&] (juce::Point<float> at, int clicks)
                {
                    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, containers, containers, juce::Time::getCurrentTime(), at,
                                             juce::Time::getCurrentTime(), clicks, false);
                };
                const int k = 4;
                const auto first = containers->getTabBounds (k);
                const juce::Point<float> at ((float) first.getRight() - 2.0f, 20.0f);
                int renamed = -1;
                auto keep = containers->onRename;
                containers->onRename = [&] (int i) { renamed = i; };
                containers->mouseDown (mouse (at, 1));
                dispatch();
                status->setCueCount (100000);   // a much wider count between the clicks
                if (containers->getTabBounds (k).getRight() <= (int) at.x)   // the edge did move past the pointer
                {
                    containers->mouseDown (mouse (at, 2));
                    dispatch();
                    containers->mouseDoubleClick (mouse (at, 2));
                    expectEquals (document.getActiveContainer(), k, "the second click keeps the first click's list");
                    expectEquals (renamed, k, "the double-click renames the first click's list");
                }
                else
                    expect (false, "the count change should move the tab edge: " + containers->getTabBounds (k).toString());

                // a list before the pressed one goes away between the clicks: the gesture follows the pressed list
                document.setActiveContainer (0);
                dispatch();
                renamed = -1;
                const auto pressedName = document.getContainerInfo (k).name;
                const auto pressedAt = containers->getTabBounds (k).toFloat().getCentre();
                containers->mouseDown (mouse (pressedAt, 1));
                dispatch();
                expect (document.removeContainer (2), "remove a list before the pressed one");
                dispatch();
                containers->mouseDown (mouse (pressedAt, 2));
                dispatch();
                containers->mouseDoubleClick (mouse (pressedAt, 2));
                expect (juce::isPositiveAndBelow (renamed, document.getNumContainers())
                            && document.getContainerInfo (renamed).name == pressedName, "the double-click renames the pressed list");
                expectEquals (document.getContainerInfo (document.getActiveContainer()).name, pressedName);

                // the pressed list itself goes away between the clicks: the second click and the double-click do nothing
                renamed = -1;
                const int victim = document.getActiveContainer();
                const auto victimAt = containers->getTabBounds (victim).toFloat().getCentre();
                containers->mouseDown (mouse (victimAt, 1));
                dispatch();
                expect (document.removeContainer (victim), "remove the pressed list");
                dispatch();
                const int activeAfterRemove = document.getActiveContainer();
                containers->mouseDown (mouse (victimAt, 2));
                dispatch();
                containers->mouseDoubleClick (mouse (victimAt, 2));
                expectEquals (renamed, -1, "no rename after the pressed list went away");
                expectEquals (document.getActiveContainer(), activeAfterRemove, "no selection change after the pressed list went away");
                containers->onRename = keep;
            }
            // a narrower window keeps the active list in view (it was in view before); a strip the wheel moved away
            // from the active list stays where the wheel left it, through a status relayout, a resize and the removal
            // of another list; a list that takes over the selection is brought into view
            {
                const int listsBefore = document.getNumContainers();
                for (int i = 0; i < 8; ++i)
                    document.addContainer ("More " + juce::String (i + 1), false);
                main.setSize (1440, 900);
                document.setActiveContainer (document.getNumContainers() - 1);
                dispatch();
                status->setWarningCount (3);
                auto activeBounds = [&] { return containers->getTabBounds (document.getActiveContainer()); };
                auto inView = [&]
                {
                    // a scrolling strip clips the tabs 16 px before the status strip
                    const auto r = activeBounds();
                    const auto statusLeft = containers->getLocalArea (status, status->getLocalBounds()).getX();
                    return ! r.isEmpty() && r.getX() >= 8 && r.getRight() <= statusLeft - 16;
                };
                auto mouse = [&] (juce::Point<float> at, int clicks)
                {
                    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, containers, containers, juce::Time::getCurrentTime(), at,
                                             juce::Time::getCurrentTime(), clicks, false);
                };
                expect (inView(), "active tab in view at 1440: " + activeBounds().toString());
                main.setSize (860, 640);
                dispatch();
                expect (inView(), "the narrower window keeps the active tab in view: " + activeBounds().toString());
                juce::MouseWheelDetails wheel {};
                wheel.deltaY = 50.0f;
                containers->mouseWheelMove (mouse ({ 60.0f, 20.0f }, 1), wheel);   // back to the first lists
                expect (! inView(), "the wheel moved the strip away from the active list");
                const int browsedX = containers->getTabBounds (0).getX();
                status->setCueCount (12345);   // a status relayout while browsing
                main.setSize (900, 640);
                dispatch();
                expectEquals (containers->getTabBounds (0).getX(), browsedX, "no jump back to the active list");
                expect (! inView(), "the strip stays where the wheel left it");
                const auto activeName = document.getContainerInfo (document.getActiveContainer()).name;
                expect (document.removeContainer (listsBefore), "remove a list before the active one");
                dispatch();
                expectEquals (document.getContainerInfo (document.getActiveContainer()).name, activeName);
                expect (! inView(), "removing a list before the active one does not pull the strip back");
                expect (document.removeContainer (document.getActiveContainer()), "remove the active list");
                dispatch();
                expect (inView(), "the list that takes over is brought into view: " + activeBounds().toString());
                while (document.getNumContainers() > listsBefore)
                    document.removeContainer (document.getNumContainers() - 1);
                main.setSize (1440, 900);
                dispatch();
            }
            document.setActiveContainer (0);
            status->setWarningCount (0);
            dispatch();
        }

        beginTest ("switching cue types keeps every inspector tab clickable (the header text never covers a tab)");
        document.setActiveContainer (0);
        main.setSize (1440, 900);
        dispatch();
        Cue groupCue;
        groupCue.number = "3";
        groupCue.name = ko ("그룹");
        groupCue.type = CueType::group;
        document.cues.add (groupCue);
        dispatch();
        for (int pick : { 1, 0, 1, 0 })   // the group cue (3 tabs), then the audio cue (6 tabs), with no resize in between
        {
            document.cues.setSelectedIndex (pick);
            dispatch();
            auto& bar = tabs->getTabbedButtonBar();
            expect (bar.getNumTabs() == (pick == 0 ? 6 : 3));
            for (int i = 0; i < bar.getNumTabs(); ++i)
                if (auto* button = bar.getTabButton (i))
                {
                    const auto centre = f.inspector().getLocalArea (button, button->getLocalBounds()).getCentre();
                    auto* hit = f.inspector().getComponentAt (centre);
                    expect (hit == button || (hit != nullptr && button->isParentOf (hit)),
                            "tab " + juce::String (i) + " takes the click after selecting cue " + juce::String (pick));
                }
        }

        beginTest ("saved split fractions still override the new default");
        f.settings.setInspectorFraction (0.45);
        expectWithinAbsoluteError (f.settings.getInspectorFraction(), 0.45, 1.0e-9);
        expectEquals (SplitLayout::inspectorHeight (736, f.settings.getInspectorFraction(), false, 12), 331);
        f.main.reset();
        f.main = std::make_unique<HiddenMain> (f.engine, f.settings, f.commands);
        f.main->setLookAndFeel (&f.theme);
        f.main->setSize (1440, 900);
        expectEquals (f.inspector().getHeight(), 324);   // round (720 * 0.45): the split area is 720 px under a 112 px transport

        // last in this run: it removes list 0, whose cues the earlier steps use
        beginTest ("a different list taking over the active tab's index is brought into view");
        {
            auto& doc = f.document();   // the main (and its document) was recreated above
            auto* bar = child<ContainerTabs> (*f.main);
            auto* footer = child<FooterBar> (*f.main);
            expect (bar != nullptr && footer != nullptr);
            if (bar != nullptr && footer != nullptr)
            {
                for (int i = 0; i < 12; ++i)
                    doc.addContainer ("Take " + juce::String (i + 1), false);
                f.main->setSize (860, 640);
                doc.setActiveContainer (0);
                dispatch();
                auto inView = [&]
                {
                    // a scrolling strip clips the tabs 16 px before the status strip
                    const auto r = bar->getTabBounds (doc.getActiveContainer());
                    const auto statusLeft = bar->getLocalArea (footer, footer->getLocalBounds()).getX();
                    return ! r.isEmpty() && r.getX() >= 8 && r.getRight() <= statusLeft - 16;
                };
                auto mouse = [&] (juce::Point<float> at, int clicks)
                {
                    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, bar, bar, juce::Time::getCurrentTime(), at,
                                             juce::Time::getCurrentTime(), clicks, false);
                };
                expect (inView(), "the first list is active and in view: " + bar->getTabBounds (0).toString());
                juce::MouseWheelDetails wheel {};
                wheel.deltaY = -50.0f;
                bar->mouseWheelMove (mouse ({ 60.0f, 20.0f }, 1), wheel);   // to the far end: the first list scrolls out
                expect (! inView(), "the wheel moved the first list out of view");
                const auto removedId = doc.getContainerInfo (0).id;
                expect (doc.removeContainer (0), "remove the active first list");
                dispatch();
                expectEquals (doc.getActiveContainer(), 0, "its neighbour takes over the same index");
                expect (doc.getContainerInfo (0).id != removedId, "a different list");
                expect (inView(), "the list that takes over index 0 is brought into view: " + bar->getTabBounds (0).toString());
            }
        }
    }

private:
    void rect (juce::Component& component, juce::Rectangle<int> wanted)
    {
        expect (component.getBounds() == wanted, "expected " + wanted.toString() + "; got " + component.getBounds().toString());
    }
    void rectIn (juce::Component& parent, juce::Component& component, juce::Rectangle<int> wanted)
    {
        const auto actual = parent.getLocalArea (&component, component.getLocalBounds());
        expect (actual == wanted, "expected " + wanted.toString() + "; got " + actual.toString());
    }
    juce::Label* label (juce::Component& root, const juce::String& text)
    {
        return child<juce::Label> (root, [&] (const auto& l) { return l.getText() == text; });
    }
    void checkLabel (juce::Component& root, const juce::String& text, juce::Rectangle<int> wanted)
    {
        auto* found = label (root, text);
        expect (found != nullptr, "missing label: " + text);
        if (found != nullptr) rect (*found, wanted);
    }
    void checkButton (juce::Component& root, const juce::String& text, juce::Rectangle<int> wanted)
    {
        auto* found = child<juce::Button> (root, [&] (const auto& b) { return b.getButtonText() == text; });
        expect (found != nullptr, "missing button: " + text);
        if (found != nullptr) rect (*found, wanted);
    }
    void noOverlap (juce::Component& parent)
    {
        for (auto* c : parent.getChildren())
        {
            if (! c->isVisible() || c->getBounds().isEmpty()) continue;
            expect (parent.getLocalBounds().contains (c->getBounds()), "outside parent: " + c->getBounds().toString());
            for (auto* other : parent.getChildren())
            {
                if (other == c) break;
                if (other->isVisible() && ! other->getBounds().isEmpty())
                    expect (! c->getBounds().intersects (other->getBounds()), "overlap: " + c->getBounds().toString() + " / " + other->getBounds().toString());
            }
        }
    }
};
static E06LayoutTests e06LayoutTests;
}
}
