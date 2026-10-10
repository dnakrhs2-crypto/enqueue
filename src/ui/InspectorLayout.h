#pragma once

#include "ui/UiUtils.h"

namespace gocue
{
/** Scroll only when the fields need it. Keep the existing 6px gutter at the right,
    but reserve a bottom gutter only when a horizontal scrollbar is necessary. */
class InspectorPage : public juce::Viewport
{
public:
    InspectorPage (juce::Component& c, int height, int width) : content (c), minHeight (height), minWidth (width)
    {
        setWantsKeyboardFocus (false);
        setScrollBarsShown (true, true);
        setScrollOnDragMode (ScrollOnDragMode::never);
        setViewedComponent (&content, false);
    }

    void setMinimumHeight (int height)
    {
        if (minHeight == height) return;
        minHeight = height;
        sizeContent();
    }

    void resized() override
    {
        if (sizing) return;
        const juce::ScopedValueSetter<bool> guard (sizing, true);
        juce::Viewport::resized();
        sizeContent();
    }

private:
    void sizeContent()
    {
        const int width = juce::jmax (minWidth, getWidth() - getScrollBarThickness());
        const int bottom = width > getWidth() ? getScrollBarThickness() : 0;
        content.setSize (width, juce::jmax (minHeight, getHeight() - bottom));
    }

    juce::Component& content;
    int minHeight;
    const int minWidth;
    bool sizing = false;
};

/** Shared basic/play columns: fixed 400px and 288px groups, flexible cue group.
    At narrow logical widths the latter two groups move below the cue group. */
struct InspectorClusters
{
    explicit InspectorClusters (int width, int rows)
    {
        const int height = rows * 38 - 8;
        wrapped = width < 1180;
        if (wrapped)
        {
            cue = { 12, 8, width - 24, height };
            playback = { 12, 8 + rows * 38, 400, height };
            trigger = { 445, playback.getY(), 288, height };
            rule2 = { 428, playback.getY(), 1, height };
            bottom = 8 + 2 * rows * 38;
        }
        else
        {
            const int cueWidth = width - 24 - 400 - 288 - 66;
            cue = { 12, 8, cueWidth, height };
            rule1 = { cue.getRight() + 16, 8, 1, height };
            playback = { rule1.getRight() + 16, 8, 400, height };
            rule2 = { playback.getRight() + 16, 8, 1, height };
            trigger = { rule2.getRight() + 16, 8, 288, height };
            bottom = 8 + rows * 38;
        }
    }

    void paint (juce::Graphics& g) const
    {
        g.setColour (Palette::outline);
        g.fillRect (rule1);
        g.fillRect (rule2);
    }

    juce::Rectangle<int> cue, playback, trigger, rule1, rule2;
    int bottom = 0;
    bool wrapped = false;
};
}
