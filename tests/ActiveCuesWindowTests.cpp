#include "VolumeCueUiTestSupport.h"
#include "app/UiScale.h"

namespace gocue::tests
{
namespace
{
using namespace volume_ui;

class ActiveCuesWindowPanelTests : public juce::UnitTest
{
public:
    ActiveCuesWindowPanelTests() : UnitTest ("ActiveCuesWindowPanel", "Enqueue") {}
    void runTest() override
    {
        Fixture f;
        const auto sound = f.addSound();
        auto& panel = f.active();
        auto* big = child<juce::TextButton> (panel, [] (const auto& b) { return b.getButtonText() == ko ("크게보기"); });
        beginTest ("big-view callback controls visibility and keeps Space focus available");
        expect (big != nullptr);
        if (big == nullptr) return;
        expect (! big->getWantsKeyboardFocus());
        panel.onBigViewRequested = {}; panel.resized();
        expect (! big->isVisible());
        int opened = 0;
        panel.onBigViewRequested = [&] { ++opened; }; panel.resized();
        expect (big->isVisible()); big->onClick(); expectEquals (opened, 1);

        beginTest ("view-only hides buttons and ignores stop/pause/scrub even when callbacks are invoked");
        expect (f.engine.play (sound));
        auto playing = f.engine.getPlayingCues();
        playing[0].progress = 0.25; // allow a scrub if the view-only check were absent
        panel.setPlayingCues (playing);
        auto* pause = child<juce::TextButton> (panel, [] (const auto& b) { return b.getButtonText() == ko ("일시정지"); });
        auto* stop = child<juce::TextButton> (panel, [] (const auto& b) { return b.getButtonText() == ko ("×"); });
        expect (pause != nullptr && stop != nullptr);
        if (pause == nullptr || stop == nullptr) return;
        expect (pause->isVisible() && stop->isVisible());
        panel.setViewOnly (true);
        expect (! pause->isVisible() && ! stop->isVisible() && ! big->isVisible() && ! panel.isScrubEnabled());
        pause->onClick(); stop->onClick(); big->onClick();
        expect (! f.engine.isPaused (sound.id) && ! f.engine.isStopping (sound.id));
        expectEquals (opened, 1);
        auto& row = *pause->getParentComponent();
        expectEquals (row.getHeight(), Palette::activeViewCardHeight, "a view-only card has no button row");
        const juce::Point<float> point ((float) row.getWidth() * 0.75f, 75.0f);
        const auto event = juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            &row, &row, juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(), 1, false);
        const auto before = f.engine.getPlayingCues()[0].filePositionSeconds;
        row.mouseDown (event); row.mouseDrag (event);
        expectWithinAbsoluteError (f.engine.getPlayingCues()[0].filePositionSeconds, before, 1.0e-9);
        panel.setScrubEnabled (true); expect (! panel.isScrubEnabled());
        panel.setPlayingCues (playing); expect (! pause->isVisible() && ! stop->isVisible());
        panel.setViewOnly (false); expect (pause->isVisible() && stop->isVisible() && big->isVisible());
        expectEquals (row.getHeight(), Palette::activeCardHeight);
        panel.setScrubEnabled (false); expect (! panel.isScrubEnabled());

