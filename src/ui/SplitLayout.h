#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::SplitLayout
{
    constexpr int minInspectorHeight = 220;
    constexpr int minTableHeight = 140;
    constexpr int minActiveCuesWidth = 260;
    constexpr int minTableWidth = 420;
    constexpr double defaultInspectorFraction = 0.45;
    constexpr double defaultActiveCuesFraction = 0.27;

    /** Size of a secondary pane (inspector below, active cues to the right): its share of the split area,
        clamped so both panes stay usable. 0 when the pane is folded away. */
    inline int paneSize (int splitSize, double fraction, bool collapsed, int dividerThickness, int minPane, int minOther)
    {
        if (collapsed || splitSize <= 0)
            return 0;

        const double share = std::isfinite (fraction) ? juce::jlimit (0.0, 1.0, fraction) : 0.5;   // garbage in the settings file: half
        const int largest = juce::jmax (0, splitSize - dividerThickness - minOther);
        const int wanted = juce::roundToInt (share * (double) splitSize);
        return juce::jlimit (juce::jmin (minPane, largest), largest, wanted);
    }

    inline int inspectorHeight (int splitHeight, double fraction, bool collapsed, int dividerThickness)
    {
        return paneSize (splitHeight, fraction, collapsed, dividerThickness, minInspectorHeight, minTableHeight);
    }

    inline int activeCuesWidth (int splitWidth, double fraction, bool collapsed, int dividerThickness)
    {
        return paneSize (splitWidth, fraction, collapsed, dividerThickness, minActiveCuesWidth, minTableWidth);
    }

    /** The share that reproduces 'size' of 'total' (a drag result), kept in 0..1; 'fallback' when there is no area yet. */
    inline double fractionFor (int size, int total, double fallback)
    {
        return total > 0 ? juce::jlimit (0.0, 1.0, (double) size / (double) total) : fallback;
    }
}
