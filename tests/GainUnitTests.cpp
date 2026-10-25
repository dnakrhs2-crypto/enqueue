#include "VolumeCueUiTestSupport.h"
#include "ui/GainControl.h"
#include "ui/TransportBar.h"
#include "ui/WaveformView.h"
#include "ui/UiUtils.h"

namespace gocue::tests
{
namespace
{
using namespace volume_ui;

/** The 기본 / 재생 tabs' gain in dB or in % (gom 2026-10-25): pressing the gain button switches the unit on both tabs,
    100 % = 0 dB and the % goes past 100 like the dB goes past 0, and a % edit moves the cue's gain and its wave exactly
    like a dB edit (only the shown unit changes). This PC keeps the unit. */
class GainUnitTests : public juce::UnitTest
{
public:
    GainUnitTests() : UnitTest ("Gain unit", "Enqueue") {}

    /** The open page of the inspector after switching to 'index' (0 = 기본, 1 = 재생 for an audio cue). */
    juce::Component* openTab (Fixture& f, int index)
    {
        auto* tabs = child<juce::TabbedComponent> (f.inspector());
        expect (tabs != nullptr);
        if (tabs == nullptr)
            return nullptr;
        tabs->setCurrentTabIndex (index);
        dispatch();
        return tabs->getCurrentContentComponent();
    }

    static juce::Label* textBox (juce::Slider& slider) { return child<juce::Label> (slider); }

    /** What typing into the slider's number box and pressing Enter does: the box's text change, then JUCE's own
        parse -> snap -> setValue. */
    static void type (juce::Slider& slider, const juce::String& text)
    {
        if (auto* box = textBox (slider))
            box->setText (text, juce::sendNotificationSync);
    }

    static juce::String shown (juce::Slider& slider)
    {
        auto* box = textBox (slider);
        return box != nullptr ? box->getText() : juce::String ("<no box>");
    }

    static float scaleFor (double db) { return juce::Decibels::decibelsToGain ((float) db, (float) Cue::minGainDb); }

    struct Controls { GainUnitButton* button = nullptr; GainSlider* slider = nullptr; WaveformView* wave = nullptr; };

    Controls controlsOn (Fixture& f, int tab)
    {
        Controls c;
        if (auto* page = openTab (f, tab))
        {
            c.button = child<GainUnitButton> (*page);
            c.slider = child<GainSlider> (*page);
            c.wave = child<WaveformView> (*page);
        }
        expect (c.button != nullptr, "the tab has the gain button");
        expect (c.slider != nullptr, "the tab has the gain slider");
        return c;
    }

    void press (GainUnitButton& button)
    {
        button.triggerClick();   // JUCE posts the click; the dispatch delivers it like a real one
        dispatch();
    }

    void runTest() override
    {
        beginTest ("percent is the level against 0 dB: 100 % = 0 dB, 50 % = -6 dB, 200 % = +6 dB, 0 % = silence");
        {
            expectWithinAbsoluteError (GainUnits::toPercent (0.0), 100.0, 1e-9);
            expectWithinAbsoluteError (GainUnits::toPercent (-6.0206), 50.0, 1e-3);
            expectWithinAbsoluteError (GainUnits::toPercent (6.0206), 200.0, 1e-2);
            expectWithinAbsoluteError (GainUnits::toPercent (Cue::maxGainDb), 1584.893, 1e-2);
            expectEquals (GainUnits::toPercent (Cue::minGainDb), 0.0, "the silence floor is 0 %");
            expectEquals (GainUnits::toPercent (-500.0), 0.0);
            expectWithinAbsoluteError (GainUnits::fromPercent (100.0), 0.0, 1e-9);
            expectWithinAbsoluteError (GainUnits::fromPercent (50.0), -6.0206, 1e-3);
            expectWithinAbsoluteError (GainUnits::fromPercent (200.0), 6.0206, 1e-3);
            expectEquals (GainUnits::fromPercent (0.0), Cue::minGainDb, "0 % is silence");
            expectEquals (GainUnits::fromPercent (-20.0), Cue::minGainDb);
            expectEquals (GainUnits::fromPercent (100000.0), Cue::maxGainDb, "past the top stops at the dB ceiling");
            for (const double db : { -60.0, -20.0, -6.0, -0.5, 0.0, 3.3, 12.0, 24.0 })
                expectWithinAbsoluteError (GainUnits::fromPercent (GainUnits::toPercent (db)), db, 1e-9);
            expectEquals (GainUnits::format (-6.0, false), juce::String ("-6.0 dB"));
            expectEquals (GainUnits::format (-6.0, true), juce::String ("50.1 %"));
            expectEquals (GainUnits::format (0.0, true), juce::String ("100.0 %"));
            expectEquals (GainUnits::format (Cue::maxGainDb, true), juce::String ("1584.9 %"));
            expectEquals (GainUnits::format (Cue::minGainDb, true), juce::String ("0.0 %"));
        }

        beginTest ("pressing the gain button switches the unit on the 기본 and the 재생 tab, and this PC keeps it");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.update (0, [] (Cue& c) { c.gainDb = -6.0; });   // not an undo step
            f.document().cues.setSelectedIndex (0);
            dispatch();
            expect (! f.settings.getGainInPercent(), "dB until someone switches");

            auto basic = controlsOn (f, 0);
            if (basic.button == nullptr || basic.slider == nullptr)
                return;
            expectEquals (basic.button->getButtonText(), ko ("게인 (dB)"));
            expectEquals (shown (*basic.slider), juce::String ("-6.0 dB"));
            expect (basic.button->isEnabled());

            press (*basic.button);
            expectEquals (basic.button->getButtonText(), ko ("게인 (%)"));
            expectEquals (shown (*basic.slider), juce::String ("50.1 %"));
            expect (f.settings.getGainInPercent(), "the unit is saved for this PC");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, -6.0, 1e-12, "switching the unit leaves the gain alone");
            expect (! f.document().canUndo(), "and is no undo step");

