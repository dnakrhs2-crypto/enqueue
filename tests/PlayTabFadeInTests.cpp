#include "VolumeCueUiTestSupport.h"
#include "ui/TimeLoopsPanel.h"
#include "ui/UiUtils.h"

namespace gocue::tests
{
namespace
{
using namespace volume_ui;

class PlayTabFadeInTests : public juce::UnitTest
{
public:
    PlayTabFadeInTests() : UnitTest ("Play tab fade-in", "Enqueue") {}

    static void addSound (Fixture& f, double length = 90.0)
    {
        f.addSound();
        f.document().cues.update (0, [length] (Cue& c) { c.durationSeconds = length; });
        f.document().cues.setSelectedIndex (0);
        dispatch();
    }

    juce::TextEditor* openEditor (Fixture& f)
    {
        f.inspector().showTimeTab();
        dispatch();
        auto* panel = child<TimeLoopsPanel> (f.inspector());
        expect (panel != nullptr);
        if (panel == nullptr)
            return nullptr;

        // Construction order: start, end, play count, rate, fade-in (independent of the layout).
        int index = 0;
        auto* editor = child<juce::TextEditor> (*panel, [&index] (const auto&) { return index++ == 4; });
        expect (editor != nullptr);
        return editor;
    }

    static void enter (juce::TextEditor& editor, const juce::String& text)
    {
        editor.setText (text, false);
        editor.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        dispatch();
    }

    static juce::String snapshot (Fixture& f) { return ProjectSerializer::toJson (f.document().toProject()); }

    void expectSeconds (Fixture& f, double seconds)
    {
        const auto& cue = f.document().cues.get (0);
        expectWithinAbsoluteError (cue.audio.envelope.fadeInSeconds (cue.regionLength()), seconds, 1e-9);
    }

