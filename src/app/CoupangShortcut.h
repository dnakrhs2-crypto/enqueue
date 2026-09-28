#pragma once

#include <juce_core/juce_core.h>

namespace gocue::CoupangShortcut
{
inline const auto buttonText = juce::String::fromUTF8 ("쿠팡 바로가기 추가");
inline const auto guidance = juce::String::fromUTF8 ("[쿠팡 바로가기 추가]를 누르면 바탕화면에 쿠팡 바로가기를 만듭니다.");
inline const auto disclosure = juce::String::fromUTF8 ("(이 바로가기는 쿠팡파트너스 활동의 일환입니다.)");

bool existsOn (const juce::File& desktop);
juce::Result createOn (const juce::File& desktop, const juce::File& iconFile);
juce::File userDesktop();
juce::File installedIcon();

struct UpdateDecision
{
    bool announce = false;
    bool offerShortcut = false;
};

// Only Tally passes existingSettingsWithoutVersion: releases before version tracking
// can be recognised by their settings file. A fresh installation remains quiet.
UpdateDecision decideUpdate (const juce::String& previous, const juce::String& current,
                             bool shortcutExists, bool existingSettingsWithoutVersion = false);
}
