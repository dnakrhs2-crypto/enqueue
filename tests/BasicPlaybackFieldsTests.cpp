#include "VolumeCueUiTestSupport.h"
#include "ui/InspectorLayout.h"
#include "ui/TimeLoopsPanel.h"

namespace gocue::tests
{
namespace
{
using namespace volume_ui;

class BasicPlaybackFieldsTests : public juce::UnitTest
{
public:
    BasicPlaybackFieldsTests() : UnitTest ("Basic playback fields", "Enqueue") {}

    struct Fields
    {
        std::array<juce::TextEditor*, 5> editors {};
        juce::ToggleButton* infinite = nullptr;
        juce::ToggleButton* pitch = nullptr;
        juce::Label* length = nullptr;
    };

    static void addSound (Fixture& f, double length = 10.0)
    {
        f.addSound();
        f.document().cues.update (0, [length] (Cue& c)
        {
            c.durationSeconds = length;
            c.audio.infiniteLoop = false;
        });
        f.document().cues.setSelectedIndex (0);
        dispatch();
    }

    static juce::Component& openTab (Fixture& f, int index)
    {
        auto* tabs = child<juce::TabbedComponent> (f.inspector());
        tabs->setCurrentTabIndex (index);
        dispatch();
        return *dynamic_cast<juce::Viewport*> (tabs->getCurrentContentComponent())->getViewedComponent();
    }

    Fields fields (juce::Component& panel)
    {
        Fields result;
        const char* labels[] { "시작", "끝", "재생 횟수", "속도", "페이드 인" };
        for (size_t i = 0; i < result.editors.size(); ++i)
        {
            auto* label = child<juce::Label> (panel, [i, &labels] (const auto& l) { return l.getText() == ko (labels[i]); });
            expect (label != nullptr);
            if (label == nullptr) continue;
            // Both layouts put the editor immediately to the right of its label.
            for (auto* c : panel.getChildren())
                if (auto* e = dynamic_cast<juce::TextEditor*> (c); e != nullptr && e->getY() == label->getY()
                    && e->getX() >= label->getRight() && (result.editors[i] == nullptr || e->getX() < result.editors[i]->getX()))
                    result.editors[i] = e;
            expect (result.editors[i] != nullptr);
        }
        result.infinite = child<juce::ToggleButton> (panel, [] (const auto& b) { return b.getButtonText() == ko ("무한 루프"); });
        result.pitch = child<juce::ToggleButton> (panel, [] (const auto& b) { return b.getButtonText() == ko ("피치 유지"); });
        result.length = child<juce::Label> (panel, [] (const auto& l) { return l.getText().startsWith (ko ("구간 ")); });
        expect (result.infinite != nullptr && result.pitch != nullptr && result.length != nullptr);
        return result;
    }

    static void enter (juce::TextEditor& editor, const juce::String& text)
    {
        editor.setText (text, false);
        editor.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        dispatch();
    }

    static juce::String snapshot (Fixture& f) { return ProjectSerializer::toJson (f.document().toProject()); }

    static double value (const Cue& c, int field)
    {
        const double values[] { c.regionStart(), c.regionEnd(), (double) c.audio.playCount, c.audio.rate,
                                c.audio.envelope.fadeInSeconds (c.regionLength()) };
        return values[field];
    }

    void same (const Fields& a, const Fields& b)
    {
        for (size_t i = 0; i < a.editors.size(); ++i)
            expectEquals (a.editors[i]->getText(), b.editors[i]->getText());
        expect (a.infinite->getToggleState() == b.infinite->getToggleState());
        expect (a.pitch->getToggleState() == b.pitch->getToggleState());
        expectEquals (a.length->getText(), b.length->getText());
    }

