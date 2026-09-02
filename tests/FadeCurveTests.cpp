#include "model/FadeCurve.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

class FadeCurveTests : public juce::UnitTest
{
public:
    FadeCurveTests() : juce::UnitTest ("FadeCurve", "GoCue") {}

    void expectMonotone (const FadeCurve& c, const juce::String& label)
    {
        double last = -1.0;

        for (int i = 0; i <= 200; ++i)
        {
            const double v = c.completion (i / 200.0);
            expect (v >= last - 1.0e-9, label + ": not monotone at " + juce::String (i));
            expect (v >= 0.0 && v <= 1.0, label + ": out of range");
            last = v;
        }
    }

    void runTest() override
    {
        beginTest ("every shape starts at 0, ends at 1 and is monotone");
        {
            FadeCurve s;                       // S-curve
            FadeCurve p; p.shape = CurveShape::parametric; p.intensity = 3.0;
            FadeCurve pm = p; pm.mirror = true;
            FadeCurve l; l.shape = CurveShape::linear;
            FadeCurve c; c.shape = CurveShape::custom; c.points = { { 0.0, 0.0 }, { 0.3, 0.7 }, { 1.0, 1.0 } };

            for (const auto* curve : { &s, &p, &pm, &l, &c })
            {
                expectWithinAbsoluteError (curve->completion (0.0), 0.0, 1e-12);
                expectWithinAbsoluteError (curve->completion (1.0), 1.0, 1e-12);
                expectWithinAbsoluteError (curve->completion (-5.0), 0.0, 1e-12);   // clamped
                expectWithinAbsoluteError (curve->completion (7.0), 1.0, 1e-12);
            }

            expectMonotone (s, "sCurve");
            expectMonotone (p, "parametric");
            expectMonotone (pm, "parametric mirror");
            expectMonotone (l, "linear");
            expectMonotone (c, "custom");

            expectWithinAbsoluteError (s.completion (0.5), 0.5, 1e-12);
            expect (s.completion (0.25) < 0.25);                                   // slow start
            expectWithinAbsoluteError (p.completion (0.5), std::pow (0.5, 3.0), 1e-12);
            expectWithinAbsoluteError (pm.completion (0.5), 0.5, 1e-12);            // symmetric
            expectWithinAbsoluteError (pm.completion (0.25) + pm.completion (0.75), 1.0, 1e-12);
            expectWithinAbsoluteError (l.completion (0.3), 0.3, 1e-12);
            expectWithinAbsoluteError (c.completion (0.3), 0.7, 1e-9);              // passes through its points
            expect (c.completion (0.15) > 0.15);
        }

        beginTest ("domains: decibel is straight, linear is amplitude, slider is between; silence handled");
        {
            FadeCurve lin;
            lin.shape = CurveShape::linear;

            lin.domain = AudioDomain::decibel;
            expectWithinAbsoluteError (lin.interpolateDb (-20.0, 0.0, 0.5, -60.0), -10.0, 1e-9);

            lin.domain = AudioDomain::linear;
            // half way in amplitude between 0.1 (-20 dB) and 1.0 = 0.55 -> -5.19 dB
            expectWithinAbsoluteError (lin.interpolateDb (-20.0, 0.0, 0.5, -60.0), juce::Decibels::gainToDecibels (0.55), 1e-6);

            lin.domain = AudioDomain::slider;
            const double mid = lin.interpolateDb (-20.0, 0.0, 0.5, -60.0);
            expect (mid < -5.19 && mid > -10.0);   // between linear and dB

            // from silence: the first step already moves (no stuck-at-floor), the end reaches the target
            expectWithinAbsoluteError (lin.interpolateDb (-120.0, 0.0, 0.0, -60.0), -60.0, 1e-9);
            expect (lin.interpolateDb (-120.0, 0.0, 0.1, -60.0) > -60.0);
            expectWithinAbsoluteError (lin.interpolateDb (-120.0, 0.0, 1.0, -60.0), 0.0, 1e-9);
            // to silence: ends at the floor and stays there
            expectWithinAbsoluteError (lin.interpolateDb (0.0, -120.0, 1.0, -60.0), -60.0, 1e-9);
            lin.domain = AudioDomain::decibel;
            expectWithinAbsoluteError (lin.interpolateDb (0.0, -120.0, 0.5, -60.0), -30.0, 1e-9);

            // equal power: parametric intensity 0.5 in the linear domain gives -3 dB in the middle
            FadeCurve ep;
            ep.shape = CurveShape::parametric;
            ep.intensity = 0.5;
            ep.domain = AudioDomain::linear;
            expectWithinAbsoluteError (ep.interpolateDb (-120.0, 0.0, 0.5, -120.0), juce::Decibels::gainToDecibels (std::sqrt (0.5)), 1e-6);
        }

        beginTest ("custom points: end points fixed, order kept, mirror lock, add / move / remove");
        {
            FadeCurve c;
            c.shape = CurveShape::custom;
            c.points = { { 0.2, 0.9 }, { 1.0, 0.3 }, { 0.0, 0.5 } };   // unsorted, bad ends
            c.sanitise();
            expectEquals ((int) c.points.size(), 3);
            expectWithinAbsoluteError (c.points.front().x, 0.0, 1e-12);
            expectWithinAbsoluteError (c.points.front().y, 0.0, 1e-12);
            expectWithinAbsoluteError (c.points.back().x, 1.0, 1e-12);
            expectWithinAbsoluteError (c.points.back().y, 1.0, 1e-12);
            expectWithinAbsoluteError (c.points[1].x, 0.2, 1e-12);

            c.addPoint (0.6, 0.4);
            expectEquals ((int) c.points.size(), 4);
            expectWithinAbsoluteError (c.points[2].x, 0.6, 1e-12);
            c.movePoint (2, 0.1, 0.5);            // cannot cross (or land on) its neighbour on the left (0.2)
            expectWithinAbsoluteError (c.points[2].x, 0.2 + FadeCurve::minPointGap, 1e-12);
            expectEquals ((int) c.points.size(), 4);
            FadeCurve twin = c;
            twin.points = { { 0.0, 0.0 }, { 0.5, 0.2 }, { 0.5, 0.8 }, { 1.0, 1.0 } };
            twin.sanitise();
            expectEquals ((int) twin.points.size(), 3);   // two points at one time: the completion may not jump
            c.movePoint (0, 0.4, 0.4);            // end points do not move in x, y is forced back to 0
            expectWithinAbsoluteError (c.points[0].x, 0.0, 1e-12);
            expectWithinAbsoluteError (c.points[0].y, 0.0, 1e-12);
            c.removePoint (0);                    // ignored
            expectEquals ((int) c.points.size(), 4);
            c.removePoint (2);
            expectEquals ((int) c.points.size(), 3);

            FadeCurve m;
            m.shape = CurveShape::custom;
            m.mirror = true;
            m.points = { { 0.0, 0.0 }, { 0.25, 0.1 }, { 1.0, 1.0 } };
            m.sanitise();
            expectEquals ((int) m.points.size(), 4);
            expectWithinAbsoluteError (m.points[2].x, 0.75, 1e-12);
            expectWithinAbsoluteError (m.points[2].y, 0.9, 1e-12);
            expectWithinAbsoluteError (m.completion (0.25) + m.completion (0.75), 1.0, 1e-9);

            FadeCurve empty;
            empty.shape = CurveShape::custom;
            empty.sanitise();
            expectEquals ((int) empty.points.size(), 2);
            expectWithinAbsoluteError (empty.completion (0.4), 0.4, 1e-9);
        }

        beginTest ("JSON round trip and clamps");
        {
            FadeCurve c;
            c.shape = CurveShape::custom;
            c.intensity = 4.5;
            c.mirror = true;
            c.domain = AudioDomain::decibel;
            c.points = { { 0.0, 0.0 }, { 0.3, 0.2 }, { 1.0, 1.0 } };
            c.sanitise();

            const auto back = FadeCurve::fromVar (juce::JSON::parse (juce::JSON::toString (c.toVar())));
            expect (back == c);

            const auto clamped = FadeCurve::fromVar (juce::JSON::parse ("{\"shape\":\"parametric\",\"intensity\":99,\"domain\":\"linear\"}"));
            expect (clamped.shape == CurveShape::parametric);
            expectWithinAbsoluteError (clamped.intensity, FadeCurve::maxIntensity, 1e-12);
            expect (clamped.domain == AudioDomain::linear);
            expect (FadeCurve::fromVar (juce::var()) == FadeCurve());
        }
    }
};

static FadeCurveTests fadeCurveTests;

} // namespace gocue::tests
