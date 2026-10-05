#include "VolumeCueUiTestSupport.h"
#include "ui/WaveformView.h"
#include "ui/UiUtils.h"

namespace gocue::tests
{
namespace
{
using namespace volume_ui;

/** The wave follows the cue's level: turned up it draws taller, turned down flatter (the main level and the file
    channel's input level), on top of the view's own vertical zoom. */
class WaveHeightTests : public juce::UnitTest
{
public:
    WaveHeightTests() : UnitTest ("Wave height follows level", "Enqueue") {}

    void runTest() override
    {
        beginTest ("the drawn scale is the vertical zoom times the main level and the channel's input level");
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            juce::AudioThumbnailCache cache (4);
            WaveformView view (formats, cache);
            Cue cue;
            cue.numChannels = 2;
            cue.levels.resize (2, 2);
            cue.levels.setDefaults();

            view.setCue (&cue);
            expectWithinAbsoluteError (view.getWaveScale (0), 1.0f, 1e-4f);
            cue.gainDb = -6.0206;
            view.setCue (&cue);
            expectWithinAbsoluteError (view.getWaveScale (0), 0.5f, 1e-3f);
            expectWithinAbsoluteError (view.getWaveScale (1), 0.5f, 1e-3f);
            cue.gainDb = 6.0206;
            view.setCue (&cue);
            expectWithinAbsoluteError (view.getWaveScale (0), 2.0f, 1e-3f);
            cue.levels.inputDb[1] = -6.0206;
            view.setCue (&cue);
            expectWithinAbsoluteError (view.getWaveScale (1), 1.0f, 1e-3f);   // +6 dB main, -6 dB on that input
            expectWithinAbsoluteError (view.getWaveScale (0), 2.0f, 1e-3f);
            view.zoomVertical (2.0f);
            expectWithinAbsoluteError (view.getWaveScale (0), 4.0f, 1e-3f);
            cue.gainDb = Cue::minGainDb;
            view.setCue (&cue);
            expectEquals (view.getWaveScale (0), 0.0f, "a silent cue draws a flat line");
            view.setCue (nullptr);
            expectWithinAbsoluteError (view.getWaveScale (0), 2.0f, 1e-4f);   // no cue: the zoom alone
        }

        beginTest ("moving the gain in the inspector reshapes the wave right away");
        {
            Fixture f;
            f.addSound ("a");
            f.document().cues.setSelectedIndex (0);
            dispatch();
            // the inspector keeps only the open tab's page in the tree: the gain is on 기본, the wave on 재생
            auto* tabs = child<juce::TabbedComponent> (f.inspector());
            expect (tabs != nullptr);
            if (tabs == nullptr)
                return;
            expectEquals (tabs->getTabNames()[0], ko ("기본"));
            expectEquals (tabs->getTabNames()[1], ko ("재생"));

            const auto waveScale = [&]
            {
                tabs->setCurrentTabIndex (1);
                dispatch();
                auto* wave = child<WaveformView> (f.inspector());
                expect (wave != nullptr, "the 재생 tab shows the wave");
                const float scale = wave != nullptr ? wave->getWaveScale (0) : -1.0f;
                tabs->setCurrentTabIndex (0);
                dispatch();
                return scale;
            };
            const auto setGain = [&] (double db)
            {
                auto* gain = child<juce::Slider> (f.inspector(), [] (const juce::Slider& s)
                {
                    return s.getTextValueSuffix() == " dB" && s.getSliderStyle() == juce::Slider::LinearHorizontal
                           && juce::approximatelyEqual (s.getMinimum(), Cue::minGainDb) && juce::approximatelyEqual (s.getMaximum(), Cue::maxGainDb);
                });
                expect (gain != nullptr, "the 기본 tab's gain slider");
                if (gain != nullptr)
                    gain->setValue (db, juce::sendNotificationSync);   // the same path as a drag step
                dispatch();
            };

            const float before = waveScale();
            expectGreaterThan (before, 0.0f);
            setGain (-12.0);
            expectWithinAbsoluteError (f.document().cues.getSelected()->gainDb, -12.0, 1e-9);
            expectWithinAbsoluteError (waveScale(), before * juce::Decibels::decibelsToGain (-12.0f), 1e-4f);
            setGain (6.0);
            expectWithinAbsoluteError (waveScale(), before * juce::Decibels::decibelsToGain (6.0f), 1e-3f);
            f.document().undo();
            dispatch();
            expectWithinAbsoluteError (waveScale(), before * juce::Decibels::decibelsToGain ((float) f.document().cues.getSelected()->gainDb), 1e-4f);   // undo moves the wave with the level
        }
    }
};

static WaveHeightTests waveHeightTests;
}
}
