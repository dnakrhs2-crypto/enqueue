#include "NativePcmConverter.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace gocue::recorder
{
namespace
{
std::uint32_t readWord(const std::uint8_t* p, const NativeFormat& f) noexcept
{
    std::uint32_t word = 0;
    for (std::uint32_t b = 0; b < f.containerBytes; ++b)
        word |= static_cast<std::uint32_t>(p[b]) << (8 * (f.byteOrder == NativeByteOrder::little ? b : f.containerBytes - 1 - b));
    return word;
}
float readFloat(const std::uint8_t* p, const NativeFormat& f) noexcept
{
    const auto word = readWord(p, f);
    float value = 0; std::memcpy(&value, &word, sizeof(value)); return value;
}
}
bool NativePcmConverter::supports(const NativeFormat& f) noexcept
{
    if (f.containerBytes < 2 || f.containerBytes > 4 || f.strideBytes < f.containerBytes
        || (f.byteOrder != NativeByteOrder::little && f.byteOrder != NativeByteOrder::big)
        || (f.alignment != NativeAlignment::leastSignificant && f.alignment != NativeAlignment::mostSignificant)) return false;
    if (f.encoding == NativeEncoding::ieeeFloat) return f.containerBytes == 4 && f.validBits == 32;
    return f.encoding == NativeEncoding::signedInteger && f.validBits >= 16 && f.validBits <= 32
        && f.validBits <= f.containerBytes * 8;
}
PcmConversionResult NativePcmConverter::pack(const NativeFormat& f, const void* source, std::size_t sourceBytes,
                                            std::uint32_t samples, std::uint8_t* destination, std::size_t destinationBytes) noexcept
{
    PcmConversionResult result;
    if (!supports(f)) { result.error = PcmConversionError::unsupportedFormat; return result; }
    const auto needed = samples ? static_cast<std::uint64_t>(samples - 1) * f.strideBytes + f.containerBytes : 0;
    if (needed > sourceBytes || static_cast<std::uint64_t>(samples) * 3 > destinationBytes || (samples && (!source || !destination)))
    { result.error = PcmConversionError::invalidBuffer; return result; }
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    if (f.encoding == NativeEncoding::ieeeFloat)
        for (std::uint32_t i = 0; i < samples; ++i)
            if (!std::isfinite(readFloat(bytes + static_cast<std::size_t>(i) * f.strideBytes, f)))
            { result.error = PcmConversionError::nonFinite; result.errorSample = i; return result; }
    for (std::uint32_t i = 0; i < samples; ++i)
    {
        const auto* p = bytes + static_cast<std::size_t>(i) * f.strideBytes;
        std::int64_t value = 0;
        if (f.encoding == NativeEncoding::ieeeFloat)
        {
            const double scaled = static_cast<double>(readFloat(p, f)) * 8388608.0;
            if (scaled >= 8388607.5) { value = 8388607; ++result.saturatedSamples; }
            else if (scaled <= -8388608.5) { value = -8388608; ++result.saturatedSamples; }
            else value = static_cast<std::int64_t>(std::round(scaled));
        }
        else
        {
            auto word = static_cast<std::uint64_t>(readWord(p, f));
            if (f.alignment == NativeAlignment::mostSignificant) word >>= f.containerBytes * 8 - f.validBits;
            const auto modulus = std::uint64_t{1} << f.validBits;
            word &= modulus - 1;
            value = (word & (modulus >> 1)) ? static_cast<std::int64_t>(word) - static_cast<std::int64_t>(modulus)
                                          : static_cast<std::int64_t>(word);
            if (f.validBits <= 24) value *= std::int64_t{1} << (24 - f.validBits);
            else
            {
                const auto divisor = std::int64_t{1} << (f.validBits - 24);
                value = value >= 0 ? (value + divisor / 2) / divisor : -((-value + divisor / 2) / divisor);
            }
            if (value > 8388607) { value = 8388607; ++result.saturatedSamples; }
            if (value < -8388608) { value = -8388608; ++result.saturatedSamples; }
        }
        const auto packed = static_cast<std::uint32_t>(value);
        auto* out = destination + static_cast<std::size_t>(i) * 3;
        out[0] = static_cast<std::uint8_t>(packed); out[1] = static_cast<std::uint8_t>(packed >> 8); out[2] = static_cast<std::uint8_t>(packed >> 16);
        const double normalised = static_cast<double>(value) / 8388608.0;
        result.sumSquares += normalised * normalised;
    }
    result.samplesWritten = samples;
    return result;
}
std::string NativePcmConverter::policy(const NativeFormat& f)
{
    if (!supports(f)) return "unsupported; reject block";
    if (f.encoding == NativeEncoding::signedInteger && f.validBits == 16)
        return "signed16 multiplied by 256; low 8 bits zero; mono PCM24 LE; no dither";
    if (f.encoding == NativeEncoding::signedInteger && f.validBits <= 24)
        return f.validBits == 24 ? "signed PCM24 preserved; mono PCM24 LE; no dither"
                                : "signed integer multiplied by 2^(24-validBits); mono PCM24 LE; no dither";
    return "normalise to PCM24; nearest rounding, ties away from zero; saturate [-8388608,8388607]; reject entire block on NaN/Inf; mono PCM24 LE; no dither";
}
}
