#include "CameraCatalog.h"
#include "app/RecorderSettings.h"
#include <charconv>
#include <sstream>

namespace gocue::recorder
{
bool CameraCatalog::sameDevice(const std::string& a, const std::string& b)
{
    return !a.empty() && !b.empty() && juce::String::fromUTF8(a.c_str()).equalsIgnoreCase(juce::String::fromUTF8(b.c_str()));
}
juce::Result CameraCatalog::configure(const UserSettings& settings)
{
    const auto valid = settings.validate(); if (valid.failed()) return valid;
    if (settings.cameraEnabled[0] && settings.cameraEnabled[1]
        && sameDevice(settings.cameraDeviceIds[0].toStdString(), settings.cameraDeviceIds[1].toStdString()))
        return juce::Result::fail(juce::String::fromUTF8("같은 카메라를 두 번 선택할 수 없습니다."));
    for (unsigned i = 0; i < 2; ++i)
    {
        auto& s = slots[i]; const auto link = settings.cameraDeviceIds[i].toStdString(), mode = settings.cameraModes[i].toStdString();
        if (s.enabled == settings.cameraEnabled[i] && (s.symbolicLink == link || sameDevice(s.symbolicLink, link)) && s.nativeMode == mode) continue;
        s.enabled = settings.cameraEnabled[i]; s.symbolicLink = link; s.nativeMode = mode; s.mode.reset(); ++s.generation;
        s.status = s.enabled && !link.empty() ? CameraSlotStatus::missing : CameraSlotStatus::disabled;
    }
    return juce::Result::ok();
}
void CameraCatalog::refresh(const std::vector<CameraDevice>& devices)
{
    for (auto& s : slots)
    {
        if (!s.enabled || s.symbolicLink.empty()) continue;
        const auto previous = s.status; s.mode.reset();
        s.status = previous == CameraSlotStatus::ready || previous == CameraSlotStatus::disconnected
            ? CameraSlotStatus::disconnected : CameraSlotStatus::missing;
        for (const auto& d : devices) if (sameDevice(d.symbolicLink, s.symbolicLink))
        {
            s.status = CameraSlotStatus::modeUnavailable;
            if (!d.unavailableReason.empty()) break;
            // Match the saved signal, never the transient native type index.
            for (const auto& mode : d.modes) if (mode.text() == s.nativeMode && mode.width == 1920 && mode.height == 1080)
            { s.mode = mode; s.status = CameraSlotStatus::ready; break; }
            break;
        }
        if (s.status != previous) ++s.generation;
    }
}
void CameraCatalog::disconnected(unsigned index)
{
    auto& s = slots.at(index);
    if (s.enabled && s.status != CameraSlotStatus::disconnected)
    { s.status = CameraSlotStatus::disconnected; s.mode.reset(); ++s.generation; }
}
namespace
{
std::uint32_t parsePositive(const std::string& text)
{
    std::uint32_t n = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), n);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || n == 0)
        throw std::invalid_argument("Expected a positive uint32: " + text);
    return n;
}
std::string attributeString(IMFAttributes* attributes, REFGUID key)
{
    WCHAR* raw = nullptr;
    UINT32 count = 0;
    checkHr(attributes->GetAllocatedString(key, &raw, &count), "GetAllocatedString");
    const std::unique_ptr<WCHAR, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
    return juce::String(raw, static_cast<size_t>(count)).toStdString();
}
struct ActivationList
{
    IMFActivate** items = nullptr;
    UINT32 count = 0;
    ~ActivationList()
    {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
};
}
Rational Rational::parse(const std::string& text)
{
    const auto slash = text.find('/');
    if (slash == std::string::npos) throw std::invalid_argument("FPS must be numerator/denominator");
    return {parsePositive(text.substr(0, slash)), parsePositive(text.substr(slash + 1))};
}
std::string Rational::text() const { return std::to_string(numerator) + "/" + std::to_string(denominator); }
const char* subtypeName(CaptureSubtype subtype) noexcept
{
    switch (subtype) { case CaptureSubtype::nv12: return "NV12"; case CaptureSubtype::yuy2: return "YUY2"; default: return "MJPEG"; }
}
GUID subtypeGuid(CaptureSubtype subtype) noexcept
{
    switch (subtype) { case CaptureSubtype::nv12: return MFVideoFormat_NV12; case CaptureSubtype::yuy2: return MFVideoFormat_YUY2; default: return MFVideoFormat_MJPG; }
}
std::string CameraMode::text() const
{
    return std::string(subtypeName(subtype)) + " " + std::to_string(width) + "x" + std::to_string(height) + " " + fps.text();
}
CameraMode CameraMode::parse(const std::string& text)
{
    std::istringstream stream(text);
    std::string subtypeText, dimensions, rate, extra;
    if (!(stream >> subtypeText >> dimensions >> rate) || (stream >> extra)) throw std::invalid_argument("Mode must be 'NV12|YUY2|MJPEG WIDTHxHEIGHT NUM/DEN'");
    CameraMode mode;
    if (subtypeText == "NV12") mode.subtype = CaptureSubtype::nv12;
    else if (subtypeText == "YUY2") mode.subtype = CaptureSubtype::yuy2;
    else if (subtypeText == "MJPEG") mode.subtype = CaptureSubtype::mjpeg;
    else throw std::invalid_argument("Unsupported native subtype: " + subtypeText);
    const auto x = dimensions.find('x');
    if (x == std::string::npos) throw std::invalid_argument("Mode dimensions require WIDTHxHEIGHT");
    mode.width = parsePositive(dimensions.substr(0, x));
    mode.height = parsePositive(dimensions.substr(x + 1));
    if (mode.width > 8192 || mode.height > 8192 || (mode.width % 2) || (mode.height % 2))
        throw std::invalid_argument("Mode dimensions must be even and <=8192");
    mode.fps = Rational::parse(rate);
    return mode;
}
bool CameraMode::sameSignal(const CameraMode& other) const noexcept
{
    return subtype == other.subtype && width == other.width && height == other.height && fps == other.fps;
}
std::optional<CameraMode> CameraCatalog::readMode(IMFMediaType* type, std::uint32_t index)
{
    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) return {};
    CameraMode mode;
    if (subtype == MFVideoFormat_NV12) mode.subtype = CaptureSubtype::nv12;
    else if (subtype == MFVideoFormat_YUY2) mode.subtype = CaptureSubtype::yuy2;
    else if (subtype == MFVideoFormat_MJPG) mode.subtype = CaptureSubtype::mjpeg;
    else return {};
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &mode.width, &mode.height))
        || FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &mode.fps.numerator, &mode.fps.denominator))
        || mode.width == 0 || mode.height == 0 || mode.fps.numerator == 0 || mode.fps.denominator == 0) return {};
    mode.nativeIndex = index;
    UINT32 stride = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) mode.stride = static_cast<LONG>(stride);
    type->GetUINT32(MF_MT_YUV_MATRIX, &mode.colour.matrix);
    type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &mode.colour.range);
    type->GetUINT32(MF_MT_VIDEO_PRIMARIES, &mode.colour.primaries);
    type->GetUINT32(MF_MT_TRANSFER_FUNCTION, &mode.colour.transfer);
    type->GetUINT32(MF_MT_INTERLACE_MODE, &mode.interlace);
    return mode;
}
ComPtr<IMFMediaSource> CameraCatalog::openSource(const std::string& link)
{
    ComPtr<IMFAttributes> attributes;
    checkHr(MFCreateAttributes(&attributes, 2), "MFCreateAttributes");
    checkHr(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID), "Set source type");
    checkHr(attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, juce::String::fromUTF8(link.c_str()).toWideCharPointer()), "Set symbolic link");
    ComPtr<IMFMediaSource> source;
    checkHr(MFCreateDeviceSource(attributes.Get(), &source), "MFCreateDeviceSource");
    return source;
}
std::vector<CameraDevice> CameraCatalog::enumerate()
{
    ComPtr<IMFAttributes> attributes;
    checkHr(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    checkHr(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID), "Set source type");
    ActivationList devices;
    checkHr(MFEnumDeviceSources(attributes.Get(), &devices.items, &devices.count), "MFEnumDeviceSources");
    std::vector<CameraDevice> result;
    for (UINT32 i = 0; i < devices.count; ++i)
    {
        CameraDevice device;
        device.id = "device" + std::to_string(i + 1);
        ComPtr<IMFMediaSource> source;
        try
        {
            device.friendlyName = attributeString(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            device.symbolicLink = attributeString(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            source = openSource(device.symbolicLink);
            ComPtr<IMFAttributes> readerAttributes;
            checkHr(MFCreateAttributes(&readerAttributes, 2), "MFCreateAttributes");
            readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
            readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
            ComPtr<IMFSourceReader> reader;
            checkHr(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttributes.Get(), &reader), "Create catalog reader");
            for (DWORD index = 0; ; ++index)
            {
                ComPtr<IMFMediaType> type;
                const auto hr = reader->GetNativeMediaType(firstVideoStream, index, &type);
                if (hr == MF_E_NO_MORE_TYPES) break;
                checkHr(hr, "GetNativeMediaType");
                if (auto mode = readMode(type.Get(), index)) device.modes.push_back(*mode);
            }
            if (device.modes.empty()) device.unavailableReason = "No native NV12/YUY2/MJPEG mode with an explicit rational frame rate";
        }
        catch (const std::exception& error) { device.unavailableReason = error.what(); }
        if (source) source->Shutdown();
        result.push_back(std::move(device));
    }
    return result;
}
juce::var CameraCatalog::modeJson(const CameraMode& mode)
{
    auto value = jsonObject();
    jsonSet(value, "mode", mode.text());
    jsonSet(value, "subtype", subtypeName(mode.subtype));
    jsonSet(value, "width", jsonInt(mode.width)); jsonSet(value, "height", jsonInt(mode.height));
    jsonSet(value, "fpsNumerator", jsonInt(mode.fps.numerator)); jsonSet(value, "fpsDenominator", jsonInt(mode.fps.denominator));
    jsonSet(value, "nativeIndex", jsonInt(mode.nativeIndex)); jsonSet(value, "stride", static_cast<int>(mode.stride));
    jsonSet(value, "mfMatrix", jsonInt(mode.colour.matrix)); jsonSet(value, "mfRange", jsonInt(mode.colour.range));
    jsonSet(value, "mfPrimaries", jsonInt(mode.colour.primaries)); jsonSet(value, "mfTransfer", jsonInt(mode.colour.transfer));
    jsonSet(value, "mfInterlace", jsonInt(mode.interlace));
    return value;
}
juce::var CameraCatalog::toJson(const std::vector<CameraDevice>& devices)
{
    juce::Array<juce::var> array;
    for (const auto& device : devices)
    {
        auto value = jsonObject();
        jsonSet(value, "id", device.id); jsonSet(value, "friendlyName", device.friendlyName);
        jsonSet(value, "symbolicLink", device.symbolicLink);
        jsonSet(value, "status", device.unavailableReason.empty() ? "available" : "unavailable");
        jsonSet(value, "reason", device.unavailableReason);
        juce::Array<juce::var> modes;
        for (const auto& mode : device.modes) modes.add(modeJson(mode));
        jsonSet(value, "nativeTypes", modes); array.add(value);
    }
    return array;
}
}
