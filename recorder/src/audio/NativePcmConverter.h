#pragma once
#include "AsioTapAbi.h"
#include <cstddef>
#include <string>

namespace gocue::recorder
{
enum class PcmConversionError { none, unsupportedFormat, invalidBuffer, nonFinite };
struct PcmConversionResult
{
    PcmConversionError error = PcmConversionError::none;
    std::uint32_t samplesWritten = 0, errorSample = 0;
    std::uint64_t saturatedSamples = 0;
    double sumSquares = 0; // normalized PCM24 values, for worker-side RMS
    explicit operator bool() const noexcept { return error == PcmConversionError::none; }
};
class NativePcmConverter
{
public:
    static bool supports(const NativeFormat&) noexcept;
    // Worker only. On nonFinite the WHOLE destination remains untouched.
    // Round nearest, ties away from zero; saturate to [-8388608, 8388607].
    // No dither. Source and destination must not overlap.
    static PcmConversionResult pack(const NativeFormat&, const void* source, std::size_t sourceBytes,
                                    std::uint32_t samples, std::uint8_t* pcm24, std::size_t destinationBytes) noexcept;
    static std::string policy(const NativeFormat&);
};
}
