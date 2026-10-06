#include "VolumeCueUiTestSupport.h"
#include "ui/InspectorLayout.h"
#include "ui/WaveformView.h"
#include "ui/UiUtils.h"

namespace gocue::tests
{
namespace
{
using namespace volume_ui;

/** The 재생 tab has the cue's gain too (gom 2026-10-06): the 기본 tab's control on the same value, so either one moves
    the other, and dragging it reshapes the wave on the same tab at every step - no tab switch, no message loop. */
class PlayTabGainTests : public juce::UnitTest
{
public:
    PlayTabGainTests() : UnitTest ("Play tab gain", "Enqueue") {}

    static juce::Slider* gainSlider (juce::Component& root)
    {
        return child<juce::Slider> (root, [] (const juce::Slider& s)
        {
            return s.getTextValueSuffix() == " dB" && s.getSliderStyle() == juce::Slider::LinearHorizontal
                   && juce::approximatelyEqual (s.getMinimum(), Cue::minGainDb) && juce::approximatelyEqual (s.getMaximum(), Cue::maxGainDb);
        });
    }

    /** The open page of the inspector after switching to 'index' (0 = 기본, 1 = 재생 for an audio cue). */
    juce::Component* openTab (Fixture& f, int index)
    {
        auto* tabs = child<juce::TabbedComponent> (f.inspector());
        expect (tabs != nullptr);
        if (tabs == nullptr)
            return nullptr;
        expectEquals (tabs->getTabNames()[index], index == 0 ? ko ("기본") : ko ("재생"));
        tabs->setCurrentTabIndex (index);
        dispatch();
        return tabs->getCurrentContentComponent();
    }

    static float scaleFor (double db) { return juce::Decibels::decibelsToGain ((float) db, (float) Cue::minGainDb); }

    void runTest() override
    {
        beginTest ("the 재생 tab's gain shows the cue's gain and reshapes the wave on the same tab at every drag step");
        {
            Fixture f;
            double nowMs = 1000.0;
            f.document().clock = [&nowMs] { return nowMs; };
            f.addSound ("a");
            f.document().cues.update (0, [] (Cue& c) { c.gainDb = -6.0; });   // not an undo step
            f.document().cues.setSelectedIndex (0);
            dispatch();

            auto* page = openTab (f, 1);
            auto* gain = page != nullptr ? gainSlider (*page) : nullptr;
            auto* wave = page != nullptr ? child<WaveformView> (*page) : nullptr;
            expect (gain != nullptr, "the 재생 tab has a gain slider");
            expect (wave != nullptr, "the 재생 tab shows the wave");
            if (gain == nullptr || wave == nullptr)
                return;

            expect (gain->isEnabled());
            expectWithinAbsoluteError (gain->getValue(), -6.0, 1e-9);
            expectWithinAbsoluteError (wave->getWaveScale (0), scaleFor (-6.0), 1e-5f);

            for (const double db : { -9.0, -14.5, -3.2, 4.0, 11.7 })   // one drag: a step every 16 ms, nothing dispatched in between
            {
                nowMs += 16.0;
                gain->setValue (db, juce::sendNotificationSync);   // the path a drag step takes
                expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, db, 1e-9);
                expectWithinAbsoluteError (wave->getWaveScale (0), scaleFor (db), scaleFor (db) * 1e-5f, "the wave follows this step");
                expectWithinAbsoluteError (wave->getWaveScale (1), scaleFor (db), scaleFor (db) * 1e-5f);
            }

            expectEquals (f.document().getUndoName(), ko ("게인 변경"));
            expect (f.document().undo(), "the drag is one undo step");
            dispatch();
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, -6.0, 1e-9);
            expect (! f.document().canUndo(), "nothing else was recorded");

            page = openTab (f, 1);
            gain = page != nullptr ? gainSlider (*page) : nullptr;
            wave = page != nullptr ? child<WaveformView> (*page) : nullptr;
            expect (gain != nullptr && wave != nullptr);
            if (gain == nullptr || wave == nullptr)
                return;
            expectWithinAbsoluteError (gain->getValue(), -6.0, 1e-9, "undo moves the slider back");
            expectWithinAbsoluteError (wave->getWaveScale (0), scaleFor (-6.0), 1e-5f, "and the wave");
        }

        beginTest ("the 기본 tab and the 재생 tab show and move one gain");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();

            auto* page = openTab (f, 1);
            auto* play = page != nullptr ? gainSlider (*page) : nullptr;
            expect (play != nullptr);
            if (play == nullptr)
                return;
            play->setValue (-18.3, juce::sendNotificationSync);

