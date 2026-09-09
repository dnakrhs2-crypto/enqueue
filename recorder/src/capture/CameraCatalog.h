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
struct UserSettings;
enum class CameraSlotStatus { disabled, missing, ready, disconnected, modeUnavailable };
struct CameraSlot
{
    bool enabled = false;
    std::string symbolicLink, nativeMode;
    std::optional<CameraMode> mode;
    std::uint64_t generation = 0;
    CameraSlotStatus status = CameraSlotStatus::disabled;
    bool ready() const noexcept { return status == CameraSlotStatus::ready; }
};
class CameraCatalog
{
public:
    // Owner-thread model. Device order/friendly names never determine slot identity.
    juce::Result configure(const UserSettings&);
    void refresh(const std::vector<CameraDevice>&);
    void disconnected(unsigned slot);
    const CameraSlot& slot(unsigned index) const { return slots.at(index); }
    static bool sameDevice(const std::string&, const std::string&);
    // Caller owns a live MTA + MF runtime. Enumeration opens types, never requests samples.
    static std::vector<CameraDevice> enumerate();
    static ComPtr<IMFMediaSource> openSource(const std::string& symbolicLink);
    static std::optional<CameraMode> readMode(IMFMediaType*, std::uint32_t index = 0);
    static juce::var modeJson(const CameraMode&);
    static juce::var toJson(const std::vector<CameraDevice>&);
private:
    std::array<CameraSlot, 2> slots{};
};
}
