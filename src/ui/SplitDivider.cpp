#include "ui/SplitDivider.h"

#include "ui/UiUtils.h"

namespace gocue
{

SplitDivider::SplitDivider (Orientation o)
    : orientation (o), toggle ("fold", Palette::dimText, Palette::text, Palette::text)
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

    if (orientation == Orientation::horizontal)
        away ? p.addTriangle (0.0f, 0.0f, 10.0f, 0.0f, 5.0f, 5.0f) : p.addTriangle (0.0f, 5.0f, 10.0f, 5.0f, 5.0f, 0.0f);
    else
        away ? p.addTriangle (0.0f, 0.0f, 5.0f, 5.0f, 0.0f, 10.0f) : p.addTriangle (5.0f, 0.0f, 5.0f, 10.0f, 0.0f, 5.0f);

    toggle.setShape (p, false, true, false);
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
    g.fillAll (Palette::panel);

    const auto b = getLocalBounds().toFloat();
    g.setColour (Palette::outline);

    if (orientation == Orientation::horizontal)
    {
        g.drawLine (0.0f, 0.5f, b.getWidth(), 0.5f);
        g.drawLine (0.0f, b.getHeight() - 0.5f, b.getWidth(), b.getHeight() - 0.5f);
    }
    else
    {
        g.drawLine (0.5f, 0.0f, 0.5f, b.getHeight());
        g.drawLine (b.getWidth() - 0.5f, 0.0f, b.getWidth() - 0.5f, b.getHeight());
    }

    // grip dots either side of the chevron
    g.setColour (Palette::dimText.withAlpha (0.6f));
    const auto c = b.getCentre();

    for (const float off : { -40.0f, -34.0f, -28.0f, 28.0f, 34.0f, 40.0f })
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

    if (onDragEnd)
        onDragEnd();
}

void SplitDivider::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onToggle)
        onToggle();
}

} // namespace gocue
