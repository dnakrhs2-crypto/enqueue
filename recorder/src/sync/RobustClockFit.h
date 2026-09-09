#pragma once
#include "ClockMath.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gocue::recorder::clock_detail
{
struct Point { std::int64_t x = 0, y = 0; };
struct Fit
{
    bool valid = false;
    std::int64_t x0 = 0, y0 = 0;
    double slope = 0, intercept = 0, rms = 0;
    std::size_t inliers = 0, rejected = 0;
};
inline double quantile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0;
    const auto i = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + i, values.end());
    return values[i];
}
// Worker-only bounded window. Seed with medians across quartiles, then two MAD
// passes. Unlike an OLS seed, a few high-leverage callback delays do not steer
// the initial slope and make good observations look like outliers.
inline Fit robustFit(const std::vector<Point>& points, double residualFloor)
{
    Fit fit;
    if (points.size() < 4) return fit;
    fit.x0 = points.front().x; fit.y0 = points.front().y;
    const auto quarter = std::max<std::size_t>(1, points.size() / 4);
    std::vector<double> firstX, firstY, lastX, lastY, offsets;
    for (std::size_t i = 0; i < quarter; ++i)
    {
        const auto a = points[i], b = points[points.size() - quarter + i];
        firstX.push_back(clock_math::difference(a.x, fit.x0)); firstY.push_back(clock_math::difference(a.y, fit.y0));
        lastX.push_back(clock_math::difference(b.x, fit.x0)); lastY.push_back(clock_math::difference(b.y, fit.y0));
    }
    const auto dx = quantile(lastX, 0.5) - quantile(firstX, 0.5);
    if (dx <= 0) return fit;
    fit.slope = (quantile(lastY, 0.5) - quantile(firstY, 0.5)) / dx;
    for (const auto p : points)
        offsets.push_back(clock_math::difference(p.y, fit.y0) - fit.slope * clock_math::difference(p.x, fit.x0));
    fit.intercept = quantile(offsets, 0.5);
    std::vector<bool> accepted(points.size(), true);
    for (unsigned pass = 0; pass < 2; ++pass)
    {
        std::vector<double> residuals, deviations;
        for (const auto p : points)
            residuals.push_back(clock_math::difference(p.y, fit.y0) - fit.slope * clock_math::difference(p.x, fit.x0) - fit.intercept);
        const auto median = quantile(residuals, 0.5);
        for (const auto r : residuals) deviations.push_back(std::abs(r - median));
        const auto threshold = std::max(residualFloor, 4.5 * 1.4826 * quantile(deviations, 0.5));
        double meanX = 0, meanY = 0, xx = 0, xy = 0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            accepted[i] = std::abs(residuals[i] - median) <= threshold;
            if (!accepted[i]) continue;
            const auto x = clock_math::difference(points[i].x, fit.x0), y = clock_math::difference(points[i].y, fit.y0);
            const auto deltaX = x - meanX, deltaY = y - meanY;
            meanX += deltaX / static_cast<double>(++count); meanY += deltaY / static_cast<double>(count);
            xx += deltaX * (x - meanX); xy += deltaX * (y - meanY);
        }
        if (count < 4 || xx <= 0) return fit;
        fit.slope = xy / xx; fit.intercept = meanY - fit.slope * meanX;
        fit.inliers = count; fit.rejected = points.size() - count;
    }
    double squares = 0;
    for (std::size_t i = 0; i < points.size(); ++i)
        if (accepted[i])
        {
            const auto r = clock_math::difference(points[i].y, fit.y0)
                - fit.slope * clock_math::difference(points[i].x, fit.x0) - fit.intercept;
            squares += r * r;
        }
    fit.rms = std::sqrt(squares / static_cast<double>(fit.inliers));
    fit.valid = std::isfinite(fit.slope) && fit.slope > 0 && std::isfinite(fit.intercept);
    return fit;
}
inline double slew(double oldValue, double target, double seconds, double tau, double maximumPerSecond) noexcept
{
    const auto step = (target - oldValue) * -std::expm1(-seconds / tau);
    return oldValue + std::clamp(step, -maximumPerSecond * seconds, maximumPerSecond * seconds);
}
}
