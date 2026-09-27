#include "AudioBackends.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <mmdeviceapi.h>
 #include <functiondiscoverykeys_devpkey.h>
 #include <wrl/client.h>
#endif

#include <optional>
#include <vector>

namespace gocue::livemix::AudioBackends
{
namespace
{
    constexpr const char* windowsTypes[] { "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)" };
}

bool isWindows (const juce::String& typeName) noexcept
{
    for (const auto* name : windowsTypes)
        if (typeName == name) return true;
    return false;
}

juce::StringArray availableTypes (juce::AudioDeviceManager& manager)
{
    juce::StringArray offered, result;
    for (auto* type : manager.getAvailableDeviceTypes())
    {
        const auto name = type->getTypeName();
        offered.add (name);
        if (name.containsIgnoreCase ("ASIO")) result.addIfNotAlreadyThere (name);
    }
    for (const auto* name : windowsTypes)
        if (offered.contains (name)) result.add (name);
    return result;
}

juce::String label (const juce::String& typeName)
{
    if (typeName.containsIgnoreCase ("ASIO")) return "ASIO";
    if (typeName == windowsTypes[0]) return juce::String::fromUTF8 ("윈도우 오디오");
    if (typeName == windowsTypes[1]) return juce::String::fromUTF8 ("윈도우 오디오 (저지연)");
    if (typeName == windowsTypes[2]) return juce::String::fromUTF8 ("윈도우 오디오 (독점)");
    return typeName;
}

bool sameContainer (const juce::String& inputName, const juce::String& outputName)
{
    if (inputName.isEmpty() || outputName.isEmpty()) return false;
#if JUCE_WINDOWS
    using Microsoft::WRL::ComPtr;
    struct ComScope
    {
        HRESULT result = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        ~ComScope() { if (SUCCEEDED (result)) CoUninitialize(); }
    } com;
    if (FAILED (com.result) && com.result != RPC_E_CHANGED_MODE) return false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS (enumerator.GetAddressOf())))) return false;

    auto idOf = [] (IMMDevice* device)
    {
        LPWSTR id = nullptr;
        juce::String result;
        if (SUCCEEDED (device->GetId (&id))) result = id;
        CoTaskMemFree (id);
        return result;
    };
    auto defaultId = [&] (EDataFlow flow)
    {
        ComPtr<IMMDevice> device;
        return SUCCEEDED (enumerator->GetDefaultAudioEndpoint (flow, eMultimedia, device.GetAddressOf()))
            ? idOf (device.Get()) : juce::String();
    };
    const auto defaultInput = defaultId (eCapture), defaultOutput = defaultId (eRender);
    ComPtr<IMMDeviceCollection> devices;
    UINT count = 0;
    if (FAILED (enumerator->EnumAudioEndpoints (eAll, DEVICE_STATE_ACTIVE, devices.GetAddressOf()))
        || FAILED (devices->GetCount (&count))) return false;

    juce::StringArray inputNames, outputNames;
    std::vector<std::optional<GUID>> inputContainers, outputContainers;
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        DWORD state = 0;
        if (FAILED (devices->Item (i, device.GetAddressOf()))
            || FAILED (device->GetState (&state)) || state != DEVICE_STATE_ACTIVE) continue;
        const auto id = idOf (device.Get());
        ComPtr<IPropertyStore> properties;
        if (FAILED (device->OpenPropertyStore (STGM_READ, properties.GetAddressOf()))) continue;

        juce::String name;
        PROPVARIANT value {};
        if (SUCCEEDED (properties->GetValue (PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
            name = value.pwszVal;
        PropVariantClear (&value);

        // Missing container properties must NOT remove an endpoint from the name list: that would renumber
        // its duplicates differently from JUCE and could incorrectly join two independent device clocks.
        std::optional<GUID> container;
        if (SUCCEEDED (properties->GetValue (PKEY_Device_ContainerId, &value)))
        {
            GUID guid {};
            const bool isGuid = value.vt == VT_CLSID && value.puuid != nullptr;
            if (isGuid) guid = *value.puuid;
            const bool parsed = isGuid || (value.vt == VT_LPWSTR && value.pwszVal != nullptr
                                           && SUCCEEDED (CLSIDFromString (value.pwszVal, &guid)));
            if (parsed && id.isNotEmpty() && ! IsEqualGUID (guid, GUID_NULL)) container = guid;
        }
        PropVariantClear (&value);

        ComPtr<IMMEndpoint> endpoint;
        EDataFlow flow = eAll;
        if (FAILED (device.As (&endpoint)) || FAILED (endpoint->GetDataFlow (&flow))) continue;
        if (flow != eCapture && flow != eRender) continue;
        auto& names = flow == eCapture ? inputNames : outputNames;
        auto& containers = flow == eCapture ? inputContainers : outputContainers;
        const bool isDefault = id == (flow == eCapture ? defaultInput : defaultOutput);
        names.insert (isDefault ? 0 : -1, name);
        containers.insert (isDefault ? containers.begin() : containers.end(), container);
    }

    // Keep the order and duplicate suffixes identical to WASAPIAudioIODeviceType::scan in JUCE 8.
    inputNames.appendNumbersToDuplicates (false, false);
    outputNames.appendNumbersToDuplicates (false, false);
    const int in = inputNames.indexOf (inputName), out = outputNames.indexOf (outputName);
    if (in < 0 || out < 0) return false;
    const auto& a = inputContainers[(size_t) in];
    const auto& b = outputContainers[(size_t) out];
    return a && b && IsEqualGUID (*a, *b);
#else
    return false;
#endif
}
} // namespace gocue::livemix::AudioBackends
