#pragma once

#include "Localisation.h"
#include "MixEngine.h"
#include "ui/UiUtils.h"

namespace gocue::livemix::DeviceFormatText
{
inline juce::String bitDepth (int bits, bool isFloat)
{
    return bits <= 0 ? tr ("알 수 없음") : (isFloat ? trf ("{0}비트 부동소수점", bits) : trf ("{0}비트", bits));
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
    return id <= 1 ? tr ("자동 (장치가 받는 가장 높은 형식)") : bitDepth (id == 2 ? 16 : id == 3 ? 24 : 32, id == 5);
}

inline juce::String directions (const MixEngine::DeviceFormat& format, bool inputOpen, bool outputOpen, bool withRates)
{
    juce::StringArray parts;
    auto add = [&] (const juce::String& prefix, int bits, bool isFloat, double rate)
    {
        parts.add (prefix + bitDepth (bits, isFloat) + (withRates && rate > 0 ? ko (" · ") + sampleRate (rate) : juce::String()));
    };
    if (inputOpen) add (tr ("입력 "), format.inputBits, format.inputFloat, format.inputDeviceRate);
    if (outputOpen) add (tr ("출력 "), format.outputBits, format.outputFloat, format.outputDeviceRate);
    return parts.joinIntoString (", ");
}

struct SettingsText { juce::String detail, hint, warning; };

inline SettingsText settings (const MixEngine::DeviceFormat& format, const MixDevice& device, bool hasControlPanel)
{
    using Kind = MixEngine::DeviceFormat::Kind;
    SettingsText text;
    if (device.type == "Windows Audio" || device.type == "Windows Audio (Low Latency Mode)")
        text.hint = tr ("녹음/재생 탭 → 장치 더블클릭 → 고급 → 기본 형식에서 바꿉니다.");
    if (format.kind == Kind::none)
    {
        text.detail = tr ("장치가 열려 있지 않습니다");
        return text;
    }
    if (format.kind == Kind::asio)
    {
        text.detail = trf ("{0} · ASIO 드라이버가 정합니다{1}", bitDepth (format.inputBits, false), (hasControlPanel ? tr (" (바꿀 수 있는 장치는 ASIO 제어판에서)") : juce::String()));
        return text;
    }
    const bool inputOpen = device.input.isNotEmpty(), outputOpen = device.output.isNotEmpty();
    text.detail = directions (format, inputOpen, outputOpen, format.kind == Kind::windowsShared);
    if (format.kind == Kind::windowsShared)
    {
        text.detail += tr (" (윈도우 소리 설정의 '기본 형식')");
        auto conversion = [&] (const juce::String& prefix, double from, double to)
        {
            if (from > 0 && to > 0 && ! juce::approximatelyEqual (from, to))
                text.hint += trf ("\n{0}윈도우가 {1} → {2}로 변환 중", prefix, sampleRate (from), sampleRate (to));
        };
        if (inputOpen) conversion (tr ("입력: "), format.inputDeviceRate, format.inputStreamRate);
        if (outputOpen) conversion (tr ("출력: "), format.outputStreamRate, format.outputDeviceRate);
    }
    else
    {
        text.detail = trf ("지금: {0}", text.detail);
        juce::StringArray warnings;
        auto refused = [&] (const juce::String& prefix, int bits, bool isFloat)
        {
            warnings.add (device.sampleFormat == "float32"
                ? trf ("{0} 장치가 {1}을 받지 않아 {2}로 열었습니다.", prefix, choiceName (choiceId (device.sampleFormat)), bitDepth (bits, isFloat))
                : trf ("{0} 장치가 {1}를 받지 않아 {2}로 열었습니다.", prefix, choiceName (choiceId (device.sampleFormat)), bitDepth (bits, isFloat)));
        };
        if (device.sampleFormat.isNotEmpty())
        {
            if (inputOpen && format.inputRefused) refused (tr ("입력"), format.inputBits, format.inputFloat);
            if (outputOpen && format.outputRefused) refused (tr ("출력"), format.outputBits, format.outputFloat);
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
    if (! item.enabled) item.text += tr (" — 이 장치 지원 안 함");
    else if (input != output) item.text += input ? tr (" — 입력만") : tr (" — 출력만");
    return item;
}
}
