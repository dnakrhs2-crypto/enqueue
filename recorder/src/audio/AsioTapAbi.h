#pragma once
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace juce { class AudioIODevice; }
namespace gocue::recorder
{
// Recorder supplies this header to the JUCE module ONLY in a hook-enabled build.
// No JUCE public header, AudioIODeviceCallbackContext or ASIO SDK type is changed.
enum class NativeEncoding : std::uint32_t { unsupported, signedInteger, ieeeFloat };
enum class NativeByteOrder : std::uint32_t { little, big };
enum class NativeAlignment : std::uint32_t { leastSignificant, mostSignificant };
struct NativeFormat
{
    std::int32_t asioSampleType;
    NativeEncoding encoding;
    NativeByteOrder byteOrder;
    NativeAlignment alignment;
    std::uint32_t containerBytes, validBits, strideBytes;
};

// ASIO LSB/MSB names specify BYTE order. Int32*16/18/20/24 store a
// right-aligned signed value in a 32-bit container. Alignment is independent
// of byte order so explicit left-aligned fixtures/adapters are also supported.
inline NativeFormat nativeFormatForAsio(std::int32_t type) noexcept
{
    NativeFormat f{type, NativeEncoding::unsupported, NativeByteOrder::little,
                   NativeAlignment::leastSignificant, 0, 0, 0};
    const bool big = type >= 0 && type < 16;
    f.byteOrder = big ? NativeByteOrder::big : NativeByteOrder::little;
    const auto base = big ? type : type - 16;
    switch (base)
    {
        case 0: f.containerBytes = 2; f.validBits = 16; break;
        case 1: f.containerBytes = 3; f.validBits = 24; break;
        case 2: f.containerBytes = 4; f.validBits = 32; break;
        case 3: f.containerBytes = 4; f.validBits = 32; f.encoding = NativeEncoding::ieeeFloat; break;
        case 4: f.containerBytes = 8; f.validBits = 64; break; // descriptor only: float64 rejected
        case 8: f.containerBytes = 4; f.validBits = 16; break;
        case 9: f.containerBytes = 4; f.validBits = 18; break;
        case 10: f.containerBytes = 4; f.validBits = 20; break;
        case 11: f.containerBytes = 4; f.validBits = 24; break;
        default: return f;
    }
    if (base != 4 && f.encoding == NativeEncoding::unsupported) f.encoding = NativeEncoding::signedInteger;
    f.strideBytes = f.containerBytes;
    return f;
}
inline bool sameNativeFormat(const NativeFormat& a, const NativeFormat& b) noexcept
{
    return a.asioSampleType == b.asioSampleType && a.encoding == b.encoding && a.byteOrder == b.byteOrder
        && a.alignment == b.alignment && a.containerBytes == b.containerBytes
        && a.validBits == b.validBits && a.strideBytes == b.strideBytes;
}
struct NativeInputView
{
    const void* data;
    std::int32_t activeIndex, physicalIndex;
    NativeFormat format;
};
enum BlockStampFlag : std::uint32_t
{
    timeInfoPresent = 1u << 0, samplePositionValid = 1u << 1, systemTimeValid = 1u << 2,
    sampleRateValid = 1u << 3, latenciesValid = 1u << 4, overloadReportingSupported = 1u << 5
};
struct BlockStamp
{
    std::uint32_t flags, asioTimeInfoFlags;
    std::int64_t samplePosition, callbackQpc;
    std::uint64_t systemTimeRaw, sequence;
    double sampleRate;
    std::int32_t bufferIndex, inputLatencySamples, outputLatencySamples;
    std::uint32_t numSamples;
    std::uint64_t xruns, resets, resyncs, latencyChanges;
};
struct SamplePositionPoll
{
    std::int64_t qpcBefore, qpcAfter, samplePosition;
    std::uint64_t systemTimeRaw;
    std::int32_t asioError;
    std::uint32_t valid;
};
struct AsioEventCounters
{
    std::uint64_t xruns, resets, resyncs, latencyChanges;
    std::uint32_t overloadReportingAvailable;
};
using AsioTapFunction = void (*)(const BlockStamp&, const NativeInputView*, std::uint32_t) noexcept;
// Set/clear ONLY while the ASIO driver is stopped. A null store is NOT an
// in-flight callback barrier. Owner must close the device before unregistering.
extern std::atomic<AsioTapFunction> recorderAsioTap;
// Patched JUCE implementation. Control/measurement thread only, never the tap.
bool pollAsioSamplePosition(juce::AudioIODevice&, SamplePositionPoll&) noexcept;
bool readAsioEventCounters(const juce::AudioIODevice&, AsioEventCounters&) noexcept;
static_assert(std::is_trivial_v<BlockStamp> && std::is_standard_layout_v<BlockStamp>);
static_assert(std::is_trivially_copyable_v<NativeInputView>);
static_assert(std::atomic<AsioTapFunction>::is_always_lock_free);
}
