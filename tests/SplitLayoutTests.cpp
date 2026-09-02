#include "ui/SplitLayout.h"

#include <juce_core/juce_core.h>

#include <limits>

namespace gocue
{

class SplitLayoutTests : public juce::UnitTest
{
public:
    SplitLayoutTests() : juce::UnitTest ("SplitLayout", "GoCue") {}

    void runTest() override
    {
        using namespace SplitLayout;

        beginTest ("the inspector keeps its share of the height across window sizes");
        {
            expectEquals (inspectorHeight (800, 0.45, false, 10), 360);
            expectEquals (inspectorHeight (1000, 0.45, false, 10), 450);   // a taller window: same proportion
            expectEquals (inspectorHeight (800, 0.45, true, 10), 0);       // folded away
        }

        beginTest ("both panes stay usable at the extremes");
        {
            expectEquals (inspectorHeight (800, 0.0, false, 10), minInspectorHeight);
            expectEquals (inspectorHeight (800, 1.0, false, 10), 800 - 10 - minTableHeight);
            expectEquals (inspectorHeight (300, 0.45, false, 10), 300 - 10 - minTableHeight);   // no room for the minimum: the table keeps its own
            expectEquals (inspectorHeight (100, 0.45, false, 10), 0);
            expectEquals (inspectorHeight (0, 0.45, false, 10), 0);
            expectEquals (inspectorHeight (800, std::numeric_limits<double>::quiet_NaN(), false, 10), 400);   // garbage in the settings: half
        }

        beginTest ("a drag result round-trips through the fraction");
        {
            const double f = fractionFor (333, 800, 0.45);
            expectEquals (inspectorHeight (800, f, false, 10), 333);
            expectWithinAbsoluteError (fractionFor (10, 0, 0.45), 0.45, 1e-9);   // no area yet: keep what we had
            expectWithinAbsoluteError (fractionFor (-50, 800, 0.45), 0.0, 1e-9);
        }

        beginTest ("active cues panel width");
        {
            expectEquals (activeCuesWidth (1200, 0.24, false, 10), 288);
            expectEquals (activeCuesWidth (1200, 0.24, true, 10), 0);
            expectEquals (activeCuesWidth (1200, 0.05, false, 10), minActiveCuesWidth);
            expectEquals (activeCuesWidth (1200, 0.9, false, 10), 1200 - 10 - minTableWidth);
        }
    }
};

static SplitLayoutTests splitLayoutTests;

} // namespace gocue