            auto play = controlsOn (f, 1);
            if (play.button == nullptr || play.slider == nullptr)
                return;
            expect (play.button != basic.button && play.slider != basic.slider, "each tab has its own controls");
            expectEquals (play.button->getButtonText(), ko ("게인 (%)"), "the 재생 tab follows the 기본 tab's switch");
            expectEquals (shown (*play.slider), juce::String ("50.1 %"));
            expectWithinAbsoluteError (play.slider->getValue(), -6.0, 1e-9, "the slider still holds the dB gain: the knob does not move");

            press (*play.button);
            expectEquals (play.button->getButtonText(), ko ("게인 (dB)"));
            expectEquals (shown (*play.slider), juce::String ("-6.0 dB"));
            expect (! f.settings.getGainInPercent());
            basic = controlsOn (f, 0);
            if (basic.button != nullptr && basic.slider != nullptr)
            {
                expectEquals (basic.button->getButtonText(), ko ("게인 (dB)"), "and the 기본 tab follows the 재생 tab's");
                expectEquals (shown (*basic.slider), juce::String ("-6.0 dB"));
            }
        }

        beginTest ("in % a typed level past 100 % raises the gain and reshapes the wave like the dB gain");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            auto play = controlsOn (f, 1);
            if (play.button == nullptr || play.slider == nullptr || play.wave == nullptr)
                return;
            press (*play.button);
            expectEquals (shown (*play.slider), juce::String ("100.0 %"));

            const auto check = [&] (const juce::String& typed, double wantedPercent, const juce::String& wantedText)
            {
                type (*play.slider, typed);
                const double db = f.document().cues.getSelected()->gainDb;
                expectWithinAbsoluteError (GainUnits::toPercent (db), wantedPercent, 1e-6, "typed " + typed);
                expectEquals (shown (*play.slider), wantedText, "typed " + typed);
                expectWithinAbsoluteError (play.wave->getWaveScale (0), scaleFor (db), scaleFor (db) * 1e-5f + 1e-9f, "the wave follows " + typed);
                expectWithinAbsoluteError (play.wave->getWaveScale (1), scaleFor (db), scaleFor (db) * 1e-5f + 1e-9f);
            };
            check ("150", 150.0, "150.0 %");
            expectWithinAbsoluteError (play.wave->getWaveScale (0), 1.5f, 1e-5f, "150 % draws the wave at 1.5 times");
            check ("250 %", 250.0, "250.0 %");
            check ("1000", 1000.0, "1000.0 %");
            check ("70%", 70.0, "70.0 %");
            check ("33.33", 33.3, "33.3 %");   // the box shows what the cue holds: 0.1 % steps
            check ("99999", GainUnits::maxPercent(), "1584.9 %");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, Cue::maxGainDb, 1e-9, "the top is the dB ceiling");
            check ("-6", GainUnits::toPercent (GainUnits::fromPercent (50.1)), "50.1 %");   // a % cannot be negative: that is dB
            check ("-6 dB", GainUnits::toPercent (GainUnits::fromPercent (50.1)), "50.1 %");
            check ("0", 0.0, "0.0 %");
            expectEquals (f.document().cues.getSelected()->gainDb, Cue::minGainDb, "0 % is the silence floor");
            check ("80", 80.0, "80.0 %");
            check ("abc", 80.0, "80.0 %");   // not a number: the gain stays
            check ("", 80.0, "80.0 %");

