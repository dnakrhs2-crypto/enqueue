#pragma once

#include <juce_graphics/juce_graphics.h>

namespace gocue::CueColors
{

/** 0 = none, 1..20 = the cue colour palette (8 basic + 12 extended, like QLab). */
constexpr int numColors = 20;

inline juce::Colour get (int index) noexcept
{
    static const juce::uint32 palette[numColors] = {
        0xffd94b4b, 0xffe08a2e, 0xffd8c22e, 0xff4caf50, 0xff2f9ec9, 0xff4a63d9, 0xff9a5bd1, 0xff8a8a8a,   // basic
        0xffb03a3a, 0xffb86a2a, 0xff9c8f1f, 0xff2e7d32, 0xff1f6f8e, 0xff34479c, 0xff6f3f99, 0xffd77aa5,
        0xff7a5a3a, 0xff3f8f7a, 0xff5c6bc0, 0xffc0c0c0 };

    if (index < 1 || index > numColors)
        return juce::Colours::transparentBlack;

    return juce::Colour (palette[index - 1]);
}

inline const char* name (int index) noexcept
{
    static const char* const names[numColors] = {
        "\xEB\xB9\xA8\xEA\xB0\x95",           // 빨강
        "\xEC\xA3\xBC\xED\x99\xA9",           // 주황
        "\xEB\x85\xB8\xEB\x9E\x91",           // 노랑
        "\xEC\xB4\x88\xEB\xA1\x9D",           // 초록
        "\xED\x95\x98\xEB\x8A\x98",           // 하늘
        "\xED\x8C\x8C\xEB\x9E\x91",           // 파랑
        "\xEB\xB3\xB4\xEB\x9D\xBC",           // 보라
        "\xED\x9A\x8C\xEC\x83\x89",           // 회색
        "\xEC\xA7\x84\xED\x95\x9C \xEB\xB9\xA8\xEA\xB0\x95",   // 진한 빨강
        "\xEA\xB0\x88\xEC\x83\x89",           // 갈색
        "\xEC\x98\xAC\xEB\xA6\xAC\xEB\xB8\x8C", // 올리브
        "\xEC\xA7\x84\xED\x95\x9C \xEC\xB4\x88\xEB\xA1\x9D",   // 진한 초록
        "\xEC\xB2\xAD\xEB\xA1\x9D",           // 청록
        "\xEB\x82\xA8\xEC\x83\x89",           // 남색
        "\xEC\xA7\x84\xED\x95\x9C \xEB\xB3\xB4\xEB\x9D\xBC",   // 진한 보라
        "\xEB\xB6\x84\xED\x99\x8D",           // 분홍
        "\xED\x9D\x99\xEC\x83\x89",           // 흙색
        "\xEC\xB2\xAD\xEC\x9E\x90",           // 청자
        "\xEC\x97\xB0\xEB\xB3\xB4\xEB\x9D\xBC", // 연보라
        "\xEC\x9D\x80\xEC\x83\x89" };         // 은색

    if (index < 1 || index > numColors)
        return "";

    return names[index - 1];
}

} // namespace gocue::CueColors
