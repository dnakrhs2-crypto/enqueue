#include "model/Envelope.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class EnvelopeTests : public juce::UnitTest
{
public:
    EnvelopeTests() : juce::UnitTest ("Envelope", "GoCue") {}

    void runTest() override
    {
        beginTest ("inactive envelopes are unity gain");
        {
            Envelope e;
            expectEquals (e.levelAt (0.0, 10.0), 1.0f);
            expectEquals (e.levelAt (5.0, 10.0), 1.0f);

            e.enabled = true;                                   // enabled but no points
            expectEquals (e.levelAt (5.0, 10.0), 1.0f);
            expectEquals (e.fadeInSeconds (10.0), 0.0);
        }

        beginTest ("linear segments interpolate and hold the end levels");
        {
            Envelope e;
            e.enabled = true;
            e.linear = true;
            e.lockToTrim = false;
            e.points = { { 1.0, 0.0 }, { 2.0, 1.0 }, { 3.0, 0.5 } };

            expectEquals (e.levelAt (0.0, 4.0), 0.0f);          // before the first point: first level
            expectWithinAbsoluteError (e.levelAt (1.5, 4.0), 0.5f, 1e-6f);
            expectWithinAbsoluteError (e.levelAt (2.0, 4.0), 1.0f, 1e-6f);
            expectWithinAbsoluteError (e.levelAt (2.5, 4.0), 0.75f, 1e-6f);
            expectEquals (e.levelAt (9.0, 4.0), 0.5f);          // after the last point: last level
            expectWithinAbsoluteError (e.fadeInSeconds (4.0), 2.0, 1e-9);
        }

        beginTest ("smooth segments ease in and out but hit the points exactly");
        {
            Envelope e;
            e.enabled = true;
            e.linear = false;
            e.lockToTrim = false;
            e.points = { { 0.0, 0.0 }, { 2.0, 1.0 } };

            expectWithinAbsoluteError (e.levelAt (0.0, 2.0), 0.0f, 1e-6f);
            expectWithinAbsoluteError (e.levelAt (1.0, 2.0), 0.5f, 1e-6f);        // symmetric
            expectWithinAbsoluteError (e.levelAt (2.0, 2.0), 1.0f, 1e-6f);
            expectLessThan (e.levelAt (0.5, 2.0), 0.25f);                          // slower start than linear
            expectGreaterThan (e.levelAt (1.5, 2.0), 0.75f);
        }

        beginTest ("locked envelopes stretch with the region length");
        {
            Envelope e;
            e.enabled = true;
            e.linear = true;
            e.lockToTrim = true;
            e.points = { { 0.0, 0.0 }, { 0.25, 1.0 }, { 0.75, 1.0 }, { 1.0, 0.0 } };

            expectWithinAbsoluteError (e.levelAt (1.0, 8.0), 0.5f, 1e-6f);        // 1 s of an 8 s region = 0.125
            expectWithinAbsoluteError (e.levelAt (2.0, 8.0), 1.0f, 1e-6f);
            expectWithinAbsoluteError (e.levelAt (7.0, 8.0), 0.5f, 1e-6f);
            expectWithinAbsoluteError (e.fadeInSeconds (8.0), 2.0, 1e-9);
            expectWithinAbsoluteError (e.fadeInSeconds (4.0), 1.0, 1e-9);

            e.setLockToTrim (false, 8.0);                                          // convert to seconds
            expect (! e.lockToTrim);
            expectWithinAbsoluteError (e.points[1].x, 2.0, 1e-9);
            expectWithinAbsoluteError (e.points[3].x, 8.0, 1e-9);
            expectWithinAbsoluteError (e.levelAt (1.0, 8.0), 0.5f, 1e-6f);

            e.setLockToTrim (true, 4.0);                                           // and back, at a new length
            expectWithinAbsoluteError (e.points[1].x, 0.5, 1e-9);
            expectWithinAbsoluteError (e.points[3].x, 1.0, 1e-9);                  // clamped to the region
        }

        beginTest ("sanitise sorts, clamps and limits the point count");
        {
            Envelope e;
            e.lockToTrim = true;
            e.points = { { 0.9, 2.0 }, { -1.0, -3.0 }, { 0.5, 0.5 } };
            e.sanitise();
            expectEquals ((int) e.points.size(), 3);
            expectWithinAbsoluteError (e.points[0].x, 0.0, 1e-12);
            expectWithinAbsoluteError (e.points[0].level, 0.0, 1e-12);
            expectWithinAbsoluteError (e.points[1].x, 0.5, 1e-12);
            expectWithinAbsoluteError (e.points[2].level, 1.0, 1e-12);

            for (int i = 0; i < Envelope::maxPoints + 10; ++i)
                e.points.push_back ({ i / 1000.0, 1.0 });

            e.sanitise();
            expectEquals ((int) e.points.size(), Envelope::maxPoints);
        }

        beginTest ("fromFadeIn builds the version-1 fade-in shape");
        {
            const auto none = Envelope::fromFadeIn (0.0);
            expect (! none.enabled);
            expectEquals (none.levelAt (0.0, 1.0), 1.0f);

            const auto fade = Envelope::fromFadeIn (0.25);
            expect (fade.enabled && fade.linear && ! fade.lockToTrim);
            expectWithinAbsoluteError (fade.levelAt (0.0, 10.0), 0.0f, 1e-6f);
            expectWithinAbsoluteError (fade.levelAt (0.125, 10.0), 0.5f, 1e-6f);
            expectWithinAbsoluteError (fade.levelAt (0.25, 10.0), 1.0f, 1e-6f);
            expectWithinAbsoluteError (fade.levelAt (5.0, 10.0), 1.0f, 1e-6f);
            expectWithinAbsoluteError (fade.fadeInSeconds (10.0), 0.25, 1e-9);
        }
    }
};

static EnvelopeTests envelopeTests;

} // namespace gocue::tests