            // a drag step goes through snapValue first (JUCE's drag path): a 0.1 % grid, the wave at every step
            for (const double db : { -3.0, 1.7, 5.25, 9.9 })
            {
                play.slider->setValue (play.slider->snapValue (db, juce::Slider::absoluteDrag), juce::sendNotificationSync);
                const double held = f.document().cues.getSelected()->gainDb;
                const double percent = GainUnits::toPercent (held);
                expectWithinAbsoluteError (percent, std::round (GainUnits::toPercent (db) * 10.0) / 10.0, 1e-6, "a 0.1 % step");
                expectWithinAbsoluteError (play.wave->getWaveScale (0), scaleFor (held), scaleFor (held) * 1e-5f, "the wave follows the drag step");
                expectEquals (shown (*play.slider), GainUnits::format (held, true));
            }

            play.slider->setValue (play.slider->getDoubleClickReturnValue(), juce::sendNotificationSync);
            expectEquals (shown (*play.slider), juce::String ("100.0 %"), "double-click is 0 dB = 100 %");
            expectEquals (f.document().getUndoName(), ko ("게인 변경"));
        }

        beginTest ("a % drag on the 기본 tab is one undo step and reaches a playing cue at once");
        {
            Fixture f;
            const auto sound = f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            expect (f.engine.play (f.document().cues.get (0)));
            auto basic = controlsOn (f, 0);
            if (basic.button == nullptr || basic.slider == nullptr)
                return;
            press (*basic.button);
            for (const double db : { -2.0, -4.0, 3.0, 6.0 })
                basic.slider->setValue (basic.slider->snapValue (db, juce::Slider::absoluteDrag), juce::sendNotificationSync);
            const double held = f.document().cues.getSelected()->gainDb;
            expectWithinAbsoluteError (GainUnits::toPercent (held), 199.5, 1e-6);   // +6 dB = 199.53 %, on the 0.1 % grid

            bool found = false;
            for (const auto& p : f.engine.getPlayingCues())
                if (p.id == sound.id)
                {
                    found = true;
                    expectWithinAbsoluteError (p.liveGainDb, held, 1e-9, "the playing cue follows the % drag");
                }
            expect (found);
            expectEquals (f.document().getUndoName(), ko ("게인 변경"));
            expect (f.document().undo(), "the drag is one undo step");
            dispatch();
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, 0.0, 1e-12);
            expect (! f.document().canUndo());
            expectEquals (shown (*basic.slider), juce::String ("100.0 %"), "undo shows in %");
        }

        beginTest ("a wheel notch always moves the gain a step: up off silence and down to it in %, a touchpad's small step in dB");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            auto basic = controlsOn (f, 0);
            if (basic.button == nullptr || basic.slider == nullptr)
                return;
            auto& slider = *basic.slider;
            juce::int64 ms = 1000000;
            const auto wheel = [&] (float deltaY)
            {
                ms += 100;   // a new event time each notch (JUCE drops a repeat with the same time)
                const auto at = slider.getLocalBounds().getCentre().toFloat();
                const juce::Time time (ms);
                const juce::MouseEvent event (juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                              &slider, &slider, time, at, time, 1, false);
                juce::MouseWheelDetails details {};
                details.deltaY = deltaY;
                slider.mouseWheelMove (event, details);
            };
            const float notch = 60.0f / 256.0f;   // one notch of a mouse wheel on Windows (juce_Windowing_windows.cpp)
            const auto gain = [&] { return f.document().cues.getSelected()->gainDb; };

            press (*basic.button);
            type (slider, "0");
            expectEquals (gain(), Cue::minGainDb);
            wheel (notch);
            expectWithinAbsoluteError (GainUnits::toPercent (gain()), 0.1, 1e-6, "one notch up from silence");
            expectEquals (shown (slider), juce::String ("0.1 %"));
            wheel (notch);
            expectGreaterThan (GainUnits::toPercent (gain()), 0.1 + 1e-6, "and on up");
            type (slider, "0.1");
            wheel (-notch);
            expectEquals (gain(), Cue::minGainDb, "one notch down from 0.1 % is silence");
            expectEquals (shown (slider), juce::String ("0.0 %"));
            for (int i = 0; i < 40; ++i)
                wheel (notch);
            expectGreaterThan (GainUnits::toPercent (gain()), 100.0, "the wheel goes past 100 %");

            press (*basic.button);   // back to dB
            type (slider, "-6");
            wheel (0.001f);   // a touchpad's small step: 0.02 dB, under half a 0.1 dB step
            expectWithinAbsoluteError (gain(), -5.9, 1e-9, "still one 0.1 dB step up, as the 0.1 dB interval gave");
            wheel (-0.001f);
            wheel (-0.001f);
            expectWithinAbsoluteError (gain(), -6.1, 1e-9, "and down");
            wheel (notch);
            expectGreaterThan (gain(), -5.0, "a mouse notch is the usual bigger step");
            expectWithinAbsoluteError (gain(), std::round (gain() * 10.0) / 10.0, 1e-9, "on the 0.1 dB grid");
        }

        beginTest ("typed in %: a signed number is dB, a thousands comma reads as one, any other comma keeps the gain");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            auto basic = controlsOn (f, 0);
            if (basic.button == nullptr || basic.slider == nullptr)
                return;
            auto& slider = *basic.slider;
            const auto percent = [&] { return GainUnits::toPercent (f.document().cues.getSelected()->gainDb); };
            press (*basic.button);

            type (slider, "+6");
            expectWithinAbsoluteError (percent(), 199.5, 1e-6, "+6 in % is +6 dB, like -6 is -6 dB");
            type (slider, "-6");
            expectWithinAbsoluteError (percent(), 50.1, 1e-6);
            type (slider, "+6%");
            expectWithinAbsoluteError (percent(), 6.0, 1e-6, "with the % typed it is 6 %");
            type (slider, "1,200");
            expectWithinAbsoluteError (percent(), 1200.0, 1e-6, "a thousands comma");
            type (slider, "1,000 %");
            expectWithinAbsoluteError (percent(), 1000.0, 1e-6);
            type (slider, "1,5");
            expectWithinAbsoluteError (percent(), 1000.0, 1e-6, "a comma that is no thousands separator keeps the gain");
            type (slider, "12,50");
            expectWithinAbsoluteError (percent(), 1000.0, 1e-6);
            expectEquals (shown (slider), juce::String ("1000.0 %"));

            press (*basic.button);   // dB
            type (slider, "-6,5");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, 20.0, 1e-9, "dB: the same comma rule, the gain stays");
            type (slider, "-3");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, -3.0, 1e-9);
        }

        beginTest ("show mode locks the unit on both tabs, as it locks this PC's other settings");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            expect (f.command (CommandIDs::toggleShowMode));
            dispatch();
            for (const int tab : { 0, 1 })
            {
                auto c = controlsOn (f, tab);
                if (c.button == nullptr)
                    continue;
                expect (! c.button->isEnabled(), "show mode: the unit stays");
                press (*c.button);
                expect (! f.settings.getGainInPercent(), "a press does nothing in show mode");
                expectEquals (c.button->getButtonText(), ko ("게인 (dB)"));
            }
            expect (f.command (CommandIDs::toggleShowMode));
            dispatch();
            auto basic = controlsOn (f, 0);
            if (basic.button != nullptr)
            {
                expect (basic.button->isEnabled(), "edit mode: it switches again");
                press (*basic.button);
                expect (f.settings.getGainInPercent());
            }
        }

        beginTest ("in dB nothing changes: 0.1 dB steps, a typed % still lands, a typo keeps the gain");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            auto basic = controlsOn (f, 0);
            if (basic.slider == nullptr)
                return;
            expectEquals (basic.slider->getTextValueSuffix(), juce::String (" dB"));
            basic.slider->setValue (basic.slider->snapValue (-6.04, juce::Slider::absoluteDrag), juce::sendNotificationSync);
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, -6.0, 1e-9, "0.1 dB steps as before");
            expectEquals (shown (*basic.slider), juce::String ("-6.0 dB"));
            type (*basic.slider, "-12.5");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, -12.5, 1e-9);
            type (*basic.slider, "+3");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, 3.0, 1e-9);
            type (*basic.slider, "50 %");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, -6.0, 1e-9, "50 % = -6.0 dB on the 0.1 dB grid");
            type (*basic.slider, "abc");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, -6.0, 1e-9, "a typo keeps the gain");
            expectEquals (shown (*basic.slider), juce::String ("-6.0 dB"));
            type (*basic.slider, "99");
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, Cue::maxGainDb, 1e-9, "the dB range is unchanged");
        }

        beginTest ("a % value, back in dB, shows on the 0.1 dB grid without changing the cue");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            auto basic = controlsOn (f, 0);
            if (basic.button == nullptr || basic.slider == nullptr)
                return;
            press (*basic.button);
            type (*basic.slider, "33.3");
            const double held = f.document().cues.getSelected()->gainDb;
            press (*basic.button);
            expectEquals (shown (*basic.slider), juce::String ("-9.6 dB"));
            expectEquals (f.document().cues.getSelected()->gainDb, held, "the switch does not write the gain");
            press (*basic.button);
            expectEquals (shown (*basic.slider), juce::String ("33.3 %"), "and back in % the exact value shows again");
        }

        beginTest ("this PC's saved unit is there from the start, on both tabs");
        {
            Fixture f;
            f.main.reset();
            dispatch();
            f.settings.setGainInPercent (true);
            f.main = std::make_unique<HiddenMain> (f.engine, f.settings, f.commands);
            f.main->setLookAndFeel (&f.theme);
            f.main->setSize (1541, 980);
            f.document().settings.autoBackup = false;   // as the Fixture sets its own
            f.document().settings.backupBeforeSave = false;
            f.document().settings.copyFilesIntoProject = false;
            f.document().settings.autoLoadNewCues = false;
            f.addSound ("a");
            f.document().cues.update (0, [] (Cue& c) { c.gainDb = 6.0; });
            f.document().cues.setSelectedIndex (0);
            dispatch();
            for (const int tab : { 0, 1 })
            {
                auto c = controlsOn (f, tab);
                if (c.button == nullptr || c.slider == nullptr)
                    continue;
                expectEquals (c.button->getButtonText(), ko ("게인 (%)"));
                expectEquals (shown (*c.slider), juce::String ("199.5 %"));
                expectEquals (c.slider->getTextValueSuffix(), juce::String (" %"));
            }
        }

        beginTest ("the next cue box above (다음 큐) shows the gain in the unit the gain button picks, audio and mic cues");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.update (0, [] (Cue& c) { c.gainDb = -6.0; });
            Cue mic;
            mic.name = "mic";
            mic.type = CueType::mic;
            mic.mic.numInputs = 2;
            mic.gainDb = 6.0;
            f.document().cues.add (mic);
            f.document().cues.setSelectedIndex (0);   // the playhead follows the selection: the standby cue
            dispatch();
            auto* transport = child<TransportBar> (*f.main);
            expect (transport != nullptr);
            if (transport == nullptr)
                return;
            const auto meta = [transport]
            {
                auto* label = child<juce::Label> (*transport, [] (const juce::Label& l) { return l.getText().contains (ko ("정지 페이드")); });
                return label != nullptr ? label->getText() : juce::String ("<no meta line>");
            };
            expect (meta().contains (ko ("게인 -6.0 dB")), meta());

            auto basic = controlsOn (f, 0);
            if (basic.button == nullptr || basic.slider == nullptr)
                return;
            press (*basic.button);
            expect (meta().contains (ko ("게인 50.1 %")), "the box follows the switch at once: " + meta());
            type (*basic.slider, "150");
            expect (meta().contains (ko ("게인 150.0 %")), "and a % edit: " + meta());

            f.document().cues.setSelectedIndex (1);
            dispatch();
            expect (meta().startsWith (ko ("마이크 큐")) && meta().contains (ko ("게인 199.5 %")), "a mic cue's line too: " + meta());

            basic = controlsOn (f, 0);
            if (basic.button != nullptr)
                press (*basic.button);
            expect (meta().contains (ko ("게인 6.0 dB")), "back in dB as before: " + meta());
        }

        beginTest ("no cue selected: the slider is off, the unit can still be switched");
        {
            Fixture f;
            dispatch();
            auto basic = controlsOn (f, 0);
            if (basic.button == nullptr || basic.slider == nullptr)
                return;
            expect (! basic.slider->isEnabled());
            expect (basic.button->isEnabled());
            press (*basic.button);
            expect (f.settings.getGainInPercent());
            expectEquals (shown (*basic.slider), juce::String ("100.0 %"));
        }
    }
};

static GainUnitTests gainUnitTests;
}
}