            page = openTab (f, 0);
            auto* basic = page != nullptr ? gainSlider (*page) : nullptr;
            expect (basic != nullptr && basic != play, "the 기본 tab has its own slider");
            if (basic == nullptr || basic == play)
                return;
            expectWithinAbsoluteError (basic->getValue(), -18.3, 1e-9, "the 기본 tab shows the 재생 tab's move");
            basic->setValue (7.5, juce::sendNotificationSync);

            page = openTab (f, 1);
            auto* wave = page != nullptr ? child<WaveformView> (*page) : nullptr;
            expect (wave != nullptr);
            expectWithinAbsoluteError (play->getValue(), 7.5, 1e-9, "the 재생 tab shows the 기본 tab's move");
            if (wave != nullptr)
                expectWithinAbsoluteError (wave->getWaveScale (0), scaleFor (7.5), scaleFor (7.5) * 1e-5f);
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, 7.5, 1e-9);
        }

        beginTest ("a playing cue follows the 재생 tab's gain at once");
        {
            Fixture f;
            const auto sound = f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            expect (f.engine.play (f.document().cues.get (0)));
            auto* page = openTab (f, 1);
            auto* gain = page != nullptr ? gainSlider (*page) : nullptr;
            expect (gain != nullptr);
            if (gain == nullptr)
                return;
            gain->setValue (-12.0, juce::sendNotificationSync);

            bool found = false;
            for (const auto& p : f.engine.getPlayingCues())
                if (p.id == sound.id)
                {
                    found = true;
                    expectWithinAbsoluteError (p.liveGainDb, -12.0, 1e-9);
                }
            expect (found, "the cue is playing");
        }

        beginTest ("show mode and an empty selection leave the 재생 tab's gain alone");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            auto* page = openTab (f, 1);
            auto* gain = page != nullptr ? gainSlider (*page) : nullptr;
            expect (gain != nullptr);
            if (gain == nullptr)
                return;

            expect (f.command (CommandIDs::toggleShowMode));
            dispatch();
            expect (! gain->isEnabled(), "show mode: no gain edits from the 재생 tab");
            expect (f.command (CommandIDs::toggleShowMode));
            dispatch();
            expect (gain->isEnabled());

            f.document().cues.setSelectedIndex (-1);
            dispatch();
            page = openTab (f, 1);
            gain = page != nullptr ? gainSlider (*page) : nullptr;
            expect (gain != nullptr);
            if (gain != nullptr)
                expect (! gain->isEnabled(), "no cue: nothing to set");
        }

        beginTest ("the gain sits in the 기본 tab's gain column at the same length; the envelope row fits that column, wide and narrow");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();

            for (const int width : { 1541, 1100 })   // the cluster layout and the wrapped one (under 1180 logical px)
            {
                f.main->setSize (width, 980);
                dispatch();
                auto* basicPage = openTab (f, 0);
                auto* basic = basicPage != nullptr ? gainSlider (*basicPage) : nullptr;
                const auto basicBounds = basic != nullptr ? basic->getBounds() : juce::Rectangle<int>();
                auto* playPage = openTab (f, 1);
                auto* play = playPage != nullptr ? gainSlider (*playPage) : nullptr;
                expect (basic != nullptr && play != nullptr);
                if (basic == nullptr || play == nullptr || play->getParentComponent() == nullptr)
                    continue;

                expectEquals (play->getX(), basicBounds.getX(), "same column as the 기본 tab's gain");
                expectEquals (play->getWidth(), basicBounds.getWidth(), "same slider length");
                auto& panel = *play->getParentComponent();
                const auto column = InspectorClusters (panel.getWidth(), 2).playback;
                expect (column.contains (play->getBounds()), "the gain stays in the middle column: " + play->getBounds().toString());

                const auto toggleFont = Palette::font (Palette::timeSize);
                int previousRight = column.getX();
                for (const char* text : { "사용", "직선 (끄면 곡선)", "시작/끝에 잠금" })
                {
                    auto* toggle = child<juce::ToggleButton> (panel, [text] (const juce::ToggleButton& t) { return t.getButtonText() == ko (text); });
                    expect (toggle != nullptr, "missing toggle: " + ko (text));
                    if (toggle == nullptr)
                        continue;
                    const auto b = toggle->getBounds();
                    expect (b.getY() == column.getY() && b.getX() >= previousRight && b.getRight() <= column.getRight(),
                            ko (text) + " sits on the column's first row, in order, inside it: " + b.toString());
                    expect ((float) b.getWidth() >= 23.0f + juce::GlyphArrangement::getStringWidth (toggleFont, ko (text)),
                            ko (text) + " shows its whole text");
                    previousRight = b.getRight();
                }
            }
        }
    }
};

static PlayTabGainTests playTabGainTests;
}
}
