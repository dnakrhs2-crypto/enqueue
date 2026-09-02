#include "model/FadeCurve.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace gocue
{

double FadeCurve::completion (double t) const noexcept
{
    if (! std::isfinite (t))
        return 0.0;

    t = juce::jlimit (0.0, 1.0, t);

    switch (shape)
    {
        case CurveShape::linear:
            return t;

        case CurveShape::sCurve:
            return 0.5 - 0.5 * std::cos (t * juce::MathConstants<double>::pi);

        case CurveShape::parametric:
        {
            const double k = juce::jlimit (minIntensity, maxIntensity, intensity);

            if (! mirror)
                return std::pow (t, k);

            // symmetric: ease in over the first half, ease out over the second
            if (t < 0.5)
                return 0.5 * std::pow (2.0 * t, k);

            return 1.0 - 0.5 * std::pow (2.0 * (1.0 - t), k);
        }

        case CurveShape::custom:
            return customCompletion (t);
    }

    return t;
}

double FadeCurve::customCompletion (double t) const noexcept
{
    if (points.size() < 2)
        return t;

    // monotone cubic (Fritsch-Carlson) through the points: no overshoot between them
    const int n = (int) points.size();
    int i = 0;

    while (i < n - 2 && points[(size_t) i + 1].x <= t)
        ++i;

    const auto& p0 = points[(size_t) i];
    const auto& p1 = points[(size_t) i + 1];
    const double h = p1.x - p0.x;

    if (h <= 1.0e-9)
        return p1.y;

    auto slopeOf = [this, n] (int k) -> double
    {
        const auto& a = points[(size_t) k];
        const auto& b = points[(size_t) k + 1];
        const double dx = b.x - a.x;
        return dx > 1.0e-9 ? (b.y - a.y) / dx : 0.0;
    };

    const double d = slopeOf (i);
    double m0 = i > 0 ? 0.5 * (slopeOf (i - 1) + d) : d;
    double m1 = i < n - 2 ? 0.5 * (d + slopeOf (i + 1)) : d;

    if (std::abs (d) < 1.0e-12)
    {
        m0 = m1 = 0.0;
    }
    else
    {
        const double a = m0 / d, b = m1 / d;
        const double s = a * a + b * b;

        if (s > 9.0)
        {
            const double tau = 3.0 / std::sqrt (s);
            m0 = tau * a * d;
            m1 = tau * b * d;
        }
    }

    const double u = (t - p0.x) / h;
    const double u2 = u * u, u3 = u2 * u;
    const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
    const double h10 = u3 - 2.0 * u2 + u;
    const double h01 = -2.0 * u3 + 3.0 * u2;
    const double h11 = u3 - u2;
    return juce::jlimit (0.0, 1.0, h00 * p0.y + h10 * h * m0 + h01 * p1.y + h11 * h * m1);
}

double FadeCurve::interpolateDb (double fromDb, double toDb, double t, double minDb) const noexcept
{
    const double c = completion (t);
    const double floorDb = minDb;
    const double from = std::isfinite (fromDb) ? juce::jmax (floorDb, fromDb) : floorDb;
    const double to = std::isfinite (toDb) ? juce::jmax (floorDb, toDb) : floorDb;
    double result;

    switch (domain)
    {
        case AudioDomain::decibel:
            result = from + (to - from) * c;
            break;

        case AudioDomain::linear:
        {
            // silence sits at the floor level (the bottom of the fader), so a fade out of -inf starts moving at once
            const double a = juce::Decibels::decibelsToGain (from);
            const double b = juce::Decibels::decibelsToGain (to);
            const double g = a + (b - a) * c;
            result = g > 0.0 ? juce::Decibels::gainToDecibels (g, floorDb) : floorDb;
            break;
        }

        case AudioDomain::slider:
        default:
        {
            // console-fader feel: interpolate in the cube root of the amplitude
            const double a = std::cbrt (juce::Decibels::decibelsToGain (from));
            const double b = std::cbrt (juce::Decibels::decibelsToGain (to));
            const double r = a + (b - a) * c;
            const double g = r * r * r;
            result = g > 0.0 ? juce::Decibels::gainToDecibels (g, floorDb) : floorDb;
            break;
        }
    }

    if (result <= floorDb + 1.0e-9)
        return floorDb;

    return result;
}

//==============================================================================
void FadeCurve::setDefaultPoints()
{
    points = { { 0.0, 0.0 }, { 1.0, 1.0 } };
}

void FadeCurve::addPoint (double x, double y)
{
    points.push_back ({ juce::jlimit (0.0, 1.0, x), juce::jlimit (0.0, 1.0, y) });
    sanitise();
}

void FadeCurve::removePoint (int index)
{
    if (index <= 0 || index >= (int) points.size() - 1)
        return;   // the end points stay

    points.erase (points.begin() + index);
    sanitise();
}

void FadeCurve::movePoint (int index, double x, double y)
{
    if (index < 0 || index >= (int) points.size())
        return;

    auto& p = points[(size_t) index];
    p.y = juce::jlimit (0.0, 1.0, y);

    if (index == 0)
        p.x = 0.0;
    else if (index == (int) points.size() - 1)
        p.x = 1.0;
    else
        p.x = juce::jlimit (points[(size_t) index - 1].x, points[(size_t) index + 1].x, x);

    sanitise();
}

void FadeCurve::sanitise()
{
    if (! std::isfinite (intensity))
        intensity = 2.0;

    intensity = juce::jlimit (minIntensity, maxIntensity, intensity);

    for (auto& p : points)
    {
        if (! std::isfinite (p.x)) p.x = 0.0;
        if (! std::isfinite (p.y)) p.y = 0.0;
        p.x = juce::jlimit (0.0, 1.0, p.x);
        p.y = juce::jlimit (0.0, 1.0, p.y);
    }

    std::stable_sort (points.begin(), points.end(), [] (const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });

    if (points.size() < 2)
        setDefaultPoints();

    points.front() = { 0.0, 0.0 };
    points.back() = { 1.0, 1.0 };

    if (mirror && points.size() > 2)
    {
        // rebuild the right half as the point reflection of the left half through (0.5, 0.5)
        std::vector<CurvePoint> left;

        for (const auto& p : points)
            if (p.x < 0.5)
                left.push_back (p);

        std::vector<CurvePoint> rebuilt = left;

        for (const auto& p : points)
            if (std::abs (p.x - 0.5) < 1.0e-9)
            {
                rebuilt.push_back ({ 0.5, 0.5 });
                break;
            }

        for (auto it = left.rbegin(); it != left.rend(); ++it)
            rebuilt.push_back ({ 1.0 - it->x, 1.0 - it->y });

        points = rebuilt;
        points.front() = { 0.0, 0.0 };
        points.back() = { 1.0, 1.0 };
    }
}

bool FadeCurve::operator== (const FadeCurve& o) const noexcept
{
    if (shape != o.shape || domain != o.domain || mirror != o.mirror || intensity != o.intensity || points.size() != o.points.size())
        return false;

    for (size_t i = 0; i < points.size(); ++i)
        if (points[i].x != o.points[i].x || points[i].y != o.points[i].y)
            return false;

    return true;
}

//==============================================================================
const char* FadeCurve::shapeToText (CurveShape s) noexcept
{
    switch (s)
    {
        case CurveShape::sCurve:     return "sCurve";
        case CurveShape::parametric: return "parametric";
        case CurveShape::linear:     return "linear";
        case CurveShape::custom:     return "custom";
    }

    return "sCurve";
}

CurveShape FadeCurve::shapeFromText (const juce::String& text) noexcept
{
    if (text == "parametric") return CurveShape::parametric;
    if (text == "linear")     return CurveShape::linear;
    if (text == "custom")     return CurveShape::custom;
    return CurveShape::sCurve;
}

const char* FadeCurve::domainToText (AudioDomain d) noexcept
{
    switch (d)
    {
        case AudioDomain::slider:  return "slider";
        case AudioDomain::decibel: return "decibel";
        case AudioDomain::linear:  return "linear";
    }

    return "slider";
}

AudioDomain FadeCurve::domainFromText (const juce::String& text) noexcept
{
    if (text == "decibel") return AudioDomain::decibel;
    if (text == "linear")  return AudioDomain::linear;
    return AudioDomain::slider;
}

juce::var FadeCurve::toVar() const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("shape", shapeToText (shape));
    obj->setProperty ("intensity", intensity);
    obj->setProperty ("mirror", mirror);
    obj->setProperty ("domain", domainToText (domain));

    juce::Array<juce::var> arr;

    for (const auto& p : points)
    {
        juce::Array<juce::var> pair;
        pair.add (p.x);
        pair.add (p.y);
        arr.add (juce::var (pair));
    }

    obj->setProperty ("points", juce::var (arr));
    return juce::var (obj);
}

FadeCurve FadeCurve::fromVar (const juce::var& v)
{
    FadeCurve c;

    if (v.getDynamicObject() == nullptr)
        return c;

    c.shape = shapeFromText (v.getProperty ("shape", "sCurve").toString());
    c.intensity = (double) v.getProperty ("intensity", 2.0);
    c.mirror = (bool) v.getProperty ("mirror", false);
    c.domain = domainFromText (v.getProperty ("domain", "slider").toString());

    if (const auto* arr = v.getProperty ("points", juce::var()).getArray())
        for (const auto& item : *arr)
            if (const auto* pair = item.getArray(); pair != nullptr && pair->size() >= 2)
                c.points.push_back ({ (double) (*pair)[0], (double) (*pair)[1] });

    if (c.shape == CurveShape::custom || ! c.points.empty())
        c.sanitise();
    else
        c.intensity = juce::jlimit (minIntensity, maxIntensity, std::isfinite (c.intensity) ? c.intensity : 2.0);

    return c;
}

} // namespace gocue
