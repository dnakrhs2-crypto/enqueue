#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{

/** A cue's gain as a share of its level (gom 2026-10-25): 100 % = 0 dB, 50 % = -6.0 dB, 200 % = +6.0 dB, 0 % = the
    silence floor (Cue::minGainDb), and the top is Cue::maxGainDb (1584.9 %). The cue keeps its gainDb whichever unit is
    shown, so the sound, the wave and the slider's knob are the same in both. */
namespace GainUnits
{
    double toPercent (double db) noexcept;
    /** Clamped to [Cue::minGainDb, Cue::maxGainDb]; 0 % or less is the silence floor. */
    double fromPercent (double percent) noexcept;
    double maxPercent() noexcept;
    /** "-6.0 dB" / "50.1 %": what the number box shows. */
    juce::String format (double db, bool percent);
}

/** Calls back when a juce::Value changes (keeps the slider's own valueChanged() out of the way). */
struct ValueFollower : juce::Value::Listener
{
    std::function<void()> onChange;
    void valueChanged (juce::Value&) override { if (onChange) onChange(); }
};

/** The 기본 and 재생 tabs' gain slider on Cue::gainDb: the same range, box and double-click reset on both tabs, shown and
    typed in the unit of the shared value it is bound to (true = %). A drag, a wheel step or a typed value lands on
    0.1 dB, or on 0.1 % in %, so the box shows what the cue holds; the knob stays where it is when the unit switches. */
class GainSlider : public juce::Slider
{
public:
    GainSlider();
    ~GainSlider() override;

    void bindUnit (juce::Value& percent);
    bool showsPercent() const;

    juce::String getTextFromValue (double value) override;
    /** A number in the shown unit; "%" or "dB" after it picks that unit, a negative number in % is dB (a share is never
        negative), and text that is not a number keeps the gain. */
    double getValueFromText (const juce::String& text) override;
    double snapValue (double attemptedValue, DragMode) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void applyUnit();
    /** 'db' on the shown unit's step: 0.1 dB, or 0.1 % in %. */
    double onGrid (double db) const;

    juce::Value unit;
    ValueFollower follower;
    bool wheeling = false;    // inside a wheel notch: snapValue moves at least one step from wheelFrom
    double wheelFrom = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainSlider)
};

/** "게인 (dB)" / "게인 (%)" where the 기본 and 재생 tabs name their gain: a press switches the unit of every gain slider
    bound to the same value. A view setting of this PC, not an edit of the cue: no undo step. */
class GainUnitButton : public juce::Button
{
public:
    GainUnitButton();
    ~GainUnitButton() override;

    void bindUnit (juce::Value& percent);

    void paintButton (juce::Graphics& g, bool over, bool down) override;
    void clicked() override;

private:
    void applyUnit();

    juce::Value unit;
    ValueFollower follower;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainUnitButton)
};

} // namespace gocue
