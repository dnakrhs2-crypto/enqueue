#pragma once
#include <intrin.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace gocue::recorder::clock_math
{
enum class Rounding { towardZero, floor, nearest }; // nearest: ties away from zero
inline std::optional<std::int64_t> add(std::int64_t a, std::int64_t b) noexcept
{
    constexpr auto lo = INT64_MIN, hi = INT64_MAX;
    if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b)) return {};
    return a + b;
}
inline std::optional<std::int64_t> subtract(std::int64_t a, std::int64_t b) noexcept
{
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) return {};
    return a - b;
}
// Integer origins are subtracted before conversion, including clocks above 2^53.
inline double difference(std::int64_t a, std::int64_t b) noexcept
{
    if (const auto d = subtract(a, b)) return static_cast<double>(*d);
    return static_cast<double>(a) - static_cast<double>(b);
}
// MSVC x64 128-bit intermediate. A representable result must not be rejected just
// because value*numerator overflows int64 (QPC uptime and long rational timelines).
inline std::optional<std::int64_t> rescale(std::int64_t value, std::uint64_t numerator,
                                         std::uint64_t denominator, Rounding rounding = Rounding::floor) noexcept
{
    if (!denominator) return {};
    const bool negative = value < 0;
    const auto magnitude = negative ? static_cast<std::uint64_t>(-(value + 1)) + 1 : static_cast<std::uint64_t>(value);
    unsigned __int64 high = 0, remainder = 0;
    const auto low = _umul128(magnitude, numerator, &high);
    if (high >= denominator) return {};
    auto result = _udiv128(high, low, denominator, &remainder);
    const bool increment = (rounding == Rounding::floor && negative && remainder)
        || (rounding == Rounding::nearest && remainder >= denominator / 2 + denominator % 2);
    const auto limit = static_cast<std::uint64_t>(INT64_MAX) + (negative ? 1u : 0u);
    if (result > limit || (increment && result == limit)) return {};
    result += increment;
    if (negative && result == (std::uint64_t{1} << 63)) return INT64_MIN;
    return negative ? -static_cast<std::int64_t>(result) : static_cast<std::int64_t>(result);
}
inline std::optional<std::int64_t> roundedOffset(std::int64_t origin, double offset) noexcept
{
    const auto rounded = std::round(offset);
    // double(INT64_MAX) rounds to 2^63; the upper bound must be exclusive.
    if (!std::isfinite(rounded) || rounded < -0x1p63 || rounded >= 0x1p63) return {};
    return add(origin, static_cast<std::int64_t>(rounded));
}
}
