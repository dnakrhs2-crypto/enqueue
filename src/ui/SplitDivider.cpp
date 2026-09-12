#include "ui/SplitDivider.h"

#include "ui/UiUtils.h"

namespace gocue
{

SplitDivider::SplitDivider (Orientation o)
    : orientation (o)
{
    setMouseCursor (orientation == Orientation::horizontal ? juce::MouseCursor::UpDownResizeCursor
                                                           : juce::MouseCursor::LeftRightResizeCursor);
    toggle.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    toggle.setWantsKeyboardFocus (false);
    toggle.onClick = [this] { if (onToggle) onToggle(); };
    addAndMakeVisible (toggle);
    updateShape();
}

void SplitDivider::setCollapsed (bool shouldBeCollapsed)
{
    if (collapsed == shouldBeCollapsed)
        return;

    collapsed = shouldBeCollapsed;
    updateShape();
    repaint();
}

void SplitDivider::updateShape()
{
    // a chevron pointing where the pane goes: down / right folds it away, up / left brings it back
    juce::Path p;
    const bool away = ! collapsed;

    const float half = Palette::dividerChevronSize * 0.5f;
    const float direction = away ? 1.0f : -1.0f;
    p.startNewSubPath (-half, -half * 0.5f * direction);
    p.lineTo (0.0f, half * 0.5f * direction);
    p.lineTo (half, -half * 0.5f * direction);
    if (orientation == Orientation::vertical)
        p.applyTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi));
    toggle.shape = p;
    toggle.repaint();
    toggle.setTooltip (collapsed ? ko ("펴기") : ko ("접기 (구분선 더블클릭도 됩니다)"));
}

void SplitDivider::resized()
{
    const auto b = getLocalBounds();

    if (orientation == Orientation::horizontal)
        toggle.setBounds (b.withSizeKeepingCentre (36, thickness));
    else
        toggle.setBounds (b.withSizeKeepingCentre (thickness, 36));
}

void SplitDivider::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);

    const auto b = getLocalBounds().toFloat();
    const auto c = b.getCentre();
    g.setColour (Palette::muted.withAlpha (Palette::rowBorderAlpha));

    for (const float off : { -36.0f, -30.0f, -24.0f })
    {
        const float x = orientation == Orientation::horizontal ? c.x + off : c.x;
        const float y = orientation == Orientation::horizontal ? c.y : c.y + off;
        g.fillEllipse (x - 1.0f, y - 1.0f, 2.0f, 2.0f);
    }
}

void SplitDivider::mouseDown (const juce::MouseEvent& e)
{
    if (collapsed)
        return;   // nothing to resize: the chevron / a double-click brings the pane back

    dragging = true;
    repaint();
    dragStart = orientation == Orientation::horizontal ? e.getScreenY() : e.getScreenX();

    if (onDragStart)
        onDragStart();
}

void SplitDivider::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const int now = orientation == Orientation::horizontal ? e.getScreenY() : e.getScreenX();

    if (onDrag)
        onDrag (now - dragStart);
}

void SplitDivider::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    repaint();

    if (onDragEnd)
        onDragEnd();
}

void SplitDivider::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onToggle)
        onToggle();
}

} // namespace gocue
