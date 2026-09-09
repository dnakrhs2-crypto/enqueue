#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <juce_core/juce_core.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace gocue::recorder
{
template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
inline constexpr DWORD firstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
inline constexpr DWORD allSourceStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
inline std::string hresultText(HRESULT hr)
{
    char buffer[16]{};
    snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}
inline void checkHr(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + ": " + hresultText(hr));
}
inline std::int64_t qpcNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
inline std::int64_t qpcFrequency() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}
inline std::string utcNowIso8601()
{
    SYSTEMTIME time{}; GetSystemTime(&time);
    char text[32]{};
    snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth), static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond), static_cast<unsigned>(time.wMilliseconds));
    return text;
}
// Scoped and balanced on each owner thread. MFStartup/MFShutdown never run on an MF callback.
struct ComApartment
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComApartment() { checkHr(result, "CoInitializeEx(MTA)"); }
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};
struct MfRuntime
{
    MfRuntime() { checkHr(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup"); }
    ~MfRuntime() { MFShutdown(); }
    MfRuntime(const MfRuntime&) = delete;
    MfRuntime& operator=(const MfRuntime&) = delete;
};
inline juce::var jsonObject() { return juce::var(new juce::DynamicObject()); }
inline void jsonSet(juce::var& value, const char* key, const juce::var& item)
{
    value.getDynamicObject()->setProperty(key, item);
}
inline void jsonSet(juce::var& value, const char* key, const std::string& item)
{
    jsonSet(value, key, juce::var(juce::String::fromUTF8(item.c_str())));
}
inline void jsonSet(juce::var& value, const char* key, const char* item)
{
    jsonSet(value, key, juce::var(item));
}
// Literal zero must remain a JSON number, not bind to the const char* overload
// as a null pointer. Preserve bool/double as well as signed integer values.
template<class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
inline void jsonSet(juce::var& value, const char* key, T item)
{
    if constexpr (std::is_same_v<T, bool>) jsonSet(value, key, juce::var(item));
    else if constexpr (std::is_floating_point_v<T>) jsonSet(value, key, juce::var(static_cast<double>(item)));
    else jsonSet(value, key, juce::var(static_cast<juce::int64>(item)));
}
inline juce::var jsonInt(std::uint64_t value) { return juce::var(static_cast<juce::int64>(value)); }
}