    void runTest() override
    {
        beginTest ("all seven fields synchronise both ways and basic edits have the play tab's single undo step");
        {
            const char* names[] { "시작 시간", "끝 시간", "재생 횟수", "속도", "페이드 인 시간", "무한 루프 켜기", "피치 유지" };
            const char* inputs[] { "1.23456", "8.76543", "3", "1.2345", "2.5" };
            const double wanted[] { 1.23456, 8.76543, 3.0, 1.2345, 2.5 };
            for (int source : { 0, 1 })
                for (int field = 0; field < 7; ++field)
                {
                    Fixture f;
                    addSound (f);
                    f.hiddenMain();
                    auto basic = fields (openTab (f, 0));
                    auto play = fields (openTab (f, 1));
                    same (basic, play);
                    openTab (f, source);
                    auto& from = source == 0 ? basic : play;
                    const auto before = snapshot (f);
                    if (field < 5)
                    {
                        from.editors[(size_t) field]->grabKeyboardFocus();
                        expect (from.editors[(size_t) field]->hasKeyboardFocus (false));
                        enter (*from.editors[(size_t) field], inputs[field]);
                        expectWithinAbsoluteError (value (f.document().cues.get (0), field), wanted[field], 1e-9);
                    }
                    else
                    {
                        (field == 5 ? from.infinite : from.pitch)->triggerClick();
                        dispatch();
                        expect (field == 5 ? f.document().cues.get (0).audio.infiniteLoop : f.document().cues.get (0).audio.preservePitch);
                    }
                    same (basic, play);   // the hidden tab has already refreshed, without opening it
                    expectEquals (f.document().getHistory().getUndoDepth(), 1);
                    expectEquals (f.document().getUndoName(), ko (names[field]));
                    const auto after = snapshot (f);
                    expect (f.document().undo());
                    dispatch();
                    same (basic, play);
                    expectEquals (snapshot (f), before);
                    expect (! f.document().canUndo());
                    expect (f.document().redo());
                    dispatch();
                    expectEquals (snapshot (f), after);
                    same (basic, play);
                }
        }

        beginTest ("all five basic inputs commit to the original cue when another row is selected without Enter");
        {
            const char* inputs[] { "1", "8", "3", "1.5", "5" };
            const double wanted[] { 1.0, 8.0, 3.0, 1.5, 5.0 };
            for (const auto nextType : { CueType::audio, CueType::group, CueType::mic, CueType::fade })
                for (bool focusFirst : { true, false })
                    for (int field = 0; field < 5; ++field)
                    {
                        Fixture f;
                        addSound (f);
                        f.addSound ("Next");
                        f.document().cues.update (1, [nextType] (Cue& c) { c.type = nextType; });
                        f.document().cues.setSelectedIndex (0);
                        f.hiddenMain();
                        auto basic = fields (openTab (f, 0));
                        auto* editor = basic.editors[(size_t) field];
                        const auto before = snapshot (f);
                        const auto next = f.document().cues.get (1);
                        editor->grabKeyboardFocus();
                        expect (editor->hasKeyboardFocus (false));
                        editor->setText (inputs[field], false);
                        if (focusFirst)
                        {
                            f.table().grabKeyboardFocus();
                            expectWithinAbsoluteError (value (f.document().cues.get (0), field), wanted[field], 1e-9,
                                                       "committed synchronously before the row click selects");
                        }
                        f.document().cues.setSelectedIndex (1);
                        f.table().grabKeyboardFocus();
                        dispatch();
                        expectEquals (f.document().cues.getSelectedIndex(), 1);
                        expectWithinAbsoluteError (value (f.document().cues.get (0), field), wanted[field], 1e-9);
                        for (int i = 0; i < 5; ++i)
                            expectWithinAbsoluteError (value (f.document().cues.get (1), i), value (next, i), 1e-9);
                        expectEquals (f.document().getHistory().getUndoDepth(), 1);
                        expect (f.document().undo());
                        dispatch();
                        expectEquals (snapshot (f), before);
                        expect (! f.document().canUndo());
                    }
        }

        beginTest ("Escape and model replacement discard pending basic inputs; rounded displays do not dirty history");
        {
            for (int field = 0; field < 5; ++field)
            {
                Fixture f;
                addSound (f);
                f.document().cues.update (0, [] (Cue& c)
                {
                    c.audio.startSeconds = 1.23456;
                    c.audio.endSeconds = 8.76543;
                    c.audio.rate = 1.2345;
                    c.audio.envelope = Envelope::fromFadeIn (2.123456);
                });
                f.hiddenMain();
                auto basic = fields (openTab (f, 0));
                auto* editor = basic.editors[(size_t) field];
                const auto before = snapshot (f);
                const auto display = editor->getText();
                f.document().markClean();
                editor->grabKeyboardFocus();
                f.table().grabKeyboardFocus();
                dispatch();
                enter (*editor, display);
                expectEquals (snapshot (f), before);
                expect (! f.document().canUndo() && ! f.document().isDirty());
                editor->grabKeyboardFocus();
                editor->setText ("6", false);
                editor->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
                dispatch();
                expectEquals (editor->getText(), display);
                expectEquals (snapshot (f), before);
                expect (! f.document().canUndo());
                f.document().perform ("name", [&] { f.document().cues.update (0, [] (Cue& c) { c.name = "Renamed"; }); });
                editor->grabKeyboardFocus();
                editor->setText ("6", false);
                expect (f.document().undo());
                dispatch();
                expectEquals (editor->getText(), display);
                f.table().grabKeyboardFocus();
                dispatch();
                expectEquals (snapshot (f), before);
                expect (! f.document().canUndo());
                expectEquals (f.document().getHistory().getRedoDepth(), 1);
                expect (f.document().redo());
                dispatch();
                expectEquals (editor->getText(), display);
            }
        }

        beginTest ("time and numeric bounds match playback rules and invalid input restores the model value");
        {
            struct Edit { int field; const char* input; double wanted; };
            const Edit edits[] { { 0, "99", 10.0 - WaveformView::minRegionSeconds }, { 1, "99", 10.0 },
                                 { 1, "0", 2.0 + WaveformView::minRegionSeconds }, { 2, "0", 1.0 },
                                 { 2, "9999", (double) AudioCueData::maxPlayCount }, { 3, "0", AudioCueData::minRate },
                                 { 3, "99", AudioCueData::maxRate }, { 4, "99", 8.0 }, { 4, "0:03.5", 3.5 } };
            for (const auto& edit : edits)
            {
                Fixture f;
                addSound (f);
                if (edit.field != 0)
                    f.document().cues.update (0, [] (Cue& c) { c.audio.startSeconds = 2.0; c.audio.playCount = 2; });
                auto basic = fields (openTab (f, 0));
                auto* editor = basic.editors[(size_t) edit.field];
                enter (*editor, edit.input);
                expectWithinAbsoluteError (value (f.document().cues.get (0), edit.field), edit.wanted, 1e-9);
                if (edit.field == 1 && edit.wanted == 10.0)
                    expectWithinAbsoluteError (f.document().cues.get (0).audio.endSeconds, -1.0, 1e-9);
                const auto before = snapshot (f);
                const int depth = f.document().getHistory().getUndoDepth();
                enter (*editor, "");
                expectEquals (snapshot (f), before);
                expectEquals (f.document().getHistory().getUndoDepth(), depth);
            }
        }

        beginTest ("minute input commits on focus loss and an ordinary document update preserves pending typing");
        {
            Fixture f;
            addSound (f, 120.0);
            f.hiddenMain();
            auto basic = fields (openTab (f, 0));
            for (int field : { 0, 1, 4 })
            {
                auto* editor = basic.editors[(size_t) field];
                editor->grabKeyboardFocus();
                const auto input = juce::String (field == 0 ? "0:02.5" : field == 1 ? "1:02.5" : "0:03.5");
                editor->setText (input, false);
                f.document().cues.update (0, [] (Cue& c) { c.name = "Name changed elsewhere"; });
                expectEquals (editor->getText(), input);
                f.table().grabKeyboardFocus();
                dispatch();
                expectWithinAbsoluteError (value (f.document().cues.get (0), field),
                                           field == 0 ? 2.5 : field == 1 ? 62.5 : 3.5, 1e-9);
                const auto before = snapshot (f);
                const int depth = f.document().getHistory().getUndoDepth();
                enter (*editor, "1::2");
                expectEquals (snapshot (f), before);
                expectEquals (f.document().getHistory().getUndoDepth(), depth);
            }
        }

        beginTest ("show mode, no selection, infinite count and zero length disable the matching fields");
        {
            Fixture f;
            addSound (f);
            auto& panel = openTab (f, 0);
            auto basic = fields (panel);
            auto play = fields (openTab (f, 1));
            openTab (f, 0);
            const auto before = snapshot (f);
            expect (f.command (CommandIDs::toggleShowMode));
            for (auto* e : basic.editors) { expect (! e->isEnabled()); enter (*e, "3"); }
            expect (! basic.infinite->isEnabled() && ! basic.pitch->isEnabled());
            expectEquals (snapshot (f), before);
            f.document().cues.setSelectedIndex (-1);
            f.document().cues.setSelectedIndex (0);   // selection also refreshes the hidden play tab while locked
            expect (f.command (CommandIDs::toggleShowMode));
            for (auto* e : basic.editors) expect (e->isEnabled());
            for (auto* e : play.editors) expect (e->isEnabled());
            basic.infinite->triggerClick();
            dispatch();
            expect (! basic.editors[2]->isEnabled() && ! play.editors[2]->isEnabled());
            expectEquals (f.document().getUndoName(), ko ("무한 루프 켜기"));
            same (basic, play);
            basic.infinite->triggerClick();
            dispatch();
            expectEquals (f.document().getUndoName(), ko ("무한 루프 끄기"));
            f.document().cues.update (0, [] (Cue& c) { c.durationSeconds = 0.0; });
            expect (! basic.editors[4]->isEnabled() && ! play.editors[4]->isEnabled());
            f.document().cues.setSelectedIndex (-1);
            dispatch();
            for (auto* e : basic.editors)
            {
                expect (e->isVisible() && ! e->isEnabled());
                expect (e->getText().isEmpty());
            }
            expect (basic.infinite->isVisible() && ! basic.infinite->isEnabled());
            expect (basic.pitch->isVisible() && ! basic.pitch->isEnabled() && ! play.pitch->isEnabled());
            same (basic, play);
            auto* memo = child<juce::TextEditor> (panel, [] (const auto& e) { return e.isMultiLine(); });
            expectEquals (memo->getY(), 122);
            expectGreaterThan (memo->getX(), panel.getWidth() / 2);
        }

        beginTest ("non-audio cues hide playback fields and return the memo below three full-width rows");
        {
            Fixture f;
            addSound (f);
            auto& panel = openTab (f, 0);
            auto basic = fields (panel);
            auto* memo = child<juce::TextEditor> (panel, [] (const auto& e) { return e.isMultiLine(); });
            for (const auto type : { CueType::group, CueType::mic, CueType::fade })
            {
                f.document().cues.update (0, [type] (Cue& c) { c.type = type; });
                dispatch();
                for (auto* e : basic.editors) expect (! e->isVisible());
                expect (! basic.infinite->isVisible() && ! basic.pitch->isVisible() && ! basic.length->isVisible());
                expectEquals (memo->getBounds().toString(), juce::Rectangle<int> (48, 122, panel.getWidth() - 60, panel.getHeight() - 130).toString());
                f.document().cues.setSelectedIndex (-1);
                dispatch();
                for (auto* e : basic.editors) expect (e->isVisible() && ! e->isEnabled() && e->getText().isEmpty());
                expectGreaterThan (memo->getX(), panel.getWidth() / 2);
                f.document().cues.setSelectedIndex (0);
            }
        }

        beginTest ("region label includes total time, infinite repeats and infinite slices on both tabs");
        {
            Fixture f;
            addSound (f);
            auto basic = fields (openTab (f, 0));
            auto play = fields (openTab (f, 1));
            f.document().cues.update (0, [] (Cue& c) { c.audio.playCount = 3; c.audio.rate = 2; });
            same (basic, play);
            expect (basic.length->getText().contains (ko ("  · 전체 ")));
            f.document().cues.update (0, [] (Cue& c) { c.audio.infiniteLoop = true; });
            same (basic, play);
            expect (basic.length->getText().endsWith (ko ("무한 반복")));
            f.document().cues.update (0, [] (Cue& c) { c.audio.infiniteLoop = false; c.audio.slices = { { 5.0, -1 } }; });
            same (basic, play);
            expect (basic.length->getText().endsWith (ko ("무한 슬라이스")));
        }

        beginTest ("a running cue follows basic end, rate, count, infinite loop and fade-in edits immediately");
        {
            Fixture f;
            addSound (f, 1.0);
            auto basic = fields (openTab (f, 0));
            const auto id = f.document().cues.get (0).id;
            expect (f.engine.play (f.document().cues.get (0)));
            juce::AudioBuffer<float> block (2, 512);
            for (int i = 0; i < 8; ++i) f.engine.renderBlock (block, 512);
            const float full = block.getRMSLevel (0, 0, 512);
            expectGreaterThan (full, 0.05f);
            enter (*basic.editors[4], "0.5");
            f.engine.renderBlock (block, 512);
            expectLessThan (block.getRMSLevel (0, 0, 512), full * 0.4f);
            expectGreaterThan (block.getRMSLevel (0, 0, 512), full * 0.1f);
            enter (*basic.editors[4], "0");
            enter (*basic.editors[1], "0.8");
            expectWithinAbsoluteError (f.engine.getPlayingCues()[0].lengthSeconds, 0.8, 1e-6);
            enter (*basic.editors[3], "2");
            expectWithinAbsoluteError (f.engine.getLiveRate (id), 2.0, 1e-9);
            expectWithinAbsoluteError (f.engine.getPlayingCues()[0].lengthSeconds, 0.4, 1e-6);
            enter (*basic.editors[2], "3");
            expectWithinAbsoluteError (f.engine.getPlayingCues()[0].lengthSeconds, 1.2, 1e-6);
            basic.infinite->triggerClick();
            dispatch();
            expectLessThan (f.engine.getPlayingCues()[0].lengthSeconds, 0.0);
            for (int i = 0; i < 120; ++i) f.engine.renderBlock (block, 512);
            expect (f.engine.isPlaying (id));
            basic.infinite->triggerClick();
            dispatch();
            enter (*basic.editors[2], "1");
            for (int i = 0; i < 80; ++i) { f.engine.renderBlock (block, 512); f.engine.reapFinishedPlayers(); }
            expect (! f.engine.isPlaying (id));
        }
    }
};

static BasicPlaybackFieldsTests basicPlaybackFieldsTests;
}
}
