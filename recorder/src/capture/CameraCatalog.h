#pragma once
#include "support/Platform.h"
#include <optional>
#include <vector>

namespace gocue::recorder
{
struct Rational
{
    std::uint32_t numerator = 0, denominator = 1;
    static Rational parse(const std::string& text);
    std::string text() const;
    double value() const noexcept { return static_cast<double>(numerator) / denominator; }
    double periodMs() const noexcept { return 1000.0 * denominator / numerator; }
    bool operator==(Rational rhs) const noexcept { return numerator == rhs.numerator && denominator == rhs.denominator; }
};
enum class CaptureSubtype { nv12, yuy2, mjpeg };
const char* subtypeName(CaptureSubtype) noexcept;
GUID subtypeGuid(CaptureSubtype) noexcept;
struct ColourInfo
{
    // Zero means missing/unknown. Preserve the MF numeric values in diagnostics.
    std::uint32_t matrix = 0, range = 0, primaries = 0, transfer = 0;
};
struct CameraMode
{
    CaptureSubtype subtype = CaptureSubtype::nv12;
    std::uint32_t width = 0, height = 0, nativeIndex = 0;
    Rational fps;
    LONG stride = 0;
    std::uint32_t interlace = 0;
    ColourInfo colour;
    std::string text() const;
    static CameraMode parse(const std::string&);
    bool sameSignal(const CameraMode&) const noexcept;
};
struct CameraDevice
{
    std::string id, friendlyName, symbolicLink, unavailableReason;
    std::vector<CameraMode> modes;
};
class CameraCatalog
{
public:
    // Caller owns a live MTA + MF runtime. Enumeration opens types, never requests samples.
    static std::vector<CameraDevice> enumerate();
    static ComPtr<IMFMediaSource> openSource(const std::string& symbolicLink);
    static std::optional<CameraMode> readMode(IMFMediaType*, std::uint32_t index = 0);
    static juce::var modeJson(const CameraMode&);
    static juce::var toJson(const std::vector<CameraDevice>&);
};
}
