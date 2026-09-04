#pragma once

#include <juce_graphics/juce_graphics.h>

namespace gocue::livemix::Palette
{

// the "다크 콘솔" mock (gom, 2026-09-04): every colour of the app lives here
const juce::Colour background   { 0xff15171b };
const juce::Colour bar          { 0xff1b1e24 };   // top bar, status bar, drawers
const juce::Colour card         { 0xff1f2228 };
const juce::Colour card2        { 0xff272b32 };   // fields, buttons, chips
const juce::Colour masterCard   { 0xff232733 };
const juce::Colour line         { 0xff2f343c };
const juce::Colour text         { 0xfff1f2f4 };
const juce::Colour dimText      { 0xff98a0ab };
const juce::Colour accent       { 0xff4c8dff };
const juce::Colour lampOn       { 0xff35d07f };
const juce::Colour lampOff      { 0xff3a3f47 };
const juce::Colour meterBg      { 0xff111317 };
const juce::Colour slotBg       { 0xff2a2e36 };
const juce::Colour slotLine     { 0xff3a404a };
const juce::Colour brand        { 0xffe5302d };   // the red ON tile
const juce::Colour danger       { 0xffff5a5f };
const juce::Colour meterGreen   { 0xff22c55e };
const juce::Colour meterYellow  { 0xffeab308 };
const juce::Colour meterRed     { 0xffef4444 };

constexpr float cardRadius = 14.0f;
constexpr float controlRadius = 9.0f;

} // namespace gocue::livemix::Palette

namespace gocue::livemix
{
/** Every text size in the app goes through this: gom (2026-09-04) wanted the whole UI's text larger. */
constexpr float textScale = 1.2f;
inline float pt (float size) noexcept { return size * textScale; }
} // namespace gocue::livemix
