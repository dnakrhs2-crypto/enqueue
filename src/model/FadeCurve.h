#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace gocue
{

enum class CurveShape { sCurve, parametric, linear, custom };

/** The space a level fade is interpolated in (QLab "audio domain"):
    slider = console-fader feel (cube root of amplitude), decibel = straight dB, linear = amplitude. */
enum class AudioDomain { slider, decibel, linear };

struct CurvePoint
{
    double x = 0.0, y = 0.0;   // 0..1 (time fraction, completion)
};

/** Shape of a fade (QLab "Curve Shape"): maps the elapsed fraction 0..1 to a completion 0..1.
    S-curve = cosine easing; parametric = t^k (k = intensity, >1 slow start) mirrored around the middle
    when 'mirror' is set; linear = straight line; custom = points joined by a monotone cubic.
    interpolateDb() applies the completion to a level change in the chosen audio domain. */
struct FadeCurve
{
    CurveShape shape = CurveShape::sCurve;
    double intensity = 2.0;               // parametric exponent (0.1 .. 10)
    std::vector<CurvePoint> points;       // custom: (0,0) ... (1,1), sorted by x
    bool mirror = false;                  // custom: keep the curve point-symmetric around (0.5, 0.5)
    AudioDomain domain = AudioDomain::slider;

    static constexpr double minIntensity = 0.1;

    static constexpr double minPointGap = 0.001;   // custom points: one per time, at least this far apart
    static constexpr double maxIntensity = 10.0;

    /** 0..1 -> 0..1; completion (0) = 0 and completion (1) = 1 for every shape. */
    double completion (double t) const noexcept;

    /** Level at fraction 't' of a fade from 'fromDb' to 'toDb'. Silence (<= minDb) is mapped to the domain's
        floor so fades from / to -inf still move smoothly; the result is clamped back to silence when it
        falls under minDb. */
    double interpolateDb (double fromDb, double toDb, double t, double minDb) const noexcept;

    /** Custom-curve helpers. */
    void setDefaultPoints();
    void addPoint (double x, double y);
    void removePoint (int index);
    void movePoint (int index, double x, double y);
    /** Enforces (0,0)/(1,1) end points, x order, range and the mirror lock. */
    void sanitise();

    bool operator== (const FadeCurve& o) const noexcept;
    bool operator!= (const FadeCurve& o) const noexcept { return ! (*this == o); }

    juce::var toVar() const;
    static FadeCurve fromVar (const juce::var& v);

    static const char* shapeToText (CurveShape s) noexcept;
    static CurveShape shapeFromText (const juce::String& text) noexcept;
    static const char* domainToText (AudioDomain d) noexcept;
    static AudioDomain domainFromText (const juce::String& text) noexcept;

private:
    double customCompletion (double t) const noexcept;
};

} // namespace gocue
