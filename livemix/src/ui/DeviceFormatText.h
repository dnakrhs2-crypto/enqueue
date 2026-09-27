#pragma once

#include "MixEngine.h"
#include "ui/UiUtils.h"

namespace gocue::livemix::DeviceFormatText
{
inline juce::String bitDepth (int bits, bool isFloat)
{
    return bits <= 0 ? ko ("알 수 없음") : juce::String (bits) + ko ("비트") + (isFloat ? ko (" 부동소수점") : juce::String());
}

inline juce::String sampleRate (double rate)
{
    return juce::String (rate / 1000.0, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " kHz";
}

inline juce::String choice (int id)
{
    switch (id)
    {
        case 2: return "int16";
        case 3: return "int24";
        case 4: return "int32";
        case 5: return "float32";
        default: return {};
    }
}

inline int choiceId (const juce::String& value)
{
    for (int id = 2; id <= 5; ++id) if (choice (id) == value) return id;
    return 1;
}

inline juce::String choiceName (int id)
{
    return id <= 1 ? ko ("자동 (장치가 받는 가장 높은 형식)") : bitDepth (id == 2 ? 16 : id == 3 ? 24 : 32, id == 5);
}

inline juce::String directions (const MixEngine::DeviceFormat& format, bool inputOpen, bool outputOpen, bool withRates)
{
    juce::StringArray parts;
    auto add = [&] (const juce::String& prefix, int bits, bool isFloat, double rate)
    {
        parts.add (prefix + bitDepth (bits, isFloat) + (withRates && rate > 0 ? ko (" · ") + sampleRate (rate) : juce::String()));
    };
    if (inputOpen) add (ko ("입력 "), format.inputBits, format.inputFloat, format.inputDeviceRate);
    if (outputOpen) add (ko ("출력 "), format.outputBits, format.outputFloat, format.outputDeviceRate);
    return parts.joinIntoString (", ");
}

struct SettingsText { juce::String detail, hint, warning; };

inline SettingsText settings (const MixEngine::DeviceFormat& format, const MixDevice& device, bool hasControlPanel)
{
    using Kind = MixEngine::DeviceFormat::Kind;
    SettingsText text;
    if (device.type == "Windows Audio" || device.type == "Windows Audio (Low Latency Mode)")
        text.hint = ko ("녹음/재생 탭 → 장치 더블클릭 → 고급 → 기본 형식에서 바꿉니다.");
    if (format.kind == Kind::none)
    {
        text.detail = ko ("장치가 열려 있지 않습니다");
        return text;
    }
    if (format.kind == Kind::asio)
    {
        text.detail = bitDepth (format.inputBits, false) + ko (" · ASIO 드라이버가 정합니다")
            + (hasControlPanel ? ko (" (바꿀 수 있는 장치는 ASIO 제어판에서)") : juce::String());
        return text;
    }
    const bool inputOpen = device.input.isNotEmpty(), outputOpen = device.output.isNotEmpty();
    text.detail = directions (format, inputOpen, outputOpen, format.kind == Kind::windowsShared);
    if (format.kind == Kind::windowsShared)
    {
        text.detail += ko (" (윈도우 소리 설정의 '기본 형식')");
        auto conversion = [&] (const juce::String& prefix, double from, double to)
        {
            if (from > 0 && to > 0 && ! juce::approximatelyEqual (from, to))
                text.hint += "\n" + prefix + ko ("윈도우가 ") + sampleRate (from) + ko (" → ") + sampleRate (to) + ko ("로 변환 중");
        };
        if (inputOpen) conversion (ko ("입력: "), format.inputDeviceRate, format.inputStreamRate);
        if (outputOpen) conversion (ko ("출력: "), format.outputStreamRate, format.outputDeviceRate);
    }
    else
    {
        text.detail = ko ("지금: ") + text.detail;
        juce::StringArray warnings;
        auto refused = [&] (const juce::String& prefix, int bits, bool isFloat)
        {
            warnings.add (prefix + ko (" 장치가 ") + choiceName (choiceId (device.sampleFormat))
                + (device.sampleFormat == "float32" ? ko ("을 받지 않아 ") : ko ("를 받지 않아 "))
                + bitDepth (bits, isFloat) + ko ("로 열었습니다."));
        };
        if (device.sampleFormat.isNotEmpty())
        {
            if (inputOpen && format.inputRefused) refused (ko ("입력"), format.inputBits, format.inputFloat);
            if (outputOpen && format.outputRefused) refused (ko ("출력"), format.outputBits, format.outputFloat);
        }
        text.warning = warnings.joinIntoString ("\n");
    }
    return text;
}

struct ExclusiveItem { juce::String text; bool enabled = true; };

inline ExclusiveItem exclusiveItem (int id, const MixEngine::DeviceFormat& format, bool inputOpen, bool outputOpen)
{
    ExclusiveItem item { choiceName (id) };
    const int inputs = inputOpen ? format.inputAccepted : 0, outputs = outputOpen ? format.outputAccepted : 0;
    // Zero masks are unknown (including non-WASAPI test devices), not evidence of an unsupported choice.
    if (id <= 1 || format.kind != MixEngine::DeviceFormat::Kind::windowsExclusive || (inputs | outputs) == 0) return item;
    const int mask = 1 << (id - 2); // WasapiFormatInfo::exclusiveInt16 / Int24 / Int32 / Float32
    const bool input = (inputs & mask) != 0, output = (outputs & mask) != 0;
    item.enabled = input || output;
    if (! item.enabled) item.text += ko (" — 이 장치 지원 안 함");
    else if (input != output) item.text += input ? ko (" — 입력만") : ko (" — 출력만");
    return item;
}
}
