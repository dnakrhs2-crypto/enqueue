#include "app/UiScale.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

/** 프로젝트 설정 > 일반 "글씨·화면 크기" (0.10.2): the chosen percent is lowered to what the display can still fit. */
class UiScaleTests : public juce::UnitTest
{
public:
    UiScaleTests() : juce::UnitTest ("UI scale (글씨·화면 크기)", "Enqueue") {}

    void runTest() override
    {
        beginTest ("only the offered percentages are accepted; anything else reads as 100");
        {
            expect (UiScale::isAllowed (100) && UiScale::isAllowed (110) && UiScale::isAllowed (125) && UiScale::isAllowed (150));
            expect (! UiScale::isAllowed (0) && ! UiScale::isAllowed (90) && ! UiScale::isAllowed (137) && ! UiScale::isAllowed (200));
            expectEquals (UiScale::normalise (125), 125);
            expectEquals (UiScale::normalise (137), 100);
            expectEquals (UiScale::normalise (-5), 100);
        }

        beginTest ("a 1080p display at 100% fits every choice, and never more than what was asked for");
        {
            const juce::Rectangle<int> fullHd (1920, 1032);   // 1080 minus the taskbar
            expectEquals (UiScale::fitPercent (150, fullHd), 150);
            expectEquals (UiScale::fitPercent (125, fullHd), 125);
            expectEquals (UiScale::fitPercent (110, fullHd), 110);
            expectEquals (UiScale::fitPercent (100, fullHd), 100);
        }

        beginTest ("a 1080p laptop already at Windows 125% stops at 125: at 150 the minimum window would not fit");
        {
            const juce::Rectangle<int> laptop (1536, 864);
            expectEquals (UiScale::fitPercent (150, laptop), 125);
            expectEquals (UiScale::fitPercent (125, laptop), 125);
            expectEquals (UiScale::fitPercent (110, laptop), 110);
        }

        beginTest ("a 1280x720 area allows 110 only; a display smaller than the minimum window keeps 100");
        {
            expectEquals (UiScale::fitPercent (150, { 1280, 720 }), 110);
            expectEquals (UiScale::fitPercent (150, { 1024, 600 }), 100);
            expectEquals (UiScale::fitPercent (150, { 0, 0 }), 100);
        }

        beginTest ("a stored value that is not a choice is treated as 100 before fitting");
        {
            expectEquals (UiScale::fitPercent (UiScale::normalise (137), { 1920, 1032 }), 100);
        }
    }
};

static UiScaleTests uiScaleTests;

} // namespace gocue::tests