        beginTest ("scale is limited by both axes and card count, clamped from one to four");
        const int needed = Palette::cardHeaderHeight + Palette::activeViewCardHeight + 3 * Palette::cardInset;
        expectEquals (ActiveCuesWindow::scaleFor (0, 0, 0), 1.0f);
        expectEquals (ActiveCuesWindow::scaleFor (420, needed, 0), 1.0f);
        expectEquals (ActiveCuesWindow::scaleFor (840, needed * 2, 1), 2.0f);
        expectEquals (ActiveCuesWindow::scaleFor (420, needed * 4, 1), 1.0f);
        expectEquals (ActiveCuesWindow::scaleFor (1680, needed, 1), 1.0f);
        expectEquals (ActiveCuesWindow::scaleFor (4200, needed * 10, 1), 4.0f);
        const int two = Palette::cardHeaderHeight + 2 * (Palette::activeViewCardHeight + Palette::cardInset) + 2 * Palette::cardInset;
        expectEquals (ActiveCuesWindow::scaleFor (840, two * 2, 2), 2.0f);
        expect (ActiveCuesWindow::scaleFor (840, needed * 2, 2) < 2.0f);
        expectEquals (ActiveCuesWindow::scaleFor (840, 600, 100000), 1.0f);
    }
};

class ActiveCuesWindowIntegrationTests : public juce::UnitTest
{
public:
    ActiveCuesWindowIntegrationTests() : UnitTest ("ActiveCuesWindowIntegration", "Enqueue") {}
    void runTest() override
    {
        Fixture f;
        const auto sound = f.addSound();
        f.document().cues.setSelectedIndex (0);
        auto& original = f.active();
        const auto bounds = original.getBounds();
        auto* parent = original.getParentComponent();
        const bool visible = original.isVisible();
        beginTest ("open, repeat-open and close preserve the original panel and table selection");
        f.hiddenMain();
        auto& window = f.makeWindow();
        expectEquals (window.getName(), ko ("활성 큐 크게보기 - Enqueue"));
        expect (window.isVisible() && window.getPanel().isViewOnly());
        expect (original.getParentComponent() == parent && original.getBounds() == bounds && original.isVisible() == visible);
        expectEquals (f.document().cues.getSelectedIndex(), 0);
        f.command (CommandIDs::showActiveCuesWindow);
        expect (ReopenLastProjectTestAccess::bigView (*f.main) == &window);
        window.closeButtonPressed();
        expect (! window.isVisible());
        expect (original.getParentComponent() == parent && original.getBounds() == bounds && original.isVisible() == visible);
        expect (f.settings.getActiveCuesWindowState().isNotEmpty());

        beginTest ("focus moving to the big view commits a main-table edit (never left pending) and keeps the selection");
        f.table().beginCellEdit (0, CueTable::colName);
        auto* editor = child<juce::TextEditor> (f.table());
        expect (editor != nullptr);
        if (editor != nullptr)
        {
            editor->setText ("Pending music name", false);
            editor->grabKeyboardFocus();
            window.open();
            editor->focusLost (juce::Component::focusChangedDirectly);
            dispatch();
            expectEquals (f.document().cues.get (0).name, juce::String ("Pending music name"));
            expectEquals (f.document().cues.getSelectedIndex(), 0);
            ReopenLastProjectTestAccess::refreshPlayback (*f.main);
            expect (window.isParentOf (juce::Component::getCurrentlyFocusedComponent()));
            f.table().finishEditing(); dispatch();
            expectEquals (f.document().cues.get (0).name, juce::String ("Pending music name"));
        }

        beginTest ("collapsed main panel, waits, show mode and list/cart changes continue updating the copy");
        f.command (CommandIDs::toggleActiveCues);
        expect (! original.isVisible());
        expect (f.engine.play (sound));
        f.engine.setLiveGainDb (sound.id, -6.0);
        Cue wait; wait.type = CueType::control; wait.control.kind = ControlKind::wait; wait.control.seconds = 8; wait.name = "Waiting";
        f.document().cues.add (wait);
        expect (ReopenLastProjectTestAccess::controller (*f.main).trigger (wait) == CueController::GoResult::started);
        ReopenLastProjectTestAccess::refreshPlayback (*f.main);
        expectEquals (window.getPanel().getCardCount(), 2);
        auto* count = child<juce::Label> (window.getPanel(), [] (const auto& label) { return label.getText().startsWith (ko ("재생 중 ")); });
        expect (count != nullptr && count->getText().contains (ko ("대기 1")));
        f.command (CommandIDs::toggleShowMode);
        ReopenLastProjectTestAccess::refreshPlayback (*f.main);
        expect (window.getPanel().isViewOnly() && ! window.getPanel().isScrubEnabled());
        expectEquals (window.getPanel().getCardCount(), 2);
        f.command (CommandIDs::toggleShowMode);
        const int cart = f.document().addContainer ("Cart", true);
        f.document().setActiveContainer (cart);
        ReopenLastProjectTestAccess::refreshPlayback (*f.main);
        expectEquals (window.getPanel().getCardCount(), 2);
        expect (child<juce::Label> (window.getPanel(), [&] (const auto& label) { return label.getText() == "Pending music name"; }) != nullptr);
        f.document().setActiveContainer (0);
        f.command (CommandIDs::toggleActiveCues);
        ReopenLastProjectTestAccess::refreshPlayback (*f.main);
        expectEquals (original.getCardCount(), window.getPanel().getCardCount());

        beginTest ("transform follows content size and count; position/size survive hide and new instance");
        window.setBounds (40, 50, 840, 600);
        auto* content = window.getContentComponent();
        expect (content != nullptr);
        if (content != nullptr)
        {
            const auto scale = ActiveCuesWindow::scaleFor (content->getWidth(), content->getHeight(), 2);
            expectWithinAbsoluteError (window.getPanel().getTransform().mat00, scale, 1.0e-6f);
        }
        const auto savedBounds = window.getBounds();
        window.closeButtonPressed();
        auto restored = std::make_unique<HiddenWindow> (f.engine, f.document().cues, f.settings);
        expect (! restored->isVisible());
        restored->open();   // the saved state comes back once the window is on the desktop (a maximised one needs its peer)
        expect (restored->getBounds() == savedBounds);
        restored.reset();
        {
            // minimised, the window saves nothing (Windows reports a minimised window as not maximised): the last real
            // state is kept
            window.open();
            window.setBounds (60, 70, 700, 500);
            window.closeButtonPressed();
            const auto kept = f.settings.getActiveCuesWindowState();
            window.open();
            window.setMinimised (true);
            window.setBounds (300, 300, 500, 400);   // a move while minimised
            expectEquals (f.settings.getActiveCuesWindowState(), kept);
            window.setMinimised (false);
        }
        {
            // a maximised state comes back maximised (the normal place is restored first, then maximised)
            auto maximisedState = "fs " + juce::Rectangle<int> (80, 90, 720, 520).toString();
            f.settings.setActiveCuesWindowState (maximisedState);
            auto again = std::make_unique<HiddenWindow> (f.engine, f.document().cues, f.settings);
            again->open();
            expect (again->isFullScreen());
            again.reset();
        }
        window.setBounds (-20000, -20000, 640, 480);
        window.open();
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window.getScreenBounds()))
            expect (display->userBounds.toNearestInt().contains (window.getBounds()), "off-screen placement is brought back into view");
        const int scaleBefore = UiScale::currentPercent();
        f.command (CommandIDs::uiScale125);
        ReopenLastProjectTestAccess::refreshPlayback (*f.main);
        expectEquals (window.getPanel().getCardCount(), 2);
        UiScale::apply (scaleBefore);

