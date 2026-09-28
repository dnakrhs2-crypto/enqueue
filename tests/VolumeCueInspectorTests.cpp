#include "VolumeCueUiTestSupport.h"

namespace gocue::tests
{
namespace
{
class VolumeCueInspectorTests : public juce::UnitTest
{
public:
    VolumeCueInspectorTests() : UnitTest ("VolumeCueInspector", "Enqueue") {}
    void runTest() override
    {
        using namespace volume_ui;
        Fixture f;
        const auto sound = f.addSound();
        f.document().cues.setSelectedIndex (0);

        beginTest ("add command uses selection, defaults and one undo step; follows selected fade target");
        expect (f.command (CommandIDs::addVolumeCue));
        expectEquals (f.document().cues.size(), 2);
        auto volume = *f.document().cues.getSelected();
        expect (volume.isFade() && volume.fade.mode == FadeMode::volume && volume.fade.targetId == sound.id);
        expectWithinAbsoluteError (volume.fade.durationSeconds, 2.0, 1.0e-9);
        expectWithinAbsoluteError (volume.fade.mainDb, -6.0, 1.0e-9);
        expectEquals (volume.name, ko ("볼륨: Music"));
        FadeCurve defaultCurve; defaultCurve.sanitise();
        expect (volume.fade.curve == defaultCurve);
        expect (f.command (CommandIDs::addVolumeCue));
        expect (f.document().cues.getSelected()->fade.targetId == sound.id);
        f.document().undo();
        expectEquals (f.document().cues.size(), 2);
        f.document().undo();
        expectEquals (f.document().cues.size(), 1);
        expect (! f.document().canUndo());
        f.document().cues.add (volume);
        f.document().cues.setSelectedIndex (1);
        f.inspector().showTimeTab();
        dispatch();

        auto* mode = child<juce::ComboBox> (f.inspector(), [] (const auto& c) { return c.getItemText (0) == ko ("페이드 인 (무음에서 올리기)"); });
        auto* offset = child<juce::TextEditor> (f.inspector(), [] (auto& c) { return c.getTooltip() == ko ("원래 볼륨 대비 (dB, 0 = 원래 볼륨으로)"); });
        expect (mode != nullptr && offset != nullptr);
        if (mode == nullptr || offset == nullptr) return;
        auto level = [&] { return f.document().cues.get (1).fade.mainDb; };
        auto undo = [&] { expect (f.document().canUndo()); f.document().undo(); expect (! f.document().canUndo()); };

        beginTest ("all four kind IDs, custom only for legacy cue, switching to volume defaults to -6 in one undo");
        expectEquals (mode->getSelectedId(), 3);
        expectEquals (mode->getNumItems(), 3);
        for (const auto id : { 1, 2 })
        {
            mode->setSelectedId (id, juce::sendNotificationSync);
            expect (f.document().cues.get (1).fade.mode == (id == 1 ? FadeMode::fadeIn : FadeMode::fadeOut));
            expect (! offset->isVisible());
            undo();
        }
        f.document().cues.update (1, [] (Cue& cue) { cue.fade.mode = FadeMode::custom; cue.fade.mainDb = 8.0; });
        expectEquals (mode->getSelectedId(), 4);
        expectEquals (mode->getNumItems(), 4);
        expectEquals (mode->getItemId (3), 4);
        mode->setSelectedId (3, juce::sendNotificationSync);
        expect (f.document().cues.get (1).fade.mode == FadeMode::volume && level() == -6.0);
        expectEquals (mode->getNumItems(), 3);
        undo();
        expect (f.document().cues.get (1).fade.mode == FadeMode::custom && level() == 8.0);
        f.document().cues.update (1, [] (Cue& cue) { cue.fade.mode = FadeMode::volume; cue.fade.mainDb = -6.0; });

        beginTest ("offset field accepts signed values, clamps, rejects text and commits Enter/focus loss only once");
        const char* inputs[] { "-6", "+3", "0", "-4.5", "999", "-999", "letters" };
        const double expected[] { -6, 3, 0, -4.5, 24, -120, -6 };
        for (int i = 0; i < 7; ++i)
        {
            offset->setText (inputs[i], false);
            offset->onReturnKey();
            offset->onFocusLost();
            expectWithinAbsoluteError (level(), expected[i], 1.0e-9);
            if (expected[i] != -6) undo();
            else expect (! f.document().canUndo());
            expect (! offset->getText().contains ("letters"));
        }

        beginTest ("actual focus loss commits to the old cue before row selection and queued callbacks do not add history");
        f.hiddenMain();
        offset->grabKeyboardFocus();
        expect (offset->hasKeyboardFocus (false));
        offset->setText ("-4.5", false);
        f.table().grabKeyboardFocus();
        f.document().cues.setSelectedIndex (0);
        dispatch();
        expectWithinAbsoluteError (level(), -4.5, 1.0e-9);
        undo();
        dispatch();
        expectWithinAbsoluteError (level(), -6.0, 1.0e-9);
        expect (! f.document().canUndo());
        f.document().cues.setSelectedIndex (1);
        f.inspector().showTimeTab();

        beginTest ("a quick button while the field has the keyboard: the button's value wins and the field lets go (Space is GO again)");
        offset->grabKeyboardFocus();
        expect (offset->hasKeyboardFocus (false));
        offset->setText ("-12", false);
        if (auto* minus3 = child<juce::TextButton> (f.inspector(), [] (const auto& b) { return b.getButtonText() == "-3"; }))
        {
            minus3->onClick();
            expectWithinAbsoluteError (level(), -3.0, 1.0e-9);
            expect (! offset->hasKeyboardFocus (true), "the field gives the keyboard back");
            dispatch();
            expectWithinAbsoluteError (level(), -3.0, 1.0e-9);   // the focus loss that followed did not commit the typed -12
            undo();
        }
        else
        {
            expect (false, "-3 button");
        }
        expectWithinAbsoluteError (level(), -6.0, 1.0e-9);

        beginTest ("show mode keeps what was typed without Enter (like a focus change) before it locks the field");
        offset->grabKeyboardFocus();
        offset->setText ("-12", false);
        expect (f.command (CommandIDs::toggleShowMode));
        expectWithinAbsoluteError (level(), -12.0, 1.0e-9);
        expect (f.command (CommandIDs::toggleShowMode));
        expectWithinAbsoluteError (level(), -12.0, 1.0e-9);
        expectEquals (offset->getText(), juce::String ("-12"));
        undo();
        dispatch();
        expectWithinAbsoluteError (level(), -6.0, 1.0e-9);

        beginTest ("quick buttons and show mode lock all volume controls without extra history");
        const char* names[] { "-3", "-6", "원래대로" };
        const double goals[] { -3, -6, 0 };
        for (int i = 0; i < 3; ++i)
        {
            auto* button = child<juce::TextButton> (f.inspector(), [&] (const auto& b) { return b.getButtonText() == ko (names[i]); });
            expect (button != nullptr);
            if (button == nullptr) continue;
            expect (! button->getWantsKeyboardFocus() && button->isVisible());
            button->onClick();
            expectWithinAbsoluteError (level(), goals[i], 1.0e-9);
            button->onClick();
            if (goals[i] != -6) undo(); else expect (! f.document().canUndo());
        }
        expect (f.command (CommandIDs::toggleShowMode));
        expect (! mode->isEnabled() && ! offset->isEnabled());
        for (const auto* buttonName : names)
        {
            auto* button = child<juce::TextButton> (f.inspector(), [&] (const auto& b) { return b.getButtonText() == ko (buttonName); });
            if (button != nullptr) { expect (! button->isEnabled()); button->onClick(); }
        }
        offset->setText ("+3", false); offset->onFocusLost();
        expectWithinAbsoluteError (level(), -6.0, 1.0e-9);
        const auto count = f.document().cues.size();
        f.command (CommandIDs::addVolumeCue);
        expectEquals (f.document().cues.size(), count);
        expect (! f.document().canUndo());
        juce::ApplicationCommandInfo info (CommandIDs::showActiveCuesWindow);
        f.main->getCommandInfo (CommandIDs::showActiveCuesWindow, info);
        expect ((info.flags & juce::ApplicationCommandInfo::isDisabled) == 0);

        beginTest ("fetching the target's level stores an offset for volume cues");
        f.command (CommandIDs::toggleShowMode);
        f.document().cues.update (0, [] (Cue& cue) { cue.gainDb = -3.0; });
        expect (f.engine.play (f.document().cues.get (0)));
        f.engine.setLiveGainDb (sound.id, -7.5);
        f.command (CommandIDs::fetchFadeLevels);
        expectWithinAbsoluteError (level(), -4.5, 1.0e-9);
        undo();

        beginTest ("curve tab survives volume mode and add menu places volume after fade-out");
        auto* tabs = child<juce::TabbedComponent> (f.inspector());
        expect (tabs != nullptr && tabs->getTabNames().contains (ko ("커브")));
        const auto menu = f.main->getMenuForIndex (2, {});
        bool follows = false;
        int previous = 0;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        {
            const auto id = it.getItem().itemID;
            if (id == CommandIDs::addVolumeCue) follows = previous == CommandIDs::addFadeOutCue;
            previous = id;
        }
        expect (follows);
    }
};
static VolumeCueInspectorTests volumeCueInspectorTests;
}
}
