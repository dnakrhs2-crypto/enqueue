#pragma once

#include "model/FadeCurve.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{

/** Editor for a FadeCurve: shape / intensity / domain / mirror controls and a canvas that draws the curve.
    For the custom shape the canvas takes points: click = add, drag = move, double-click or Delete = remove. */
class CurveEditor : public juce::Component
{
public:
    CurveEditor();
    ~CurveEditor() override;

    void setCurve (const FadeCurve& curve);
    const FadeCurve& getCurve() const noexcept { return curve; }
    void setEditable (bool editable);

    /** Every change; 'finished' is false while a point is being dragged. */
    std::function<void (const FadeCurve& curve, bool finished)> onChange;

    void resized() override;

private:
    class Canvas : public juce::Component
    {
    public:
        explicit Canvas (CurveEditor& o) : owner (o) { setWantsKeyboardFocus (true); }
        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        void mouseDoubleClick (const juce::MouseEvent& e) override;
        bool keyPressed (const juce::KeyPress& key) override;

    private:
        juce::Point<float> toScreen (double x, double y) const;
        juce::Point<double> toCurve (juce::Point<float> p) const;
        int hitPoint (juce::Point<float> p) const;

        CurveEditor& owner;
        int selected = -1;
        bool dragging = false;
        bool changed = false;   // something moved / was added during this gesture
        int masterIndex (int index) const;
    };

    void updateControls();
    void notify (bool finished);

    FadeCurve curve;
    bool editable = true;
    bool refreshing = false;

    juce::Label shapeLabel, intensityLabel, domainLabel, hint;
    juce::ComboBox shapeBox, domainBox;
    juce::Slider intensitySlider;
    juce::ToggleButton mirrorToggle;
    juce::TextButton resetButton;
    Canvas canvas { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveEditor)
};

} // namespace gocue