        beginTest ("project replacement keeps the window alive and removes old cards");
        f.engine.stopAll(); f.settle();
        auto& controller = ReopenLastProjectTestAccess::controller (*f.main);
        controller.resetForNewProject();
        f.document().newProject(); f.document().settings.autoBackup = false;
        ReopenLastProjectTestAccess::refreshPlayback (*f.main);
        expect (&window == ReopenLastProjectTestAccess::bigView (*f.main) && window.isVisible());
        expectEquals (window.getPanel().getCardCount(), 0);
        Project project; project.cues().push_back (sound); project.settings.autoBackup = false;
        const auto file = f.scratch.folder.getChildFile ("reopened.enqueue");
        expect (ProjectSerializer::save (project, file).wasOk());
        f.main->openProjectFile (file, false);
        f.settle(); // project reset/chain gate is acknowledged by the audio callback before the next play
        expect (f.engine.play (sound));
        ReopenLastProjectTestAccess::refreshPlayback (*f.main);
        expectEquals (window.getPanel().getCardCount(), 1);
    }
};

class ActiveCuesWindowKeyboardTests : public juce::UnitTest
{
public:
    ActiveCuesWindowKeyboardTests() : UnitTest ("ActiveCuesWindowKeyboard", "Enqueue") {}
    void runTest() override
    {
        Fixture f;
        const auto a = f.addSound ("A");
        const auto b = f.addSound ("B");
        const auto c = f.addSound ("C");
        f.document().cues.update (2, [] (Cue& cue) { cue.hotkey = "H"; });
        f.document().cues.setSelectedIndex (0);
        f.document().cues.setPlayheadIndex (0);
        f.document().settings.requireKeyUp = true;
        f.document().settings.doubleGoSeconds = 0.5;
        ReopenLastProjectTestAccess::useJucePanicFallback (*f.main);
        auto& router = ReopenLastProjectTestAccess::keyboard (*f.main);
        router.applicationActiveChanged (true);
        auto& window = f.makeWindow();
        auto& origin = window.getPanel();
        const juce::KeyPress space (juce::KeyPress::spaceKey), esc (juce::KeyPress::escapeKey), hotkey ('H');
        beginTest ("big view has main window scope and a fixed table-key owner");
        const auto context = router.contextFor (&origin, space);
        expect (context.window == ShortcutKeyContext::Window::main && context.focus == ShortcutScope::cueTable);

        beginTest ("Space uses the main GO path, held-key release and double-GO gate across both windows");
        expect (f.key (space, origin));
        expect (f.engine.isPlaying (a.id));
        expect (! f.engine.isPlaying (b.id));
        f.now += 1.0;
        expect (f.key (space, *child<juce::TableListBox> (f.table())));
        expect (! f.engine.isPlaying (b.id)); // held key is not another GO
        f.release (space);
        expect (f.key (space, origin));
        expect (f.engine.isPlaying (b.id));
        f.release (space);
        f.now += 0.1;
        expect (f.key (space, origin));
        expect (! f.engine.isPlaying (c.id)); // freshly pressed but inside double-GO window
        f.release (space);

        beginTest ("P pauses without changing focus; cue hotkey dispatches from big-view descendants");
        const juce::KeyPress pause ('P');
        f.key (pause, origin); f.release (pause);
        f.settle(); // pause has a short audio ramp
        expect (f.engine.isPaused (b.id));
        f.key (pause, origin); f.release (pause);
        f.settle();
        expect (! f.engine.isPaused (b.id));
        f.now += 1.0;
        expect (f.key (hotkey, origin)); f.release (hotkey);
        expect (f.engine.isPlaying (c.id));

        beginTest ("arrow selection reaches the existing table, and Ctrl+S saves through the main target");
        f.document().cues.setSelectedIndex (0);
        const juce::KeyPress down (juce::KeyPress::downKey);
        expect (f.key (down, origin)); f.release (down);
        expectEquals (f.document().cues.getSelectedIndex(), 1);
        const auto file = f.scratch.folder.getChildFile ("keys.enqueue");
        expect (ReopenLastProjectTestAccess::saveAs (*f.main, file));
        f.document().perform ("Rename", [&] { f.document().cues.update (0, [] (Cue& cue) { cue.name = "Saved from big view"; }); });
        const juce::KeyPress save ('S', juce::ModifierKeys::ctrlModifier, 0);
        expect (f.key (save, origin)); f.release (save);
        Project saved;
        expect (ProjectSerializer::load (file, saved).wasOk());
        expectEquals (saved.cues()[0].name, juce::String ("Saved from big view"));

        beginTest ("Esc uses the same panic gesture: fade, then hard stop inside 0.5 seconds");
        f.key (esc, origin, 200000.0); f.release (esc);
        expect (f.engine.isStopping (a.id) && f.engine.isStopping (b.id) && f.engine.isStopping (c.id));
        f.key (esc, origin, 200200.0); f.release (esc);
        f.settle();
        expectEquals (f.engine.getNumPlaying(), 0);
    }
};
static ActiveCuesWindowPanelTests activeCuesWindowPanelTests;
static ActiveCuesWindowIntegrationTests activeCuesWindowIntegrationTests;
static ActiveCuesWindowKeyboardTests activeCuesWindowKeyboardTests;
}
}