    void runTest() override
    {
        beginTest ("Enter sets three seconds in one undo step; undo and redo refresh the field");
        {
            Fixture f;
            addSound (f);
            f.hiddenMain();
            auto* editor = openEditor (f);
            if (editor == nullptr) return;
            const auto before = snapshot (f);
            expect (editor->isEnabled());
            expectEquals (editor->getText(), juce::String ("0:00.000"));
            editor->grabKeyboardFocus();
            expect (editor->hasKeyboardFocus (false));
            enter (*editor, "3");
            expectSeconds (f, 3.0);
            expectEquals (editor->getText(), juce::String ("0:03.000"));
            expectEquals (f.document().getUndoName(), ko ("페이드 인 시간"));
            expectEquals (f.document().getHistory().getUndoDepth(), 1);
            expect (f.document().undo());
            dispatch();
            expectEquals (snapshot (f), before);
            expectEquals (editor->getText(), juce::String ("0:00.000"));
            expect (! f.document().canUndo());
            expect (f.document().redo());
            dispatch();
            expectSeconds (f, 3.0);
            expectEquals (editor->getText(), juce::String ("0:03.000"));
        }

        beginTest ("minutes and seconds commit on focus loss; values clamp to the trimmed region");
        {
            Fixture f;
            addSound (f);
            f.document().cues.update (0, [] (Cue& c) { c.audio.startSeconds = 5.0; c.audio.endSeconds = 75.0; });
            f.hiddenMain();
            auto* editor = openEditor (f);
            if (editor == nullptr) return;
            editor->grabKeyboardFocus();
            expect (editor->hasKeyboardFocus (false));
            editor->setText ("1:02.5", false);
            f.table().grabKeyboardFocus();
            dispatch();
            expectSeconds (f, 62.5);
            expectEquals (editor->getText(), juce::String ("1:02.500"));
            expectEquals (f.document().getHistory().getUndoDepth(), 1);
            enter (*editor, "99");
            expectSeconds (f, 70.0);
            expectEquals (editor->getText(), juce::String ("1:10.000"));
            enter (*editor, "0");
            expectSeconds (f, 0.0);
            expectEquals (editor->getText(), juce::String ("0:00.000"));
        }

        beginTest ("unreadable or empty input restores the value without changing the document or history");
        {
            Fixture f;
            addSound (f);
            f.document().cues.update (0, [] (Cue& c) { c.audio.envelope = Envelope::fromFadeIn (2.0); });
            auto* editor = openEditor (f);
            if (editor == nullptr) return;
            f.document().markClean();
            const auto before = snapshot (f);
            for (const char* text : { "letters", "1::2", "" })
            {
                enter (*editor, text);
                expectEquals (snapshot (f), before);
                expectEquals (editor->getText(), juce::String ("0:02.000"));
                expect (! f.document().isDirty());
                expect (! f.document().canUndo());
            }
        }

        beginTest ("visiting a rounded field preserves all points and both undo and redo history");
        {
            Fixture f;
            addSound (f);
            f.document().cues.update (0, [] (Cue& c)
            {
                c.audio.envelope = Envelope::fromFadeIn (2.123456);
                c.audio.envelope.points.insert (c.audio.envelope.points.begin() + 1, { 1.0, 0.4 });
            });
            f.document().perform ("gain", [&] { f.document().cues.update (0, [] (Cue& c) { c.gainDb = -3.0; }); });
            f.document().perform ("name", [&] { f.document().cues.update (0, [] (Cue& c) { c.name = "Renamed"; }); });
            expect (f.document().undo());
            f.document().markClean();
            f.hiddenMain();
            auto* editor = openEditor (f);
            if (editor == nullptr) return;
            const auto before = snapshot (f);
            expectEquals (editor->getText(), juce::String ("0:02.123"));
            editor->grabKeyboardFocus();
            expect (editor->hasKeyboardFocus (false));
            f.table().grabKeyboardFocus();
            dispatch();
            enter (*editor, "2.123");
            expectEquals (snapshot (f), before);
            expect (! f.document().isDirty());
            expectEquals (f.document().getHistory().getUndoDepth(), 1);
            expectEquals (f.document().getHistory().getRedoDepth(), 1);
            expectEquals (f.document().getUndoName(), juce::String ("gain"));
            expectEquals (f.document().getRedoName(), juce::String ("name"));

            // An unlocked fade can extend past the end after trimming; viewing it is still not an edit.
            f.document().cues.update (0, [] (Cue& c) { c.audio.endSeconds = 1.0; });
            f.document().markClean();
            const auto trimmed = snapshot (f);
            editor->grabKeyboardFocus();
            f.table().grabKeyboardFocus();
            dispatch();
            expectEquals (snapshot (f), trimmed);
            expect (! f.document().isDirty());
            expectEquals (f.document().getHistory().getUndoDepth(), 1);
            expectEquals (f.document().getHistory().getRedoDepth(), 1);
        }

        beginTest ("document envelope edits refresh the field; Escape cancels pending input");
        {
            Fixture f;
            addSound (f);
            f.hiddenMain();
            auto* editor = openEditor (f);
            if (editor == nullptr) return;
            f.document().cues.update (0, [] (Cue& c) { c.audio.envelope = Envelope::fromFadeIn (4.25); });
            dispatch();
            expectEquals (editor->getText(), juce::String ("0:04.250"));
            const auto before = snapshot (f);
            editor->grabKeyboardFocus();
            editor->setText ("6", false);
            editor->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            dispatch();
            expectEquals (snapshot (f), before);
            expectEquals (editor->getText(), juce::String ("0:04.250"));
            expect (! f.document().canUndo());
        }

        beginTest ("no selection, unknown length and show mode disable the fade-in field");
        {
            Fixture f;
            addSound (f);
            auto* editor = openEditor (f);
            if (editor == nullptr) return;
            f.document().cues.setSelectedIndex (-1);
            dispatch();
            expect (! editor->isEnabled());
            const auto unselected = snapshot (f);
            enter (*editor, "3");
            expectEquals (snapshot (f), unselected);
            expect (! f.document().canUndo());

            f.document().cues.setSelectedIndex (0);
            for (const double length : { 0.0, -1.0 })
            {
                f.document().cues.update (0, [length] (Cue& c) { c.durationSeconds = length; });
                dispatch();
                expect (! editor->isEnabled());
                const auto before = snapshot (f);
                enter (*editor, "3");
                expectEquals (snapshot (f), before);
                expect (! f.document().canUndo());
            }

            f.document().cues.update (0, [] (Cue& c) { c.durationSeconds = 90.0; });
            dispatch();
            expect (editor->isEnabled());
            expect (f.command (CommandIDs::toggleShowMode));
            dispatch();
            expect (! editor->isEnabled());
            expect (f.command (CommandIDs::toggleShowMode));
            dispatch();
            expect (editor->isEnabled());
        }

        beginTest ("focus-loss input uses the edited cue's length when the selection changes");
        {
            Fixture f;
            addSound (f, 10.0);
            f.addSound ("Next");   // one-second region, shorter than the entered fade-in
            f.document().cues.setSelectedIndex (0);
            f.hiddenMain();
            auto* editor = openEditor (f);
            if (editor == nullptr) return;
            editor->grabKeyboardFocus();
            editor->setText ("3", false);
            f.document().cues.setSelectedIndex (1);
            f.table().grabKeyboardFocus();
            dispatch();
            expectSeconds (f, 3.0);
            expect (! f.document().cues.get (1).audio.envelope.isActive());
            expectEquals (editor->getText(), juce::String ("0:00.000"));
            expectEquals (f.document().getHistory().getUndoDepth(), 1);
        }

        beginTest ("a playing cue hears the numeric fade-in immediately without restarting");
        {
            Fixture f;
            addSound (f, 1.0);
            auto* editor = openEditor (f);
            if (editor == nullptr) return;
            const auto id = f.document().cues.get (0).id;
            expect (f.engine.play (f.document().cues.get (0)));
            juce::AudioBuffer<float> block (2, 512);
            for (int i = 0; i < 8; ++i)
                f.engine.renderBlock (block, block.getNumSamples());
            const float full = block.getRMSLevel (0, 0, block.getNumSamples());
            expectGreaterThan (full, 0.05f);
            enter (*editor, "0.5");
            f.engine.renderBlock (block, block.getNumSamples());
            const float faded = block.getRMSLevel (0, 0, block.getNumSamples());
            expectGreaterThan (faded, full * 0.1f);
            expectLessThan (faded, full * 0.4f);
            expect (f.engine.isPlaying (id));
            enter (*editor, "0");
            f.engine.renderBlock (block, block.getNumSamples());
            expectWithinAbsoluteError (block.getRMSLevel (0, 0, block.getNumSamples()), full, full * 0.05f);
        }
    }
};

static PlayTabFadeInTests playTabFadeInTests;
}
}
